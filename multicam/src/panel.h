#ifndef MULTICAM_PANEL_H
#define MULTICAM_PANEL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Web 面板桥接后端：注册 bridge 模块 "multicam"，方法见 panel.c。
   主循环每秒把快照写入（panel_status_publish），bridge 线程读。 */

typedef struct {
    char cam_id[64];
    bool enabled;         /* 配置启用 */
    bool gray;            /* 当前置灰 */
    int64_t age_ms;       /* 帧龄；<0 无帧 */
    char note[96];        /* 置灰/错误原因 */
    uint64_t reconnects;
    uint64_t decoded;     /* 该路累计解码帧数（抽帧有效性观测） */
    int keyint_ms;        /* 实测 GOP 间隔（窗口解码观测，0=未测量） */
    int g2r_port;
    int g2r_rss_kb;       /* go2rtc 子进程常驻内存（0=不在场） */
    bool g2r_alive;
    int crop_x;           /* 当前水平裁切起点 */
    int src_w, src_h;     /* 源画面原始分辨率 */
} panel_tile_t;

typedef struct {
    bool valid;
    bool pushing;         /* 推流会话活 */
    char layout_type[32]; /* 布局类型，如 "crop_1x2" */
    int canvas_w, canvas_h, fps;
    int tile_w, tile_h;   /* 单 tile 尺寸（面板叠加/标定的几何换算用） */
    int layout_cols;      /* tile 网格列数（1 = 竖排） */
    uint64_t frames;
    int session_failures;
    int n_slots, n_cams;
    panel_tile_t tiles[8];
    time_t started_at;
} panel_status_t;

/* 主线程启动前调用：登记全局停止/重载标志与配置路径。 */
void panel_init(volatile int *stop_flag, volatile int *reload_flag, const char *config_path);

/* 启动 bridge 服务线程（返回 0 成功）。 */
int panel_start(void);

/* 停止并回收 bridge 线程（重载/退出前）。 */
void panel_stop(void);

/* 主循环每秒发布快照。 */
void panel_status_publish(const panel_status_t *st);

/* run() 启动捕获后绑定 tile→捕获映射（bridge preview.full 取景器快照用），
   cap_by_tile[tile] = 捕获下标或 -1；重载/退出前传 NULL 解绑。 */
void panel_bind_captures(void *caps, const int *cap_by_tile, int n);

/* 主循环每帧调用：面板 source.set 的待办换源请求，有则返回 true 并取出。
   src 由调用方提供缓冲（建议 512 字节）。 */
bool panel_take_source_change(int *tile_out, char *src, size_t cap);

#endif
