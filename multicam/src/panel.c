#include "panel.h"
#include "auth.h"
#include "catalog.h"
#include "capture.h"
#include "multicam.h"
#include "vendor/cJSON.h"
#include <ainice/bridge.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static volatile int *g_stop_flag, *g_reload_flag;
static const char *g_config_path;
static pthread_t g_bridge_th;
static bool g_bridge_started = false;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static panel_status_t g_status;

/* tile→捕获映射（run() 启动捕获后绑定，preview.full 快照用） */
static pthread_mutex_t g_caps_lock = PTHREAD_MUTEX_INITIALIZER;
static capture_t *g_caps;
static int g_cap_by_tile[MULTICAM_MAX_TILES];

void panel_bind_captures(void *caps, const int *cap_by_tile, int n)
{
    pthread_mutex_lock(&g_caps_lock);
    g_caps = caps;
    for (int i = 0; i < MULTICAM_MAX_TILES; i++)
        g_cap_by_tile[i] = (cap_by_tile && i < n) ? cap_by_tile[i] : -1;
    pthread_mutex_unlock(&g_caps_lock);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* 配置保存的原子写：tmp + fsync + rename */
static int write_file_atomic(const char *path, const char *data, size_t len)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, len);
    int rc = (w == (ssize_t)len) ? 0 : -1;
    if (rc == 0) rc = fsync(fd);
    close(fd);
    if (rc == 0) rc = rename(tmp, path);
    if (rc != 0) unlink(tmp);
    return rc;
}

static cJSON *resp_ok(void)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return r;
}

static int reply_json(ainice_bridge_response_t *response, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return -1;
    int rc = ainice_bridge_response_json(response, s);
    free(s);
    return rc;
}

static int handle_status_get(ainice_bridge_response_t *response)
{
    panel_status_t st;
    pthread_mutex_lock(&g_status_lock);
    st = g_status;
    pthread_mutex_unlock(&g_status_lock);

    cJSON *r = resp_ok();
    cJSON *s = cJSON_AddObjectToObject(r, "status");
    cJSON_AddBoolToObject(s, "valid", st.valid);
    cJSON_AddBoolToObject(s, "pushing", st.pushing);
    cJSON_AddStringToObject(s, "layout_type", st.layout_type[0] ? st.layout_type : "crop_1x2");
    cJSON_AddNumberToObject(s, "canvas_w", st.canvas_w);
    cJSON_AddNumberToObject(s, "canvas_h", st.canvas_h);
    cJSON_AddNumberToObject(s, "tile_w", st.tile_w);
    cJSON_AddNumberToObject(s, "tile_h", st.tile_h);
    cJSON_AddNumberToObject(s, "layout_cols", st.layout_cols);
    cJSON_AddNumberToObject(s, "fps", st.fps);
    /* 进程 RSS（statm 第 2 列 = 常驻页数；插件沙箱可读自己的 /proc/self） */
    {
        FILE *f = fopen("/proc/self/statm", "r");
        long total_pages = 0, rss_pages = 0;
        if (f && fscanf(f, "%ld %ld", &total_pages, &rss_pages) == 2)
            cJSON_AddNumberToObject(s, "rss_kb", rss_pages * 4);
        if (f) fclose(f);
    }
    cJSON_AddNumberToObject(s, "frames", (double)st.frames);
    cJSON_AddNumberToObject(s, "session_failures", st.session_failures);
    cJSON_AddNumberToObject(s, "uptime_s", (double)(time(NULL) - st.started_at));
    cJSON *tiles = cJSON_AddArrayToObject(s, "tiles");
    for (int i = 0; i < st.n_slots; i++) {
        panel_tile_t *t = &st.tiles[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i);
        cJSON_AddStringToObject(o, "cam_id", t->cam_id);
        cJSON_AddBoolToObject(o, "enabled", t->enabled);
        cJSON_AddBoolToObject(o, "gray", t->gray);
        cJSON_AddNumberToObject(o, "age_ms", (double)t->age_ms);
        cJSON_AddStringToObject(o, "note", t->note);
        cJSON_AddNumberToObject(o, "reconnects", (double)t->reconnects);
        cJSON_AddNumberToObject(o, "decoded", (double)t->decoded);
        cJSON_AddNumberToObject(o, "keyint_ms", t->keyint_ms);
        cJSON_AddNumberToObject(o, "crop_x", t->crop_x);
        cJSON_AddNumberToObject(o, "src_w", t->src_w);
        cJSON_AddNumberToObject(o, "src_h", t->src_h);
        cJSON_AddNumberToObject(o, "g2r_port", t->g2r_port);
        cJSON_AddNumberToObject(o, "g2r_rss_kb", t->g2r_rss_kb);
        cJSON_AddBoolToObject(o, "g2r_alive", t->g2r_alive);
        cJSON_AddItemToArray(tiles, o);
    }
    return reply_json(response, r);
}

