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
#include <stdarg.h>
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
#define AUTH_API_TIMEOUT_MS 20000u
/* 0.6.1：未授权期间保活重试退避 */
#define AUTH_SPAWN_RETRY_MIN_MS 3000
#define AUTH_SPAWN_RETRY_MAX_MS 30000u
#define AUTH_TICK_MS 5000     /* tick 节流（auth_authorized 有文件 IO） */

static pid_t g_auth_pid = 0;
static char g_auth_bin_tmp[512];   /* 补丁副本（停止时清理） */
static int64_t g_last_used_ms = 0;
static pthread_mutex_t g_auth_lock = PTHREAD_MUTEX_INITIALIZER;

/* 0.6.1 取证/代际：等 code 会话在实例进程内存里，实例死过即作废 */
static unsigned g_auth_spawns = 0;       /* 成功 fork 计数（每代 +1） */
static unsigned g_session_gen = 0;       /* 最近一次 start 成功时的代际 */
static bool g_session_valid = false;     /* start 成功后置位（会话在实例内存里） */
static int g_last_exit_status = -1;      /* 最近一次死亡 waitpid 原始 status */
static int g_last_term_sig = -1;         /* WIFSIGNALED ? WTERMSIG : -1 */
static char g_auth_data_dir[256];        /* configure 缓存（tick/取证写文件用） */
static char g_auth_g2r_bin[300];
static char g_auth_mh_yaml[300];
static int64_t g_next_spawn_try = 0;
static int64_t g_spawn_backoff = AUTH_SPAWN_RETRY_MIN_MS;

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

/* ---------- 0.6.1 取证：实例死亡原因落盘（SSH 可读，跨重启留存） ---------- */

