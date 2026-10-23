#ifndef MULTICAM_H
#define MULTICAM_H

#include <stdint.h>
#include <stddef.h>

#define MULTICAM_MAX_TILES 8
#define MULTICAM_MAX_PATH  512

typedef struct {
    char id[64];
    int tile;
    int crop_x;           /* 水平裁切起点（0..src_w-取景宽，-1 表示自动居中） */
    int enabled;
    int external;         /* 供外部播放：go2rtc 实例照跑但本插件不拉流（厂商
                             RTSP 输出为单观看者，让出唯一观看位给外部播放器），
                             该路置灰暂停检测 */
    char source[256];
} multicam_camera_t;

/* 检测区域不再存插件配置：设备侧 zone profile 是唯一事实源
   （总览界面绘制，GET /api/zones?input=bitstream）；面板只读叠加。 */

typedef struct {
    int layout_version;
    char layout_type[32]; /* "crop_1x2"（默认推荐左右并排裁切）或旧版 "1x2" / "2x1" */
    int layout_cols;      /* layout.type "行x列" 的列数；0 = 单行横排（兼容旧配置） */
    int squeeze_pct;      /* 横向压缩系数 ×100（crop_1x2：每路取景窗口宽 = 400×系数。
                             >100 时更宽的源窗口被压进 400 宽 tile——画面横向变瘦、
                             实际覆盖更宽；画布几何 800×480 不变。100..250） */
    int tile_w, tile_h;   /* crop_1x2 下为 400x480，旧版由实际流决定 */
    int canvas_w, canvas_h; /* crop_1x2 下为 800x480 (5:3) */
    int fps;
    char data_dir[256];   /* go2rtc yaml/socket/日志的根 */
    char g2r_bin[256];    /* go2rtc 二进制 */
    char mh_yaml[256];    /* mhcamera go2rtc.yaml（token 来源） */
    int base_port;        /* 每路实例 RTSP 端口 = base_port + 序号 */
    multicam_camera_t cameras[MULTICAM_MAX_TILES];
    int camera_count;
    int window_enabled;    /* GOP 窗口解码总开关（capture.window），默认开 */
    int window_frames;     /* 每 GOP 连解帧数（capture.window_frames），默认 4 */
} multicam_config_t;

int config_load(multicam_config_t *cfg, const char *path);

#endif
