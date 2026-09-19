#include "catalog.h"
#include "g2r.h"
#include "httputil.h"
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DISC_START_TIMEOUT_MS 25000

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int catalog_ensure_instance(const char *data_dir, const char *mh_yaml,
                            const char *g2r_bin, char *err, size_t err_cap)
{
    char yaml[512], sock[512], logp[512], tokens[2048];
    snprintf(sock, sizeof(sock), "%s/tmp/g2r-disc.sock", data_dir);
    snprintf(logp, sizeof(logp), "%s/tmp/g2r-disc.log", data_dir);
    snprintf(yaml, sizeof(yaml), "%s/go2rtc-disc.yaml", data_dir);

    /* token 自持（disc 实例无常驻实例源，index=-1：快照 → mh 回退） */
    if (g2r_extract_tokens(data_dir, -1, mh_yaml, tokens, sizeof(tokens), NULL, 0) != 0) {
        snprintf(err, err_cap, "无可用 token（先在面板完成账号授权）");
        return -1;
    }

    FILE *ou = fopen(yaml, "w");
    if (!ou) { snprintf(err, err_cap, "写 %s 失败", yaml); return -1; }
    fprintf(ou, "api:\n  unix_listen: %s\nxiaomi:\n  %s", sock, tokens);
    fclose(ou);
    chmod(yaml, 0600);

    struct stat st;
    int running = stat(sock, &st) == 0;
    if (running) {
        char resp[256];
        if (http_unix(sock, "GET", "/api/xiaomi", NULL, resp, sizeof(resp)) == 0)
            return 0; /* 已在运行 */
    }

    /* fork 发现实例（仅 xiaomi 目录模块，无 camera 源；随父进程死 + 内存上限） */
    setenv("GOMEMLIMIT", "16MiB", 1);
    setenv("GOGC", "40", 1);
    pid_t pid = fork();
    unsetenv("GOMEMLIMIT");
    unsetenv("GOGC");
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        setsid();
        int lg = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); close(lg); }
        execl(g2r_bin, g2r_bin, "-config", yaml, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { snprintf(err, err_cap, "fork 失败"); return -1; }

    int64_t deadline = now_ms() + DISC_START_TIMEOUT_MS;
    while (now_ms() < deadline) {
        if (stat(sock, &st) == 0) {
            char resp[256];
            if (http_unix(sock, "GET", "/api/xiaomi", NULL, resp, sizeof(resp)) == 0)
                return 0;
        }
        usleep(200 * 1000);
    }
    snprintf(err, err_cap, "发现实例启动超时（看 %s）", logp);
    return -1;
}

/* 通用设备对象采集：遍历 JSON 树，收集含 id+（model|name）的对象。 */
static void collect_devices(cJSON *node, cJSON *out)
{
    if (!node) return;
    if (cJSON_IsObject(node)) {
        cJSON *id = cJSON_GetObjectItem(node, "id");
        cJSON *model = cJSON_GetObjectItem(node, "model");
        cJSON *name = cJSON_GetObjectItem(node, "name");
        bool has_id = cJSON_IsString(id) || cJSON_IsNumber(id);
        if (has_id && (cJSON_IsString(model) || cJSON_IsString(name))) {
            char did[64] = "";
            if (cJSON_IsString(id)) snprintf(did, sizeof(did), "%s", id->valuestring);
            else if (cJSON_IsNumber(id)) snprintf(did, sizeof(did), "%d", id->valueint);
            /* 去重 */
            cJSON *it;
            bool dup = false;
            cJSON_ArrayForEach(it, out) {
                cJSON *x = cJSON_GetObjectItem(it, "id");
                if (x && cJSON_IsString(x) && !strcmp(x->valuestring, did)) { dup = true; break; }
            }
            if (!dup) {
                cJSON *cp = cJSON_Duplicate(node, 1);
                cJSON_ReplaceItemInObject(cp, "id", cJSON_CreateString(did));
                cJSON_AddItemToArray(out, cp);
            }
        }
        /* 分页字段透传记录到 out 容器外——由调用方另行提取 */
        cJSON *child;
        cJSON_ArrayForEach(child, node) collect_devices(child, out);
    } else if (cJSON_IsArray(node)) {
        cJSON *child;
        cJSON_ArrayForEach(child, node) collect_devices(child, out);
    }
}

