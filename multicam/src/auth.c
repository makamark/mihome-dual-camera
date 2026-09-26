/* 原生米家账号短信授权：1:1 复刻 mhcamera（MIT, wade-hello）的授权链路。
   - vendor/xiaomi_api_client.c：请求构造/HTTP 解析/响应状态机/错误分类（原样）
   - 本文件：等价其 service.c 的 auth 编排（sms_required/authenticated/error
     分类）+ process/store 的授权实例生命周期（无 token 起实例、yaml 带
     api.allow_paths、verify 后 token 落授权实例 yaml → 迁移自持快照）。
   bridge（panel.c）：auth.session / auth.status / auth.clear。 */
#define _GNU_SOURCE
#include "auth.h"
#include "g2r.h"
#include "vendor/cJSON.h"
#include "vendor/xiaomi_api_client.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define AUTH_PORT 8559        /* 与 cam 实例（8554+i，i≤5）错开 */
#define AUTH_INDEX 9          /* tmp/go2rtc-p9 命名隔离 */
#define AUTH_IDLE_STOP_MS (10u * 60u * 1000u)
/* 参考实现 XC_XIAOMI_DEFAULT_API_TIMEOUT_MS = 20000 */
#define AUTH_API_TIMEOUT_MS 20000u

static pid_t g_auth_pid = 0;
static char g_auth_bin_tmp[512];   /* 补丁副本（停止时清理） */
static int64_t g_last_used_ms = 0;
static pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;

/* 授权会话状态（对应 service.c 的 sms_target/code_length；bridge 线程串行访问） */
static struct {
    char state_text[32];       /* idle / sms_required / authenticated */
    char masked_target[96];
    unsigned code_length;
    unsigned retry_after_seconds;
    char calling_code[12];     /* 上次 start 的手机号（会话观测） */
    char national_number[24];
} g_auth_state = { .state_text = "idle" };

/* 本地发码限流窗口（对齐 xc_set_rate_deadline，monotonic 截止） */
static int64_t g_rate_until_ms = 0;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void paths(const char *data_dir, char *sock, size_t sock_cap,
                  char *yaml, size_t yaml_cap, char *logp, size_t log_cap)
{
    if (sock) snprintf(sock, sock_cap, "%s/tmp/g2r-auth.sock", data_dir);
    if (yaml) snprintf(yaml, yaml_cap, "%s/go2rtc-auth.yaml", data_dir);
    if (logp) snprintf(logp, log_cap, "%s/tmp/g2r-auth.log", data_dir);
}

int auth_service_start(const char *data_dir, const char *g2r_bin,
                       char *err, size_t err_cap)
{
    char sock[512], yaml[512], logp[512];
    paths(data_dir, sock, sizeof(sock), yaml, sizeof(yaml), logp, sizeof(logp));
    int rc = -1;

    pthread_mutex_lock(&g_auth_lock);
    g_last_used_ms = now_ms();
    if (g_auth_pid > 0) {
        int status;
        if (waitpid(g_auth_pid, &status, WNOHANG) == g_auth_pid) {
            /* 已死且收割成功（僵尸）——kill(pid,0) 对僵尸也返回 0，
               不收割会误判"在跑"，verify 时 connect refused */
            fprintf(stderr, "multicam: 授权实例已退出(status=%d)，重新拉起\n", status);
            g_auth_pid = 0;
        } else if (kill(g_auth_pid, 0) == 0) {
            pthread_mutex_unlock(&g_auth_lock);
            return 0; /* 已在跑 */
        } else {
            g_auth_pid = 0;
        }
    }

    /* 授权实例 yaml：无 token 也能起（xiaomi: 空段），api 白名单放行
       xiaomi-phone（照 mhcamera store.c xc_config_ensure 的生成格式） */
    FILE *fp = fopen(yaml, "w");
    if (!fp) {
        pthread_mutex_unlock(&g_auth_lock);
        snprintf(err, err_cap, "写 %s 失败: %s", yaml, strerror(errno));
        return -1;
    }
    fprintf(fp,
            "api:\n  listen: \"\"\n  unix_listen: %s\n"
            "  allow_paths:\n    - /api/xiaomi\n    - /api/camera/source\n"
            "    - /api/xiaomi-phone\n    - /api/camera/rtsp\n"
            "xiaomi:\n", sock);
    fclose(fp);
    chmod(yaml, 0600);

    snprintf(g_auth_bin_tmp, sizeof(g_auth_bin_tmp), "%s", g2r_bin);
    unlink(sock);

    /* GOMEMLIMIT/GOGC/GOMAXPROCS 由 main() 进程级设置，子进程继承 */
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        int log = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log >= 0) { dup2(log, 1); dup2(log, 2); close(log); }
        setsid();
        execl(g_auth_bin_tmp, g_auth_bin_tmp, "-config", yaml, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        pthread_mutex_unlock(&g_auth_lock);
        snprintf(err, err_cap, "fork 失败");
        return -1;
    }
    g_auth_pid = pid;
    pthread_mutex_unlock(&g_auth_lock);

    /* 等 api 就绪：用 vendor 的 wait_ready（GET /api/xiaomi 轮询，deadline 管理） */
    struct xc_xiaomi_api_client client;
    if (xc_xiaomi_api_client_init(&client, sock, AUTH_API_TIMEOUT_MS) != 0) {
        snprintf(err, err_cap, "api client init 失败");
        goto out;
    }
    if (xc_xiaomi_api_wait_ready(&client, 25000u, NULL, NULL) != 0) {
        snprintf(err, err_cap, "授权服务未就绪（看 %s）", logp);
        goto out;
    }
    rc = 0;