static int handle_config_get(ainice_bridge_response_t *response)
{
    FILE *fp = fopen(g_config_path, "rb");
    if (!fp) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", "配置文件不存在");
        return reply_json(response, r);
    }
    char buf[32768];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = 0;
    cJSON *doc = cJSON_Parse(buf);
    if (!doc) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", "配置解析失败");
        return reply_json(response, r);
    }
    cJSON *r = resp_ok();
    cJSON_AddItemToObject(r, "config", doc);
    return reply_json(response, r);
}

/* config.set 的最小校验：enabled 摄像头 ≤8、tile 唯一且 0..7、fps 1..30 */
static int config_validate(cJSON *root, char *err, size_t err_cap)
{
    cJSON *cameras = cJSON_GetObjectItem(root, "cameras");
    if (!cJSON_IsArray(cameras)) {
        snprintf(err, err_cap, "cameras 必须是数组");
        return -1;
    }
    int n = cJSON_GetArraySize(cameras);
    if (n < 1 || n > MULTICAM_MAX_TILES) {
        snprintf(err, err_cap, "cameras 数量须在 1..%d", MULTICAM_MAX_TILES);
        return -1;
    }
    bool used[MULTICAM_MAX_TILES] = {false};
    cJSON *it;
    cJSON_ArrayForEach(it, cameras) {
        cJSON *tile = cJSON_GetObjectItem(it, "tile");
        cJSON *enabled = cJSON_GetObjectItem(it, "enabled");
        cJSON *source = cJSON_GetObjectItem(it, "source");
        if (!cJSON_IsString(source) || !source->valuestring[0]) {
            snprintf(err, err_cap, "camera.source 缺失");
            return -1;
        }
        if (!cJSON_IsTrue(enabled)) continue;
        int t = cJSON_IsNumber(tile) ? tile->valueint : -1;
        if (t < 0 || t >= MULTICAM_MAX_TILES) {
            snprintf(err, err_cap, "camera.tile 越界");
            return -1;
        }
        if (used[t]) {
            snprintf(err, err_cap, "多个启用摄像头占用 tile %d", t);
            return -1;
        }
        used[t] = true;
    }
    cJSON *output = cJSON_GetObjectItem(root, "output");
    cJSON *fps = output ? cJSON_GetObjectItem(output, "fps") : NULL;
    if (cJSON_IsNumber(fps) && (fps->valueint < 1 || fps->valueint > 30)) {
        snprintf(err, err_cap, "fps 须在 1..30");
        return -1;
    }
    return 0;
}

static int handle_config_set(cJSON *params, ainice_bridge_response_t *response)
{
    cJSON *cfg = cJSON_GetObjectItem(params, "config");
    if (!cJSON_IsObject(cfg)) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", "params.config 缺失");
        return reply_json(response, r);
    }
    char err[128] = "";
    if (config_validate(cfg, err, sizeof(err)) != 0) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", err);
        return reply_json(response, r);
    }
    char *out = cJSON_Print(cfg);
    if (!out) return -1;
    size_t len = strlen(out);
    out[len] = '\n';
    int rc = write_file_atomic(g_config_path, out, len + 1);
    free(out);
    cJSON *r = resp_ok();
    if (rc == 0) {
        cJSON_AddBoolToObject(r, "reload", true);
        *g_reload_flag = 1;
    } else {
        cJSON_AddStringToObject(r, "error", strerror(errno));
    }
    return reply_json(response, r);
}