/* 提取分页游标：找 has_more=true 且带 next_start_did 的对象。 */
static bool extract_cursor(cJSON *node, char *next, size_t cap, bool *has_more)
{
    if (!node) return false;
    if (cJSON_IsObject(node)) {
        cJSON *hm = cJSON_GetObjectItem(node, "has_more");
        cJSON *ns = cJSON_GetObjectItem(node, "next_start_did");
        if (cJSON_IsBool(hm) && cJSON_IsTrue(hm) && ns) {
            if (cJSON_IsString(ns)) snprintf(next, cap, "%s", ns->valuestring);
            else if (cJSON_IsNumber(ns)) snprintf(next, cap, "%d", ns->valueint);
            *has_more = true;
            return true;
        }
        cJSON *child;
        cJSON_ArrayForEach(child, node) {
            if (extract_cursor(child, next, cap, has_more)) return true;
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child;
        cJSON_ArrayForEach(child, node) {
            if (extract_cursor(child, next, cap, has_more)) return true;
        }
    }
    return false;
}

/* 探测可用的设备列表端点（返回 0 = ep 填充可用端点）。 */
static bool probe_endpoint(const char *sock, const char *account, char *ep, size_t cap,
                           char *resp, size_t resp_cap)
{
    static const char *cands[] = {
        "/api/xiaomi?id=%s&region=cn",       /* select_cameras.py 实测可用 */
        "/api/xiaomi/devices?start_did=%s",
        "/api/xiaomi/%s/devices?start_did=%s",
        "/api/xiaomi/%s/devices",
        "/api/xiaomi/%s",
        "/api/xiaomi/catalog?account_id=%s",
        NULL
    };
    for (int i = 0; cands[i]; i++) {
        char path[256];
        if (strstr(cands[i], "%s") && strstr(cands[i], "%s") != strstr(cands[i], "%s") + 1)
            snprintf(path, sizeof(path), cands[i], account, "");
        else if (strstr(cands[i], "%s"))
            snprintf(path, sizeof(path), cands[i], account);
        else
            snprintf(path, sizeof(path), "%s", cands[i]);
        /* 空设备树不算成功，须包含设备对象——先看响应体积与关键字段 */
        if (http_unix(sock, "GET", path, NULL, resp, resp_cap) != 0) continue;
        cJSON *root = cJSON_Parse(resp);
        if (!root) continue;
        /* 无分页首屏可能为空但有有效结构——记录端点，靠分页与回退补齐；
           这里以"结构可解析"为准，设备为空也接受（账号可能只有少数设备）。 */
        bool has_more = false;
        char next[64] = "";
        extract_cursor(root, next, sizeof(next), &has_more);
        snprintf(ep, cap, "%s", path);
        cJSON_Delete(root);
        return true;
    }
    return false;
}

cJSON *catalog_fetch_devices(const char *sock, char *err, size_t err_cap)
{
    char resp[16384];
    char ep[256] = "";
    char account[64] = "";

    /* 账号列表 */
    if (http_unix(sock, "GET", "/api/xiaomi", NULL, resp, sizeof(resp)) == 0) {
        cJSON *root = cJSON_Parse(resp);
        if (root) {
            cJSON *it;
            cJSON_ArrayForEach(it, root) {
                if (cJSON_IsString(it)) { snprintf(account, sizeof(account), "%s", it->valuestring); break; }
                if (cJSON_IsNumber(it)) { snprintf(account, sizeof(account), "%d", it->valueint); break; }
            }
            cJSON_Delete(root);
        }
    }

    if (!probe_endpoint(sock, account, ep, sizeof(ep), resp, sizeof(resp))) {
        snprintf(err, err_cap, "未探测到可用的设备目录端点（xiami API 路径需在线抓包确认）");
        return NULL;
    }

    cJSON *out = cJSON_CreateArray();
    char start_did[64] = "";
    int pages = 0;
    while (pages < 20) {
        char path[320];
        if (strstr(ep, "start_did=%s") || strstr(ep, "start_did="))
            snprintf(path, sizeof(path), "%s%s%s", ep,
                     strchr(ep, '?') ? "&" : "?", start_did[0] ? start_did : "");
        else
            snprintf(path, sizeof(path), "%s", ep);

        if (http_unix(sock, "GET", path, NULL, resp, sizeof(resp)) != 0) break;
        cJSON *root = cJSON_Parse(resp);
        if (!root) break;
        collect_devices(root, out);
        bool has_more = false;
        char next[64] = "";
        extract_cursor(root, next, sizeof(next), &has_more);
        cJSON_Delete(root);
        if (!has_more || !next[0]) break;
        snprintf(start_did, sizeof(start_did), "%s", next);
        pages++;
    }

    if (cJSON_GetArraySize(out) == 0) {
        snprintf(err, err_cap, "目录为空（端点 %s）", ep);
        cJSON_Delete(out);
        return NULL;
    }
    return out;
}

cJSON *catalog_find_device(cJSON *devices, const char *did)
{
    cJSON *it;
    cJSON_ArrayForEach(it, devices) {
        cJSON *x = cJSON_GetObjectItem(it, "id");
        if (x && cJSON_IsString(x) && !strcmp(x->valuestring, did)) return it;
    }
    return NULL;
}

void catalog_stop_instance(const char *data_dir)
{
    DIR *d = opendir("/proc");
    if (d) {
        struct dirent *e;
        char path[64], buf[1024];
        while ((e = readdir(d))) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
            snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
            int fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = 0;
            if (strstr(buf, "go2rtc") && strstr(buf, "go2rtc-disc.yaml")) {
                pid_t pid = (pid_t)atoi(e->d_name);
                if (pid > 1) kill(pid, SIGKILL);
            }
        }
        closedir(d);
    }
    char sock[512];
    snprintf(sock, sizeof(sock), "%s/tmp/g2r-disc.sock", data_dir);
    unlink(sock);
}