out:
    if (rc != 0) auth_service_stop();
    return rc;
}

void auth_service_stop(void)
{
    pthread_mutex_lock(&g_auth_lock);
    if (g_auth_pid > 0) {
        kill(g_auth_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int status;
            if (waitpid(g_auth_pid, &status, WNOHANG) > 0) break;
            usleep(100 * 1000);
        }
        kill(g_auth_pid, SIGKILL);
        for (int i = 0; i < 20; i++) {
            int status;
            pid_t w = waitpid(g_auth_pid, &status, WNOHANG);
            if (w > 0 || (w < 0 && errno == ECHILD)) break;
            usleep(50 * 1000);
        }
        g_auth_pid = 0;
    }
    if (g_auth_bin_tmp[0] && strstr(g_auth_bin_tmp, "/tmp/go2rtc-p"))
        unlink(g_auth_bin_tmp);
    pthread_mutex_unlock(&g_auth_lock);
}

/* phone 动作（start/verify/resend/cancel）。返回 0 且更新 g_auth_state；
   失败 -1，err 填充 mhcamera 同款错误分类信息。 */
int auth_phone(const char *data_dir, const char *g2r_bin,
               const char *action, const char *calling_code,
               const char *national_number, const char *code,
               char *err, size_t err_cap)
{
    char sock[512], yaml[512];
    paths(data_dir, sock, sizeof(sock), yaml, sizeof(yaml), NULL, 0);
    struct xc_xiaomi_request request = {0};
    struct xc_xiaomi_response response;
    struct xc_xiaomi_api_client client;

    if (auth_service_start(data_dir, g2r_bin, err, err_cap) != 0) return -1;
    if (xc_xiaomi_api_client_init(&client, sock, AUTH_API_TIMEOUT_MS) != 0) {
        snprintf(err, err_cap, "api client init 失败");
        return -1;
    }

    if (!strcmp(action, "start")) {
        request.action = XC_XIAOMI_PHONE_START;
        request.calling_code = calling_code;
        request.national_number = national_number;
    } else if (!strcmp(action, "verify")) {
        request.action = XC_XIAOMI_PHONE_VERIFY;
        request.code = code;
    } else if (!strcmp(action, "resend")) {
        request.action = XC_XIAOMI_PHONE_RESEND;
    } else if (!strcmp(action, "cancel")) {
        request.action = XC_XIAOMI_PHONE_CANCEL;
    } else {
        snprintf(err, err_cap, "未知 action");
        return -1;
    }

    /* 本地限流（对齐 xc_rate_active_locked）：start/resend 在上次发码的
       retry_after 窗口内直接本地拒绝，不打到 vendor */
    if ((request.action == XC_XIAOMI_PHONE_START ||
         request.action == XC_XIAOMI_PHONE_RESEND) &&
        now_ms() < g_rate_until_ms) {
        snprintf(err, err_cap, "rate_limit: sms_retry_later");
        return -1;
    }

    pthread_mutex_lock(&g_auth_lock);
    g_last_used_ms = now_ms();
    pthread_mutex_unlock(&g_auth_lock);

    /* 不做实例重启重试（严格对照 mhcamera：go2rtc 常驻、会话在其进程内，
       verify/resend 打到重启后的新实例只会 input；死了就报网络错） */
    if (xc_xiaomi_api_call(&client, &request, &response) != 0) {
        /* 网络层失败分类对齐 xc_call */
        if (errno == EPROTO || errno == EOVERFLOW || errno == EMSGSIZE)
            snprintf(err, err_cap, "protocol: xiaomi_response_invalid");
        else if (errno == ENOMEM)
            snprintf(err, err_cap, "internal: out_of_memory");
        else
            snprintf(err, err_cap, "network: xiaomi_api_unavailable");
        return -1;
    }

    if (response.phone_state[0]) {
        if (!strcmp(response.phone_state, "error")) {
            /* mhcamera service.c 的错误分类（rate_limit/input/protocol/...） */
            const char *category = "provider";
            if (!strcmp(response.phone_error, "sms_send_limit_tomorrow") ||
                !strcmp(response.phone_error, "rate_limit"))
                category = "rate_limit";
            else if (!strcmp(response.phone_error, "input") ||
                     !strcmp(response.phone_error, "phone_account_not_found"))
                category = "input";
            else if (!strcmp(response.phone_error, "network") ||
                     !strcmp(response.phone_error, "protocol") ||
                     !strcmp(response.phone_error, "internal") ||
                     !strcmp(response.phone_error,
                             "additional_verification_required"))
                category = response.phone_error;
            else if (strstr(response.phone_error, "ticket_") ||
                     strstr(response.phone_error, "passport_") ||
                     strstr(response.phone_error, "xiaomi_token_") ||
                     !strcmp(response.phone_error, "phone_info_response_invalid"))
                category = "protocol";
            snprintf(err, err_cap, "%s: %s", category, response.phone_error);
            if (response.has_phone_provider_code)
                snprintf(err + strlen(err), err_cap - strlen(err),
                         " (code=%d)", response.phone_provider_code);
            xc_xiaomi_response_clear(&response);
            return -1;
        }
        if (!strcmp(response.phone_state, "sms_required")) {
            snprintf(g_auth_state.state_text, sizeof(g_auth_state.state_text),
                     "sms_required");
            snprintf(g_auth_state.masked_target,
                     sizeof(g_auth_state.masked_target), "%s",
                     response.phone_masked_target);
            g_auth_state.code_length = response.phone_code_length;
            g_auth_state.retry_after_seconds = response.phone_retry_after_seconds;
            /* 限流窗口对齐 xc_set_rate_deadline */
            g_rate_until_ms = now_ms() +
                (int64_t)response.phone_retry_after_seconds * 1000;
            if (request.action == XC_XIAOMI_PHONE_START) {
                snprintf(g_auth_state.calling_code,
                         sizeof(g_auth_state.calling_code), "%s",
                         calling_code ? calling_code : "");
                snprintf(g_auth_state.national_number,
                         sizeof(g_auth_state.national_number), "%s",
                         national_number ? national_number : "");
            }
            xc_xiaomi_response_clear(&response);
            return 0;
        }
        if (!strcmp(response.phone_state, "authenticated")) {
            /* 对齐 mhcamera：verify 成功后补一次 ACCOUNTS 拉取确认
               （needs_accounts），账号列表非空 token 才算落住 */
            struct xc_xiaomi_request acc = {.action = XC_XIAOMI_ACCOUNTS};
            struct xc_xiaomi_response acc_resp;
            bool accounts_ok = false;
            if (xc_xiaomi_api_call(&client, &acc, &acc_resp) == 0) {
                accounts_ok = acc_resp.account_count > 0;
                xc_xiaomi_response_clear(&acc_resp);
            }
            if (!accounts_ok) {
                snprintf(err, err_cap, "protocol: xiaomi_response_invalid");
                xc_xiaomi_response_clear(&response);
                return -1;
            }
            snprintf(g_auth_state.state_text, sizeof(g_auth_state.state_text),
                     "authenticated");
            xc_xiaomi_response_clear(&response);
            return 0;
        }
        snprintf(g_auth_state.state_text, sizeof(g_auth_state.state_text),
                 "%s", response.phone_state);   /* idle 等 */
        xc_xiaomi_response_clear(&response);
        return 0;
    }

    xc_xiaomi_response_clear(&response);
    snprintf(err, err_cap, "protocol: xiaomi_response_invalid");
    return -1;
}