/* 取景器快照（preview.full）：把 tile 对应捕获的全幅原画编一帧 JPEG 回给面板。
   每 tile 一份缓存，150ms 内重复请求直接回缓存（拖动跟手靠前端本地画框）。 */
#define SNAP_BUF_CAP (256 * 1024)
typedef struct {
    uint8_t *buf;
    size_t len;
    int64_t ms;
} snap_cache_t;
static snap_cache_t g_snap_cache[MULTICAM_MAX_TILES];

static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    /* base64 标准字母表；数字段拆成两段写——字符串替换式脱敏会把源码里
       连续 8 位数字误当密码替换（数字段曾被误替换成 0REDACTED9 致取景器
       花屏），运行期字符串内容不变 */
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                            "0123" "456789" "+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? T[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* 运行时换源（source.set）：bridge 线程校验+写配置文件+置待办；
   主循环 take 后就地重建该路（g2r/捕获），不重启进程。 */
static pthread_mutex_t g_src_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_src_pending = false;
static int g_src_tile = -1;
static char g_src_url[512];

bool panel_take_source_change(int *tile_out, char *src, size_t cap)
{
    pthread_mutex_lock(&g_src_lock);
    if (!g_src_pending) {
        pthread_mutex_unlock(&g_src_lock);
        return false;
    }
    *tile_out = g_src_tile;
    snprintf(src, cap, "%s", g_src_url);
    g_src_pending = false;
    g_src_tile = -1;
    pthread_mutex_unlock(&g_src_lock);
    return true;
}

static int handle_source_set(cJSON *params, ainice_bridge_response_t *response)
{
    int tile = -1;
    cJSON *j = cJSON_GetObjectItem(params, "tile");
    if (cJSON_IsNumber(j)) tile = j->valueint;
    j = cJSON_GetObjectItem(params, "source");
    const char *src = cJSON_IsString(j) ? j->valuestring : NULL;

    cJSON *r = resp_ok();
    if (tile < 0 || tile >= MULTICAM_MAX_TILES || !src || !src[0]) {
        cJSON_AddStringToObject(r, "error", "参数缺失或非法（tile/source）");
        return reply_json(response, r);
    }
    if (strncmp(src, "xiaomi://", 9) != 0 && strncmp(src, "rtsp://", 7) != 0) {
        cJSON_AddStringToObject(r, "error", "仅支持 xiaomi:// 或 rtsp:// 源");
        return reply_json(response, r);
    }

    /* 落盘：更新配置文件中该 tile 的 source（重启后仍生效） */
    {
        FILE *fp = fopen(g_config_path, "rb");
        char buf[32768];
        size_t n = fp ? fread(buf, 1, sizeof(buf) - 1, fp) : 0;
        if (fp) fclose(fp);
        buf[n] = 0;
        cJSON *doc = cJSON_Parse(buf);
        cJSON *cams = doc ? cJSON_GetObjectItem(doc, "cameras") : NULL;
        cJSON *it;
        bool found = false;
        cJSON_ArrayForEach(it, cams) {
            cJSON *t = cJSON_GetObjectItem(it, "tile");
            if (cJSON_IsNumber(t) && t->valueint == tile) {
                cJSON *s = cJSON_GetObjectItem(it, "source");
                if (cJSON_IsString(s)) cJSON_SetValuestring(s, src);
                else cJSON_AddStringToObject(it, "source", src);
                found = true;
                break;
            }
        }
        if (!found) {
            cJSON_Delete(doc);
            cJSON_AddStringToObject(r, "error", "配置中未找到该 tile");
            return reply_json(response, r);
        }
        char *out = cJSON_Print(doc);
        cJSON_Delete(doc);
        if (!out) {
            cJSON_AddStringToObject(r, "error", "配置序列化失败");
            return reply_json(response, r);
        }
        size_t len = strlen(out);
        out[len] = '\n';
        int wrc = write_file_atomic(g_config_path, out, len + 1);
        free(out);
        if (wrc != 0) {
            cJSON_AddStringToObject(r, "error", "配置写入失败");
            return reply_json(response, r);
        }
    }

    pthread_mutex_lock(&g_src_lock);
    g_src_tile = tile;
    snprintf(g_src_url, sizeof(g_src_url), "%s", src);
    g_src_pending = true;
    pthread_mutex_unlock(&g_src_lock);
    return reply_json(response, r);
}

