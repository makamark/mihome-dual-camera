#ifndef MULTICAM_AUTH_H
#define MULTICAM_AUTH_H

#include <stddef.h>

/* 原生米家账号短信授权（1:1 复刻 mhcamera 开源实现，MIT，见 vendor/）。
   bridge 方法（panel.c 路由）：auth.session / auth.status / auth.clear。
   bridge 线程串行调用，会话状态无需加锁。 */

/* 确保授权实例在跑（无 token 也能起）。0 成功。 */
int auth_service_start(const char *data_dir, const char *g2r_bin, char *err, size_t err_cap);

/* 停掉授权实例（verify 成功/取消后调用；实例空闲 10 分钟自动停）。 */
void auth_service_stop(void);

/* phone 动作（start/verify/resend/cancel；start 需 calling_code+national_number，
   verify 需 code）。0 成功（会话状态经下方 getter 读取）；失败 -1 填 err
   （mhcamera 同款 category: message 分类）。 */
int auth_phone(const char *data_dir, const char *g2r_bin,
               const char *action, const char *calling_code,
               const char *national_number, const char *code,
               char *err, size_t err_cap);

/* 会话状态（最近一次 phone 动作后）。 */
const char *auth_state_text(void);
const char *auth_masked_target(void);
unsigned auth_code_length(void);
unsigned auth_retry_after(void);

/* verify 成功后的 token 迁移：授权实例 yaml → data_dir/xiaomi-token.yaml。 */
int auth_migrate_token(const char *data_dir, char *err, size_t err_cap);

/* 已授权判定：自持链（快照/实例 yaml）或 mh yaml 任一有 xiaomi 段。 */
int auth_authorized(const char *data_dir, const char *mh_yaml);

/* 空闲回收（主循环周期调用；授权实例只在会话期常驻）。 */
void auth_idle_tick(void);

#endif
