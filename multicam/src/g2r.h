#ifndef MULTICAM_G2R_H
#define MULTICAM_G2R_H

#include <stddef.h>
#include <stdbool.h>

/* 每路摄像头一个 go2rtc 子实例：拉流 + 认证 RTSP 输出。
   token 自持：实例自身 yaml（运行中自续）→ 授权快照 xiaomi-token.yaml →
   mhcamera yaml 回退（授权后首次，命中即落快照）。mhcamera 仅短信授权时
   需要在场（xiaomi-phone 属主门），其余时间停用/不在场均可。 */

typedef struct {
    int index;              /* 实例序号（决定 tmp/g2r-<i> 命名） */
    int port;               /* RTSP 输出端口 */
    char source[512];       /* xiaomi:// 源 URL */
    char data_dir[256];     /* 插件数据目录 */
    char g2r_bin[256];      /* go2rtc 二进制路径 */
    char mh_yaml[256];      /* mhcamera 的 go2rtc.yaml（token 回退源） */
    char tmp_bin[512];      /* 端口补丁副本路径（停止时清理） */
    char username[32];
    char password[32];

    bool running;
    volatile bool restart_pending; /* 源不健康/启动失败，请求 watcher 重建 */
    long pid;
} g2r_instance_t;

/* token 自持抽取（g2r_start / cameras.list 共用）：
   index>=0 时先试自身实例 yaml（自续最新），再试授权快照，最后 mh yaml
   （命中即自动落快照）。返回 0 成功；src_out 可空，回填来源描述。 */
int g2r_extract_tokens(const char *data_dir, int index, const char *mh_yaml,
                       char *tokens, size_t cap, char *src_out, size_t src_cap);

/* 从任意 yaml 抽 xiaomi: 段并落 data_dir/xiaomi-token.yaml 快照
   （原生授权 verify 成功后迁移 token 用）。 */
int g2r_snapshot_tokens(const char *data_dir, const char *yaml_path);

/* go2rtc 二进制端口补丁公开版（auth 实例复用，index=9 命名隔离）。 */
int g2r_make_patched_bin(const char *g2r_bin, int port, const char *data_dir,
                         int index, char *tmp_out, size_t tmp_cap,
                         char *err, size_t err_cap);

/* 启动实例并等待源 running + RTSP enabled；返回 0 成功。
   失败时已尝试清理。err 输出可读原因。 */
int g2r_start(g2r_instance_t *inst, char *err, size_t err_cap);

/* 扫 /proc 杀掉本插件所有残留 go2rtc 实例（跨代孤儿）。 */
void g2r_kill_stale(void);

/* 停止实例（SIGTERM 子进程，清理 socket 与补丁副本）。 */
void g2r_stop(g2r_instance_t *inst);

/* 生成随机凭证（大小写字母+数字，8 位）。 */
void g2r_gen_credentials(char *user, size_t user_cap, char *pass, size_t pass_cap);

#endif