static int handle_preview_full(cJSON *params, ainice_bridge_response_t *response)
{
    int tile = 0;
    cJSON *j = cJSON_GetObjectItem(params, "cam");
    if (cJSON_IsNumber(j)) tile = j->valueint;
    else if (cJSON_IsString(j) && j->valuestring[0]) tile = atoi(j->valuestring);

    capture_t *cap = NULL;
    pthread_mutex_lock(&g_caps_lock);
    if (tile >= 0 && tile < MULTICAM_MAX_TILES && g_caps && g_cap_by_tile[tile] >= 0)
        cap = &g_caps[g_cap_by_tile[tile]];
    pthread_mutex_unlock(&g_caps_lock);

    snap_cache_t *c = &g_snap_cache[tile >= 0 && tile < MULTICAM_MAX_TILES ? tile : 0];
    int64_t now = now_ms();
    if (cap && c->buf && c->len && now - c->ms < 150) {
        cJSON *r = resp_ok();
        char *b64 = malloc((c->len + 2) / 3 * 4 + 1);
        if (!b64) { cJSON_AddStringToObject(r, "error", "oom"); return reply_json(response, r); }
        b64_encode(c->buf, c->len, b64);
        cJSON_AddStringToObject(r, "jpeg", b64);
        cJSON_AddNumberToObject(r, "ts", c->ms);
        free(b64);
        return reply_json(response, r);
    }
    if (!cap) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", "捕获未就绪（插件启动中或该路未启用）");
        return reply_json(response, r);
    }
    if (!c->buf) {
        c->buf = malloc(SNAP_BUF_CAP);
        if (!c->buf) {
            cJSON *r = resp_ok();
            cJSON_AddStringToObject(r, "error", "oom");
            return reply_json(response, r);
        }
    }
    int n = capture_snapshot_jpeg(cap, c->buf, SNAP_BUF_CAP);
    if (n < 0) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", n == -2 ? "JPEG 过大（缓冲不足）" : "无可用帧");
        return reply_json(response, r);
    }
    c->len = (size_t)n;
    c->ms = now;
    cJSON *r = resp_ok();
    char *b64 = malloc(((size_t)n + 2) / 3 * 4 + 1);
    if (!b64) { cJSON_AddStringToObject(r, "error", "oom"); return reply_json(response, r); }
    b64_encode(c->buf, c->len, b64);
    cJSON_AddStringToObject(r, "jpeg", b64);
    cJSON_AddNumberToObject(r, "ts", c->ms);
    free(b64);
    return reply_json(response, r);
}

static int handle_cameras_list(const char *data_dir, const char *mh_yaml,
                               const char *g2r_bin, ainice_bridge_response_t *response)
{
    char err[192] = "";
    char sock[512];
    snprintf(sock, sizeof(sock), "%s/tmp/g2r-disc.sock", data_dir);
    if (catalog_ensure_instance(data_dir, mh_yaml, g2r_bin, err, sizeof(err)) != 0) {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", err);
        return reply_json(response, r);
    }
    cJSON *devices = catalog_fetch_devices(sock, err, sizeof(err));
    catalog_stop_instance(data_dir);
    cJSON *r = resp_ok();
    if (devices) {
        cJSON_AddItemToObject(r, "devices", devices);
    } else {
        cJSON_AddStringToObject(r, "error", err[0] ? err : "目录查询失败");
    }
    return reply_json(response, r);
}

/* 原生授权 bridge：面板经 multicam 模块直接完成米家短信授权（1:1 复刻
   mhcamera 的链路，见 vendor/xiaomi_api_client.c 与 src/auth.c）。 */
