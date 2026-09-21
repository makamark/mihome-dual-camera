#define _GNU_SOURCE
#include "g2r.h"
#include "httputil.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void g2r_gen_credentials(char *user, size_t user_cap, char *pass, size_t pass_cap)
{
    /* 实测规则：用户名只接受小写字母（"viewer" 为验证通过值），
       密码需含大写+小写+数字的 8 位；启用状态下不可改凭证 */
    static const char up[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    static const char low[] = "abcdefghijkmnpqrstuvwxyz";
    static const char dig[] = "23456789";
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    snprintf(user, user_cap, "viewer");
    (void)user_cap;
    size_t i = 0;
    pass[i++] = up[(seed >> 5) % (sizeof(up) - 1)];
    pass[i++] = low[(seed >> 11) % (sizeof(low) - 1)];
    pass[i++] = dig[(seed >> 17) % (sizeof(dig) - 1)];
    while (i < 8 && i + 1 < pass_cap) {
        seed = seed * 1103515245u + 12345u;
        pass[i++] = low[(seed >> 16) % (sizeof(low) - 1)];
    }
    pass[i] = 0;
}

/* 从 mhcamera 的 yaml 提取 xiaomi: 段（缩进两行的 token 映射）。 */
static int extract_token_section(const char *yaml_path, char *out, size_t cap)
{
    FILE *fp = fopen(yaml_path, "r");
    if (!fp) return -1;
    char line[1024];
    bool in_section = false;
    size_t used = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, "xiaomi:", 7)) { in_section = true; continue; }
        if (in_section) {
            if (line[0] == ' ' || line[0] == '\t') {
                size_t l = strlen(line);
                if (used + l + 1 > cap) break;
                memcpy(out + used, line, l + 1);
                used += l;
            } else {
                break;
            }
        }
    }
    fclose(fp);
    if (used == 0) return -1;
    return 0;
}

/* token 快照原子落盘：此后 mhcamera 不在场也能拨号。 */
static void write_token_snapshot(const char *path, const char *tokens)
{
    char tmp[540];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "xiaomi:\n%s", tokens);
    fclose(f);
    chmod(tmp, 0600);
    if (rename(tmp, path) != 0) unlink(tmp);
}

int g2r_extract_tokens(const char *data_dir, int index, const char *mh_yaml,
                       char *tokens, size_t cap, char *src_out, size_t src_cap)
{
    char own[512], snap[512];
    if (index >= 0) {
        snprintf(own, sizeof(own), "%s/go2rtc-%d.yaml", data_dir, index);
        if (extract_token_section(own, tokens, cap) == 0) {
            if (src_out) snprintf(src_out, src_cap, "自身实例 yaml（自续）");
            return 0;
        }
    }
    snprintf(snap, sizeof(snap), "%s/xiaomi-token.yaml", data_dir);
    if (extract_token_section(snap, tokens, cap) == 0) {
        if (src_out) snprintf(src_out, src_cap, "授权快照");
        return 0;
    }
    if (extract_token_section(mh_yaml, tokens, cap) == 0) {
        if (src_out) snprintf(src_out, src_cap, "mhcamera yaml（已迁移快照）");
        write_token_snapshot(snap, tokens);
        return 0;
    }
    return -1;
}

/* 从任意 yaml（如授权实例的 go2rtc-auth.yaml）抽 xiaomi: 段并落快照。
   原生授权 verify 成功后用它把 token 迁移进自持链。 */
int g2r_snapshot_tokens(const char *data_dir, const char *yaml_path)
{
    char tokens[2048], snap[512];
    if (extract_token_section(yaml_path, tokens, sizeof(tokens)) != 0) return -1;
    snprintf(snap, sizeof(snap), "%s/xiaomi-token.yaml", data_dir);
    write_token_snapshot(snap, tokens);
    return 0;
}

static bool json_field_true(const char *json, const char *field)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":true", field);
    return strstr(json, pat) != NULL;
}

/* 简易取 JSON 字符串字段值（vendor 响应紧凑无空格）。 */
static bool json_str(const char *json, const char *field, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", field);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
    out[i] = 0;
    return true;
}

/* camera 模块的 RTSP 监听端口是常量字符串 "video:8554"（rodata）。
   逐实例补丁成 video:855{4+i}（最多 6 路：8554..8559），写入 tmp 副本后 exec。
   公开版供 auth 实例（index 9 / 端口 8559）复用。 */
