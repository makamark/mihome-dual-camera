#ifndef MULTICAM_AUTH_H
#define MULTICAM_AUTH_H

#include <stdbool.h>
#include <stddef.h>

/* 原生米家账号短信授权（1:1 复刻 mhcamera 开源实现，MIT，见 vendor/）。
   bridge 方法（panel.c 路由）：auth.session / auth.status / auth.clear。
   SMS 会话保存在授权实例（go2rtc）进程内存里：start 成功后实例若死亡，
   验证码即作废——verify 必须命中同一实例。 */

/* 确保授权实例在跑（无 token 也能起）。0 成功。 */
int auth_service_start(const char *data_dir, const char *g2r_bin, char *err, size_t err_cap);

/* 停掉授权实例（verify 成功/取消/已授权后由 tick 调用）。 */
void auth_service_stop(void);

/* phone 动作（start/verify/resend/cancel；start 需 calling_code+national_number，
   verify 需 code）。0 成功（会话状态经下方 getter 读取）；失败 -1 填 err
   （mhcamera 同款 category: message 分类）。
   0.6.1：verify 遇实例死过（会话丢失）直接报 session:sms_session_lost；
   resend 遇会话丢失用存储的手机号透明重新 start（发新码）。 */
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

/* 主循环接线：缓存路径 + 周期 tick。0.6.1 起未授权期间授权实例常驻
   （pids 配额紧张时按需拉起的实例会在 Go runtime 惰性建线程的瞬间撞配额
   无声死亡，等码会话随之作废；常驻让建线程发生在插件启动、配额最宽裕时）。 */
void auth_service_configure(const char *data_dir, const char *g2r_bin,
                            const char *mh_yaml);
void auth_service_tick(void);

/* 取证/观测（auth.status 用）。alive=实例在跑；spawns=成功拉起代际数；
   last_exit_status/last_term_sig=最近一次死亡（-1 未知）；session_valid=
   等 code 会话仍在原实例内（实例死过即 false）。 */
bool auth_service_alive(void);
unsigned auth_spawns(void);
int auth_last_exit_status(void);
int auth_last_term_sig(void);
bool auth_session_valid(void);
/* 授权短信会话进行中（catalog 发现实例让路，防线程配额竞争）。 */
bool auth_sms_active(void);

#endif