static int bridge_auth_session(cJSON *params, ainice_bridge_response_t *response)
{
    const char *data_dir = getenv("AINICE_PLUGIN_DATA");
    const char *plugin_dir = getenv("AINICE_PLUGIN_DIR");
    char g2r[512];
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", data_dir && data_dir[0] ? data_dir : ".");
    snprintf(g2r, sizeof(g2r), "%s/bin/go2rtc", plugin_dir ? plugin_dir : "/plugins/multicam");

    const char *action = "";
    cJSON *a = cJSON_GetObjectItem(params, "action");
    if (cJSON_IsString(a)) action = a->valuestring;
    const char *cc = "", *nn = "", *code = "";
    cJSON *j;
    j = cJSON_GetObjectItem(params, "calling_code");   if (cJSON_IsString(j)) cc = j->valuestring;
    j = cJSON_GetObjectItem(params, "national_number");if (cJSON_IsString(j)) nn = j->valuestring;
    j = cJSON_GetObjectItem(params, "code");           if (cJSON_IsString(j)) code = j->valuestring;

    char err[192] = "";
    if (auth_phone(dir, g2r, action, cc, nn, code, err, sizeof(err)) != 0) {
        cJSON *r = resp_ok();
        cJSON *auth = cJSON_AddObjectToObject(r, "auth");
        cJSON_AddStringToObject(auth, "state_text", "error");
        cJSON_AddStringToObject(auth, "error", err);
        return reply_json(response, r);
    }
    cJSON *r = resp_ok();
    cJSON *auth = cJSON_AddObjectToObject(r, "auth");
    cJSON_AddStringToObject(auth, "state_text", auth_state_text());
    if (auth_masked_target()[0])
        cJSON_AddStringToObject(auth, "masked_target", auth_masked_target());
    if (auth_code_length())
        cJSON_AddNumberToObject(auth, "code_length", auth_code_length());
    if (auth_retry_after())
        cJSON_AddNumberToObject(auth, "retry_after_seconds", auth_retry_after());
    int rc = reply_json(response, r);
    /* verify 成功：token 已写授权实例 yaml → 迁移自持快照并回收实例；
       响应送达后再置 reload（防 execv 切断响应） */
    if (!strcmp(auth_state_text(), "authenticated")) {
        if (auth_migrate_token(dir, err, sizeof(err)) == 0) {
            fprintf(stderr, "multicam: ✓ 授权完成，token 已迁移自持快照\n");
            *g_reload_flag = 1;
        } else {
            fprintf(stderr, "multicam: ⚠ token 迁移失败：%s\n", err);
        }
        auth_service_stop();
    }
    return rc;
}

static int bridge_auth_status(ainice_bridge_response_t *response)
{
    const char *data_dir = getenv("AINICE_PLUGIN_DATA");
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", data_dir && data_dir[0] ? data_dir : ".");
    cJSON *r = resp_ok();
    cJSON *auth = cJSON_AddObjectToObject(r, "auth");
    int authorized = auth_authorized(dir, "/data/plugins/mhcamera/go2rtc.yaml");
    cJSON_AddStringToObject(auth, "state_text",
                            authorized ? "authenticated" : "no_token");
    return reply_json(response, r);
}

/* 从 yaml 移除 xiaomi: 段及其缩进子行（保留其余配置；auth.clear 用，
   否则 token 会从 mh yaml 回退源复活，清除不彻底） */
static void strip_xiaomi_section(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char buf[16384];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    FILE *o = fopen(path, "w");
    if (!o) return;
    bool in_section = false;
    char *line = buf, *next;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "xiaomi:", 7)) { in_section = true; }
        else if (in_section && (line[0] == ' ' || line[0] == '\t')) { /* 属于段的行，跳过 */ }
        else { in_section = false; fprintf(o, "%s\n", line); }
        line = next;
    }
    fclose(o);
}

/* 清除授权：删自持快照与实例 yaml（g2r_start 会重建），清授权实例与
   mh yaml 回退源的 xiaomi 段（防复活），停授权实例并重载 */
static int bridge_auth_clear(ainice_bridge_response_t *response)
{
    const char *data_dir = getenv("AINICE_PLUGIN_DATA");
    char dir[256], path[512];
    snprintf(dir, sizeof(dir), "%s", data_dir && data_dir[0] ? data_dir : ".");
    snprintf(path, sizeof(path), "%s/xiaomi-token.yaml", dir);
    unlink(path);
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "%s/go2rtc-%d.yaml", dir, i);
        unlink(path);
    }
    snprintf(path, sizeof(path), "%s/go2rtc-auth.yaml", dir);
    strip_xiaomi_section(path);
    strip_xiaomi_section("/data/plugins/mhcamera/go2rtc.yaml");
    auth_service_stop();
    *g_reload_flag = 1;
    return reply_json(response, resp_ok());
}

