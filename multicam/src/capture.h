#ifndef MULTICAM_CAPTURE_H
#define MULTICAM_CAPTURE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* 每路摄像头一个捕获线程：RTSP(TCP) → 解码(hevc/h264 软解) → 最新 YUV420P 帧。
   槽位分辨率按首帧上锁（运行期分辨率变化只丢帧计数，不重新分配），
   防止快照拷贝越界。消费方通过 capture_paint_tile 原子地把最近一帧
   按行距直接拷进画布 tile 区域。 */

typedef struct {
    /* YUV420P 连续布局：Y(w*h) U(w*h/4) V(w*h/4) */
    uint8_t *data;
    int w, h;
    int64_t updated_ms;     /* CLOCK_MONOTONIC 毫秒；0 = 尚未有帧 */
    uint64_t frame_count;
} capture_slot_t;

typedef struct {
    char url[512];          /* rtsp://user:pass@host:port/stream */
    capture_slot_t slot;
    pthread_mutex_t lock;
    volatile bool stop;
    pthread_t thread;
    bool thread_started;
    uint64_t reconnects;
    uint64_t size_drops;    /* 分辨率与槽位锁定值不符被丢弃的帧数 */
    uint64_t pixfmt_drops;  /* 非 YUV420P/yuvj420p 像素格式被丢弃的帧数 */
    int warned_fmt;         /* 上次已告警的像素格式（每种只告警一次） */
    char last_error[160];
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
} capture_t;

int capture_start(capture_t *cap, const char *url);
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

#endif