int g2r_make_patched_bin(const char *g2r_bin, int port, const char *data_dir,
                         int index, char *tmp_out, size_t tmp_cap,
                         char *err, size_t err_cap)
{
    /* 端口 8554 是原始常量，直接使用原始二进制，无需复制！
       避免在仅 32MB 的 /data 分区重复写入 8.3MB 大文件导致存储耗尽 */
    if (port == 8554) {
        snprintf(tmp_out, tmp_cap, "%s", g2r_bin);
        return 0;
    }
    char src[512];
    snprintf(src, sizeof(src), "%s", g2r_bin);
    FILE *in = fopen(src, "rb");
    if (!in) { snprintf(err, err_cap, "无法打开 %s", src); return -1; }
    fseek(in, 0, SEEK_END);
    long sz = ftell(in);
    fseek(in, 0, SEEK_SET);
    char *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, in) != (size_t)sz) {
        fclose(in); free(buf);
        snprintf(err, err_cap, "读取 %s 失败", src);
        return -1;
    }
    fclose(in);

    const unsigned char pat[] = "video:8554";
    char repl[16];
    snprintf(repl, sizeof(repl), "video:%d", port);
    int count = 0;
    for (size_t i = 0; i + sizeof(pat) <= (size_t)sz; i++) {
        if (memcmp(buf + i, pat, sizeof(pat) - 1) == 0 && strlen(repl) == sizeof(pat) - 1) {
            memcpy(buf + i, repl, strlen(repl));
            count++;
        }
    }
    char tmp_bin[512];
    snprintf(tmp_bin, sizeof(tmp_bin), "%s/tmp/go2rtc-p%d", data_dir, index);
    unlink(tmp_bin);
    FILE *ou = fopen(tmp_bin, "wb");
    if (!ou) { free(buf); snprintf(err, err_cap, "写补丁副本失败: %s", strerror(errno)); return -1; }
    if (fwrite(buf, 1, (size_t)sz, ou) != (size_t)sz) {
        fclose(ou); free(buf); unlink(tmp_bin);
        snprintf(err, err_cap, "补丁副本写入不完整（磁盘满？）");
        return -1;
    }
    fclose(ou);
    free(buf);
    chmod(tmp_bin, 0755);
    if (count == 0) {
        /* 兼容未来修复了端口硬编码的版本：原样使用 */
        snprintf(tmp_bin, sizeof(tmp_bin), "%s", g2r_bin);
    }
    snprintf(tmp_out, tmp_cap, "%s", tmp_bin);
    return 0;
}

static int make_patched_copy(g2r_instance_t *inst, char *err, size_t err_cap)
{
    return g2r_make_patched_bin(inst->g2r_bin, inst->port, inst->data_dir,
                                inst->index, inst->tmp_bin, sizeof(inst->tmp_bin),
                                err, err_cap);
}

/* 扫 /proc 杀掉匹配槽位的残留 go2rtc 实例。
   index=-1 清全部槽位（仅限 app 启动时，清上一代孤儿）；
   指定 index 只清该槽位（g2r_start 内用，绝不碰其他在跑实例）。 */