static int bridge_handler(const char *request_json, ainice_bridge_response_t *response,
                          void *userdata)
{
    (void)userdata;
    cJSON *root = request_json ? cJSON_Parse(request_json) : NULL;
    cJSON *method_item = root ? cJSON_GetObjectItem(root, "method") : NULL;
    cJSON *params = root ? cJSON_GetObjectItem(root, "params") : NULL;
    const char *method = cJSON_IsString(method_item) ? method_item->valuestring : "";
    if (!params || !cJSON_IsObject(params)) params = cJSON_CreateObject();

    int rc;
    if (!strcmp(method, "status.get")) {
        rc = handle_status_get(response);
    } else if (!strcmp(method, "config.get")) {
        rc = handle_config_get(response);
    } else if (!strcmp(method, "config.set")) {
        rc = handle_config_set(params, response);
    } else if (!strcmp(method, "reload")) {
        *g_reload_flag = 1;
        rc = reply_json(response, resp_ok());
    } else if (!strcmp(method, "source.set")) {
        rc = handle_source_set(params, response);
    } else if (!strcmp(method, "preview.full")) {
        rc = handle_preview_full(params, response);
    } else if (!strcmp(method, "auth.session")) {
        rc = bridge_auth_session(params, response);
    } else if (!strcmp(method, "auth.status")) {
        rc = bridge_auth_status(response);
    } else if (!strcmp(method, "auth.clear")) {
        rc = bridge_auth_clear(response);
    } else if (!strcmp(method, "auth.asset")) {
        /* 已废弃：授权改原生实现，不再内嵌 mhcamera 安装包 */
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", "auth.asset 已废弃（原生授权）");
        rc = reply_json(response, r);
    } else if (!strcmp(method, "cameras.list")) {
        const char *data_dir = getenv("AINICE_PLUGIN_DATA");
        const char *plugin_dir = getenv("AINICE_PLUGIN_DIR");
        char g2r[512], mh[512];
        snprintf(g2r, sizeof(g2r), "%s/bin/go2rtc", plugin_dir ? plugin_dir : "/plugins/multicam");
        snprintf(mh, sizeof(mh), "%s", "/data/plugins/mhcamera/go2rtc.yaml");
        /* mhcamera yaml 路径优先用当前配置里的（config.get 同源），简化：直接读文件 */
        rc = handle_cameras_list(data_dir && data_dir[0] ? data_dir : ".",
                                 mh, g2r, response);
    } else {
        cJSON *r = resp_ok();
        cJSON_AddStringToObject(r, "error", "unknown method");
        rc = reply_json(response, r);
    }
    if (params != cJSON_GetObjectItem(root, "params")) cJSON_Delete(params);
    cJSON_Delete(root);
    return rc;
}

static int should_stop_bridge(void *userdata)
{
    (void)userdata;
    return (*g_stop_flag || *g_reload_flag) ? 1 : 0;
}

static void *bridge_thread(void *arg)
{
    (void)arg;
    ainice_bridge_serve_until("multicam", bridge_handler, NULL,
                              should_stop_bridge, NULL);
    return NULL;
}

void panel_init(volatile int *stop_flag, volatile int *reload_flag, const char *config_path)
{
    g_stop_flag = stop_flag;
    g_reload_flag = reload_flag;
    g_config_path = config_path;
    memset(&g_status, 0, sizeof(g_status));
}

int panel_start(void)
{
    if (pthread_create(&g_bridge_th, NULL, bridge_thread, NULL) == 0) {
        g_bridge_started = true;
        return 0;
    }
    return -1;
}

void panel_stop(void)
{
    if (g_bridge_started) {
        pthread_join(g_bridge_th, NULL);
        g_bridge_started = false;
    }
}

void panel_status_publish(const panel_status_t *st)
{
    pthread_mutex_lock(&g_status_lock);
    g_status = *st;
    pthread_mutex_unlock(&g_status_lock);
}
