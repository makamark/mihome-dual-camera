#ifndef MULTICAM_CAPTURE_H
#define MULTICAM_CAPTURE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* 每路摄像头一个捕获线程：RTSP / AIV1 → 解码(hevc/h264/mjpeg 软解) → 最新 YUV420P 帧。
   任意 YUV 像素格式（422/444/NV12/10bit/灰度）入槽时统一转 420P；运行期
   分辨率变化就地重建槽位（读方都在槽位锁内按当时尺寸访问）。消费方经
   capture_paint_fit / capture_paint_tile 原子地把最近一帧写进画布 tile。 */

typedef struct {
    /* YUV420P 连续布局：Y(w*h) U(w*h/4) V(w*h/4) */
    uint8_t *data;
    int w, h;
    int64_t updated_ms;     /* CLOCK_MONOTONIC 毫秒；0 = 尚未有帧 */
    uint64_t frame_count;
} capture_slot_t;

typedef struct {
    char url[512];          /* rtsp://user:pass@host:port/stream（rtsp 直连模式） */
    capture_slot_t slot;
    pthread_mutex_t lock;
    volatile bool stop;
    pthread_t thread;
    bool thread_started;
    uint64_t reconnects;
    uint64_t size_changes;  /* 运行期源分辨率变化次数（槽位重建） */
    uint64_t pixfmt_drops;  /* 无法转换的像素格式被丢弃的帧数 */
    char note[96];          /* 面板可读的无画面原因（认证失败/404/格式不支持…），有帧即清空；锁内读写 */
    volatile bool rtsp_udp; /* rtsp 传输：false=TCP（默认），协商失败自动切换 */
    int warned_fmt;         /* 上次已告警的像素格式（每种只告警一次） */
    char last_error[160];
    /* AIV1 输入模式（米家路）：不经 RTSP 回环，直接消费 go2rtc 实例的
       media socket（unix 域）。免一次 RTSP/TCP 封装-解封装，且不再占用
       实例唯一的 RTSP 观看位——检测与外部播放可并存。 */
    bool aiv1;              /* 输入模式：true = AIV1，false = rtsp 直连 */
    char aiv1_path[108];    /* 实例 media socket 路径（逐实例补丁进二进制） */
    const long *aiv1_peer_pid; /* 实例 pid 指针（实例重建后更新；SO_PEERCRED 校验） */
    volatile int aiv1_fd;   /* 当前连接 fd（stop 时 shutdown 立即唤醒 recv） */
    uint8_t *aiv1_buf;      /* 帧载荷缓冲（AVCC 校验后原地转 Annex B） */
    size_t aiv1_buf_cap;
    /* GOP 窗口解码：源 20fps 但画布只需 GOP 级更新——每个 GOP 从关键帧起
       连解 window_frames 帧，其余帧 AVDISCARD_NONKEY（只解析不重建，近零成本），
       下一个关键帧自动完全恢复。keyint 为实测平滑均值，供置灰阈值联动。 */
    volatile int window_enabled;  /* 总开关（配置 × 标定旁路，主循环写入） */
    int window_frames;            /* 每 GOP 连解帧数（含 I 帧） */
    volatile int window_active;   /* 实际生效（keyint 测量通过后自动激活） */
    int burst_left;               /* 本 GOP 剩余连解预算 */
    int key_count;                /* 已见关键帧数（测量期） */
    volatile int keyint_ms;       /* 实测 GOP 间隔毫秒，0=未知 */
    uint64_t skipped_pkts;        /* 窗口外被跳过的包数 */
    int64_t last_key_ms;          /* 上一个关键帧到达时刻（仅捕获线程访问） */
    /* 取景器 MJPEG 快照（仅 panel bridge 线程访问；void* 避免头文件引 ffmpeg） */
    void *snap_enc;               /* AVCodecContext*（懒初始化常驻） */
    void *snap_frm;               /* AVFrame*（编码输入帧，源宽 >848 时等比缩小） */
    int64_t snap_pts;             /* 快照帧序号 */
} capture_t;

int capture_start(capture_t *cap, const char *url);
/* AIV1 模式启动：peer_pid 指向 go2rtc 实例的 pid（实例重建后值更新，
   每次重连按 SO_PEERCRED 校验，杜绝接到旧实例/外来 socket）。 */
int capture_start_aiv1(capture_t *cap, const char *sock_path, const long *peer_pid);
void capture_stop(capture_t *cap);

/* 主循环每轮调用：enabled=0 恢复全解（标定旁路）；frames=每 GOP 连解帧数 */
void capture_set_window(capture_t *cap, int enabled, int frames);

/* 实测 GOP 间隔毫秒；未知 0。置灰阈值需 > keyint×2 才不闪灰。 */
int64_t capture_keyint_ms(capture_t *cap);

/* 加锁读当前槽位分辨率（首帧等待用）。无帧时 w=h=0。 */
void capture_dims(capture_t *cap, int *w, int *h);

/* 只读帧龄（日志用）：无帧返回 -1。 */
int64_t capture_age(capture_t *cap);

/* 把最近一帧按平面/行距直接拷进画布 tile 区域。
   返回帧龄毫秒；无帧 -1；槽位分辨率与 expect 不符 -2。 */
int64_t capture_paint_tile(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                           int dst_y_stride, int dst_c_stride,
                           int expect_w, int expect_h);

/* 按指定 (crop_x, crop_y, crop_w, crop_h) 从源画面裁切并拷入目标画布平面。
   crop_x/y < 0 时自动居中裁切。各参数及步长须为偶数。
   返回帧龄毫秒；无帧 -1；尺寸不足 -2。 */
int64_t capture_paint_crop(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                           int dst_y_stride, int dst_c_stride,
                           int crop_x, int crop_y,
                           int crop_w, int crop_h);

/* crop_1x2 取景窗口（与面板取景器同一算法）：窗口高 = 源全高，
   窗口宽 = 源高 × out_w/out_h × 压缩系数（取偶，超过源宽则收敛为整幅）。
   848×480 源 1.0× 即 400×480 原样裁切；1080p 源窗口 900×1080 缩放进 tile。 */
int capture_fit_window(int src_w, int src_h, int squeeze_pct, int out_w, int out_h);

/* 按取景窗口把源画面缩放进 out_w×out_h tile（任意分辨率源）：窗口从 crop_x
   起（<0 居中，越界收敛），窗口与 tile 等大时直拷，否则二维双线性缩放。
   src_w/src_h/win_w 可空，回填该帧实际尺寸与窗口宽（面板状态用）。
   返回帧龄毫秒；无帧 -1；参数非法 -2；out_w/out_h 超上限 -3。 */
int64_t capture_paint_fit(capture_t *cap, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                          int dst_y_stride, int dst_c_stride,
                          int crop_x, int squeeze_pct, int out_w, int out_h,
                          int *src_w, int *src_h, int *win_w);

/* 当前无画面原因（capture.note），无则空串。 */
void capture_note(capture_t *cap, char *out, size_t cap_len);

/* 取景器快照：把当前槽位全幅帧编码成一帧 JPEG（面板拖动取景框用；
   源宽 >848 时等比缩小，面板按源尺寸绘制，坐标不受影响）。
   编码器懒初始化常驻（每路一个，仅 bridge 线程串行调用）。
   返回 JPEG 字节数；无帧 -1；输出缓冲不足 -2。 */
int capture_snapshot_jpeg(capture_t *cap, uint8_t *buf, size_t buf_cap);

#endif