static void kill_stale_slot(int index)
{
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *e;
    char path[64], buf[1024], name[32], yname[32];
    snprintf(name, sizeof(name), "go2rtc-p%d", index);
    snprintf(yname, sizeof(yname), "go2rtc-%d.yaml", index);
    pid_t me = getpid();
    while ((e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = 0;
        bool hit;
        if (index >= 0)
            hit = strstr(buf, name) && strstr(buf, yname);
        else
            hit = strstr(buf, "go2rtc-p") && strstr(buf, "go2rtc-") && strstr(buf, "-config");
        if (hit) {
            pid_t pid = (pid_t)atoi(e->d_name);
            if (pid > 1 && pid != me) kill(pid, SIGKILL);
        }
    }
    closedir(d);
    usleep(200 * 1000);
}

void g2r_kill_stale(void)
{
    kill_stale_slot(-1);
}

int g2r_start(g2r_instance_t *inst, char *err, size_t err_cap)
{
    char sock[512], logp[512], yaml[1024];
    snprintf(sock, sizeof(sock), "%s/tmp/g2r-%d.sock", inst->data_dir, inst->index);
    snprintf(logp, sizeof(logp), "%s/tmp/g2r-%d.log", inst->data_dir, inst->index);
    snprintf(yaml, sizeof(yaml), "%s/go2rtc-%d.yaml", inst->data_dir, inst->index);

    snprintf(err, err_cap, "ok");

    /* 凭证只在首次生成；重建实例必须复用（启用态不可改凭证） */
    if (!inst->username[0])
        g2r_gen_credentials(inst->username, sizeof(inst->username),
                            inst->password, sizeof(inst->password));

    /* 只清本槽位的残留（其他槽位的在跑实例绝不能碰） */
    kill_stale_slot(inst->index);
    unlink(sock);

    char tokens[2048];
    char tsrc[96] = "";
    if (g2r_extract_tokens(inst->data_dir, inst->index, inst->mh_yaml,
                           tokens, sizeof(tokens), tsrc, sizeof(tsrc)) != 0) {
        snprintf(err, err_cap, "无可用 token（先在面板完成账号授权）");
        return -1;
    }
    fprintf(stderr, "multicam: g2r[%d] token 源：%s\n", inst->index, tsrc);

    char patched[512];
    if (make_patched_copy(inst, patched, sizeof(patched)) != 0) {
        snprintf(err, err_cap, "make_patched_copy 失败: %s", patched);
        return -1;
    }

    FILE *fp = fopen(yaml, "w");
    if (!fp) { snprintf(err, err_cap, "写 %s 失败: %s", yaml, strerror(errno)); return -1; }
    fprintf(fp, "api:\n  listen: \"\"\n  unix_listen: %s\nrtsp:\n  listen: \":%d\"\nxiaomi:\n  %s",
            sock, inst->port, tokens);
    fclose(fp);
    chmod(yaml, 0600);

    /* Go 运行时内存上限（CPU 有余量，GC 频率换堆：8MiB×2 实例）。
       GOMAXPROCS=1：单核 A53 上多 P 只会增加 OS 线程需求（sysmon/retake 等），
       曾在系统线程配额收紧时致 newosproc errno=11 崩溃循环。 */
    setenv("GOMEMLIMIT", "8MiB", 1);
    setenv("GOGC", "30", 1);
    setenv("GOMAXPROCS", "1", 1);
    inst->pid = fork();
    unsetenv("GOMEMLIMIT");
    unsetenv("GOGC");
    unsetenv("GOMAXPROCS");
    if (inst->pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* 父进程死后内核收尸，杜绝孤儿占端口 */
        int log = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log >= 0) { dup2(log, 1); dup2(log, 2); close(log); }
        setsid();
        execl(inst->tmp_bin, inst->tmp_bin, "-config", yaml, (char *)NULL);
        _exit(127);
    }
    if (inst->pid < 0) { snprintf(err, err_cap, "fork 失败"); return -1; }

    /* 等 unix socket 就绪（每阶段独立超时，避免多实例累积共享截止时间） */
    char resp[4096];
    int64_t dl_sock = now_ms() + 25000;
    while (now_ms() < dl_sock) {
        struct stat st;
        if (stat(sock, &st) == 0 && http_unix(sock, "GET", "/api/camera/rtsp", NULL, resp, sizeof(resp)) == 0)
            break;
        usleep(200 * 1000);
    }

    /* 下发源并等 running */
    char body[768];
    snprintf(body, sizeof(body), "{\"source\":\"%s\"}", inst->source);
    bool source_ok = false;
    int64_t dl_src = now_ms() + 20000;
    while (now_ms() < dl_src) {
        if (http_unix(sock, "POST", "/api/camera/source", body, resp, sizeof(resp)) == 0) {
            char state[32] = "";
            http_unix(sock, "GET", "/api/camera/source", NULL, resp, sizeof(resp));
            json_str(resp, "state", state, sizeof(state));
            if (!strcmp(state, "running")) { source_ok = true; break; }
        }
        usleep(500 * 1000);
    }
    if (!source_ok) {
        snprintf(err, err_cap, "go2rtc 源未进入 running（看 %s）", logp);
        g2r_stop(inst);
        return -1;
    }

    /* 启用认证 RTSP 输出 */
    snprintf(body, sizeof(body), "{\"enabled\":true,\"username\":\"%s\",\"password\":\"%s\"}",
             inst->username, inst->password);
    bool rtsp_ok = false;
    int64_t dl_rtsp = now_ms() + 20000;
    while (now_ms() < dl_rtsp) {
        if (http_unix(sock, "POST", "/api/camera/rtsp", body, resp, sizeof(resp)) == 0) {
            char post_resp[512];
            snprintf(post_resp, sizeof(post_resp), "%s", resp);
            http_unix(sock, "GET", "/api/camera/rtsp", NULL, resp, sizeof(resp));
            if (json_field_true(resp, "enabled")) { rtsp_ok = true; break; }
            fprintf(stderr, "multicam: g2r[%d] rtsp POST→ %.160s\n",
                    inst->index, post_resp);
        } else {
            fprintf(stderr, "multicam: g2r[%d] rtsp enable http 失败\n", inst->index);
        }
        usleep(500 * 1000);
    }
    if (!rtsp_ok) {
        char last[160];
        snprintf(last, sizeof(last), "%s", err);
        snprintf(err, err_cap, "go2rtc RTSP 输出未启用（看 %s）: %.80s", logp, last);
        g2r_stop(inst);
        return -1;
    }
    inst->running = true;
    fprintf(stderr, "multicam: g2r[%d] rtsp 端口 %d（凭证 %s:****）\n",
            inst->index, inst->port, inst->username);
    return 0;
}

void g2r_stop(g2r_instance_t *inst)
{
    if (inst->pid > 0) {
        kill((pid_t)inst->pid, SIGTERM);
        for (int i = 0; i < 20; i++) {
            int status;
            pid_t w = waitpid((pid_t)inst->pid, &status, WNOHANG);
            if (w > 0) break;
            usleep(100 * 1000);
        }
        kill((pid_t)inst->pid, SIGKILL);
        for (int i = 0; i < 20; i++) {
            int status;
            pid_t w = waitpid((pid_t)inst->pid, &status, WNOHANG);
            if (w > 0 || (w < 0 && errno == ECHILD)) break;
            usleep(50 * 1000);
        }
        inst->pid = 0;
    }
    char sock[512];
    snprintf(sock, sizeof(sock), "%s/tmp/g2r-%d.sock", inst->data_dir, inst->index);
    unlink(sock);
    if (inst->tmp_bin[0] && strcmp(inst->tmp_bin, inst->g2r_bin) != 0)
        unlink(inst->tmp_bin);
    inst->running = false;
}