static void forensics_log(const char *fmt, ...)
{
    if (!g_auth_data_dir[0]) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/tmp/auth-forensics.log", g_auth_data_dir);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 64 * 1024) {
        FILE *f = fopen(path, "w");   /* 超限重置（低频事件，64KB 足够久） */
        if (f) fclose(f);
    }
    FILE *f = fopen(path, "a");
    if (!f) return;
    char ts[40];
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    struct tm tm;
    localtime_r(&t.tv_sec, &tm);
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tm);
    fprintf(f, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* waitpid 收割时记录死亡方式（status 原始值 + 信号解码） */
static void record_exit(int status, const char *why)
{
    g_last_exit_status = status;
    g_last_term_sig = WIFSIGNALED(status) ? WTERMSIG(status) : -1;
    if (WIFSIGNALED(status))
        forensics_log("DEAD gen=%u sig=%d (%s)", g_auth_spawns,
                      WTERMSIG(status), why);
    else
        forensics_log("DEAD gen=%u exit=%d (%s)", g_auth_spawns,
                      WEXITSTATUS(status), why);
    fprintf(stderr, "multicam: 授权实例退出（gen=%u %s: %s sig=%d exit=%d）\n",
            g_auth_spawns, why,
            WIFSIGNALED(status) ? "被信号杀死" : "自行退出",
            WIFSIGNALED(status) ? WTERMSIG(status) : 0,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/* 读日志尾部一行（wait_ready 失败诊断用；newosproc=线程配额撞限） */
static void log_tail(const char *path, char *out, size_t cap)
{
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256], last[256] = "";
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (n) snprintf(last, sizeof(last), "%s", line);
    }
    fclose(f);
    snprintf(out, cap, "%s", last);
}

/* 起 api 就绪等待（不持锁；fork 后调用）。0 成功，-1 填 err。 */
static int wait_api_ready(const char *data_dir, char *err, size_t err_cap)
{
    char sock[512], logp[512];
    paths(data_dir, sock, sizeof(sock), NULL, 0, logp, sizeof(logp));
    struct xc_xiaomi_api_client client;
    if (xc_xiaomi_api_client_init(&client, sock, AUTH_API_TIMEOUT_MS) != 0) {
        snprintf(err, err_cap, "api client init 失败");
        return -1;
    }
    if (xc_xiaomi_api_wait_ready(&client, 25000u, NULL, NULL) != 0) {
        char tail[160];
        log_tail(logp, tail, sizeof(tail));
        if (strstr(tail, "newosproc") || strstr(tail, "OS thread") ||
            strstr(tail, "ulimit"))
            snprintf(err, err_cap, "授权服务启动失败：线程配额不足（系统资源紧张）");
        else if (tail[0])
            snprintf(err, err_cap, "授权服务未就绪（日志：%s）", tail);
        else
            snprintf(err, err_cap, "授权服务未就绪（看 %s）", logp);
        return -1;
    }
    return 0;
}

/* fork 授权实例（调用方持 g_auth_lock）。返回 pid（>0），失败 -1 填 err。 */
static pid_t spawn_one(const char *data_dir, const char *g2r_bin,
                       char *err, size_t err_cap)
{
    char sock[512], yaml[512], logp[512];
    paths(data_dir, sock, sizeof(sock), yaml, sizeof(yaml), logp, sizeof(logp));

    /* 授权实例 yaml：无 token 也能起（xiaomi: 空段），api 白名单放行
       xiaomi-phone（照 mhcamera store.c xc_config_ensure 的生成格式） */
    FILE *fp = fopen(yaml, "w");
    if (!fp) {
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

    /* 授权 yaml 写盘后再打端口补丁：未补丁的二进制 camera 模块 RTSP 监听
       硬编码 8554，与第 1 路实例冲突（token 在场时 lane-0 占 8554）。
       补丁副本落 data_dir/tmp（8.3MB，停实例即删；授权低频，写损可忽略） */
    char patched[512];
    if (g2r_make_patched_bin(g2r_bin, AUTH_PORT, data_dir, AUTH_INDEX,
                             patched, sizeof(patched), err, err_cap) != 0)
        return -1;
    snprintf(g_auth_bin_tmp, sizeof(g_auth_bin_tmp), "%s", patched);
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
        snprintf(err, err_cap, "fork 失败: %s", strerror(errno));
        return -1;
    }
    g_auth_spawns++;
    g_auth_pid = pid;
    forensics_log("SPAWN gen=%u pid=%d", g_auth_spawns, (int)pid);
    return pid;
}

int auth_service_start(const char *data_dir, const char *g2r_bin,
                       char *err, size_t err_cap)
{
    pthread_mutex_lock(&g_auth_lock);
    g_last_used_ms = now_ms();
    if (g_auth_pid > 0) {
        int status;
        if (waitpid(g_auth_pid, &status, WNOHANG) == g_auth_pid) {
            /* 已死且收割成功（僵尸）——kill(pid,0) 对僵尸也返回 0，
               不收割会误判"在跑"，verify 时 connect refused */
            record_exit(status, "start发现");
            g_auth_pid = 0;
        } else if (kill(g_auth_pid, 0) == 0) {
            pthread_mutex_unlock(&g_auth_lock);
            return 0; /* 已在跑 */
        } else {
            g_auth_pid = 0;
        }
    }
    pthread_mutex_unlock(&g_auth_lock);

    /* 线程配额紧张是瞬态：newosproc 类启动失败 3s 后重试一次 */
    for (int attempt = 0; attempt < 2; attempt++) {
        pthread_mutex_lock(&g_auth_lock);
        pid_t pid = spawn_one(data_dir, g2r_bin, err, err_cap);
        pthread_mutex_unlock(&g_auth_lock);
        if (pid <= 0) return -1;
        if (wait_api_ready(data_dir, err, err_cap) == 0) return 0;
        auth_service_stop();
        if (attempt == 0 && strstr(err, "线程配额不足")) {
            fprintf(stderr, "multicam: 授权实例线程配额不足，3s 后重试\n");
            sleep(3);
            continue;
        }
        return -1;
    }
    return -1;
}

void auth_service_stop(void)
{
    pthread_mutex_lock(&g_auth_lock);
    if (g_auth_pid > 0) {
        kill(g_auth_pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int status;
            if (waitpid(g_auth_pid, &status, WNOHANG) > 0) {
                record_exit(status, "stop");
                break;
            }
            usleep(100 * 1000);
        }
        if (g_auth_pid > 0 && kill(g_auth_pid, 0) == 0) {
            kill(g_auth_pid, SIGKILL);
            for (int i = 0; i < 20; i++) {
                int status;
                pid_t w = waitpid(g_auth_pid, &status, WNOHANG);
                if (w > 0) { record_exit(status, "stop-kill"); break; }
                if (w < 0 && errno == ECHILD) break;
                usleep(50 * 1000);
            }
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

    /* 0.6.1 会话语义：等 code 会话在实例进程内存里，实例死过（代际变化）
       即作废。verify 直接诚实报错；resend 用存储手机号透明重新 start。 */
    bool session_lost = g_session_valid && g_auth_spawns != g_session_gen;
    bool restart_start = false;
    if (!strcmp(action, "verify")) {
        if (session_lost) {
            snprintf(err, err_cap,
                     "session: sms_session_lost（授权服务中断过，验证码已失效，请点「重新发送」）");
            return -1;
        }
        request.action = XC_XIAOMI_PHONE_VERIFY;
        request.code = code;
    } else if (!strcmp(action, "resend")) {
        if (session_lost || !g_session_valid) {
            if (!g_auth_state.calling_code[0] || !g_auth_state.national_number[0]) {
                snprintf(err, err_cap,
                         "session: sms_session_lost（请重新输入手机号发送验证码）");
                return -1;
            }
            restart_start = true;   /* 换 new 码：按 START 语义重走 */
        } else {
            request.action = XC_XIAOMI_PHONE_RESEND;
        }
    } else if (!strcmp(action, "start")) {
        /* nothing */
    } else if (!strcmp(action, "cancel")) {
        request.action = XC_XIAOMI_PHONE_CANCEL;
    } else {
        snprintf(err, err_cap, "未知 action");
        return -1;
    }
    if (restart_start || !strcmp(action, "start")) {
        request.action = XC_XIAOMI_PHONE_START;
        request.calling_code = g_auth_state.calling_code;
        request.national_number = g_auth_state.national_number;
    }
    if (request.action == XC_XIAOMI_PHONE_START && !restart_start) {
        request.calling_code = calling_code;
        request.national_number = national_number;
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
                         request.calling_code ? request.calling_code : "");
                snprintf(g_auth_state.national_number,
                         sizeof(g_auth_state.national_number), "%s",
                         request.national_number ? request.national_number : "");
            }
            /* 会话落在当前代际实例内（死过即 session_lost） */
            g_session_valid = true;
            g_session_gen = g_auth_spawns;
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
            g_session_valid = false;   /* 会话已消费 */
            xc_xiaomi_response_clear(&response);
            return 0;
        }
        snprintf(g_auth_state.state_text, sizeof(g_auth_state.state_text),
                 "%s", response.phone_state);   /* idle 等 */
        g_session_valid = false;
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

void auth_service_configure(const char *data_dir, const char *g2r_bin,
                            const char *mh_yaml)
{
    snprintf(g_auth_data_dir, sizeof(g_auth_data_dir), "%s", data_dir);
    snprintf(g_auth_g2r_bin, sizeof(g_auth_g2r_bin), "%s", g2r_bin);
    snprintf(g_auth_mh_yaml, sizeof(g_auth_mh_yaml), "%s",
             mh_yaml && mh_yaml[0] ? mh_yaml : "");
}

/* 主循环周期调用（内部 5s 节流）：未授权保活授权实例（死亡自动重建，
   退避 3→30s）；已授权回收。0.6.1 之前按需拉起的实例会在 Go runtime
   惰性建线程瞬间撞 pids 配额无声死亡，等码会话作废 → verify input。 */
void auth_service_tick(void)
{
    static int64_t last_tick = 0;
    int64_t now = now_ms();
    if (now - last_tick < AUTH_TICK_MS) return;
    last_tick = now;

    if (!g_auth_data_dir[0]) return;   /* 未 configure（selftest 等） */

    if (auth_authorized(g_auth_data_dir, g_auth_mh_yaml)) {
        if (g_auth_pid > 0) {
            fprintf(stderr, "multicam: 已授权，回收授权服务\n");
            auth_service_stop();
        }
        return;
    }

    /* 未授权：保活。先收割检测（静默死亡只有这里发现） */
    pthread_mutex_lock(&g_auth_lock);
    if (g_auth_pid > 0) {
        int status;
        if (waitpid(g_auth_pid, &status, WNOHANG) == g_auth_pid) {
            record_exit(status, "tick发现");
            g_auth_pid = 0;
        } else if (kill(g_auth_pid, 0) == 0) {
            pthread_mutex_unlock(&g_auth_lock);
            return;   /* 在跑 */
        } else {
            g_auth_pid = 0;
        }
    }
    pthread_mutex_unlock(&g_auth_lock);

    if (now < g_next_spawn_try) return;
    char err[192];
    if (auth_service_start(g_auth_data_dir, g_auth_g2r_bin,
                           err, sizeof(err)) == 0) {
        g_spawn_backoff = AUTH_SPAWN_RETRY_MIN_MS;
        g_next_spawn_try = 0;
    } else {
        fprintf(stderr, "multicam: 授权服务保活失败: %s（%lldms 后重试）\n",
                err, (long long)g_spawn_backoff);
        g_next_spawn_try = now + g_spawn_backoff;
        g_spawn_backoff = g_spawn_backoff * 2 > AUTH_SPAWN_RETRY_MAX_MS
                            ? AUTH_SPAWN_RETRY_MAX_MS : g_spawn_backoff * 2;
    }
}

/* ---------- 取证/观测 getter（bridge auth.status） ---------- */

bool auth_service_alive(void)
{
    pthread_mutex_lock(&g_auth_lock);
    bool alive = g_auth_pid > 0 && kill(g_auth_pid, 0) == 0;
    pthread_mutex_unlock(&g_auth_lock);
    return alive;
}

unsigned auth_spawns(void) { return g_auth_spawns; }
int auth_last_exit_status(void) { return g_last_exit_status; }
int auth_last_term_sig(void) { return g_last_term_sig; }
bool auth_session_valid(void)
{
    return g_session_valid && g_auth_spawns == g_session_gen;
}
bool auth_sms_active(void)
{
    return !strcmp(g_auth_state.state_text, "sms_required") &&
           auth_service_alive();
}