const char *auth_state_text(void) { return g_auth_state.state_text; }
const char *auth_masked_target(void) { return g_auth_state.masked_target; }
unsigned auth_code_length(void) { return g_auth_state.code_length; }
unsigned auth_retry_after(void) { return g_auth_state.retry_after_seconds; }

int auth_migrate_token(const char *data_dir, char *err, size_t err_cap)
{
    char yaml[512];
    paths(data_dir, NULL, 0, yaml, sizeof(yaml), NULL, 0);
    /* vendor 把 token 原子写回授权实例自身 yaml；抽出来落自持快照 */
    if (g2r_snapshot_tokens(data_dir, yaml) != 0) {
        snprintf(err, err_cap, "授权实例 yaml 无 xiaomi 段（迁移失败）");
        return -1;
    }
    return 0;
}

int auth_authorized(const char *data_dir, const char *mh_yaml)
{
    char tokens[2048];
    return g2r_extract_tokens(data_dir, -1, mh_yaml,
                              tokens, sizeof(tokens), NULL, 0) == 0;
}

void auth_idle_tick(void)
{
    pthread_mutex_lock(&g_auth_lock);
    bool idle = g_auth_pid > 0 && g_last_used_ms &&
                now_ms() - g_last_used_ms > AUTH_IDLE_STOP_MS;
    pthread_mutex_unlock(&g_auth_lock);
    /* SMS 会话进行中不回收（等码期间实例死了 verify 会失败） */
    if (idle && strcmp(g_auth_state.state_text, "sms_required") != 0) {
        fprintf(stderr, "multicam: 授权实例空闲回收\n");
        auth_service_stop();
    }
}
