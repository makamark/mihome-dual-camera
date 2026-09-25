#include "multicam.h"
#include "auth.h"
#include "capture.h"
#include "compose.h"
#include "g2r.h"
#include "panel.h"
#include "push.h"
#include <ainice/api.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

#define TILE_STALE_MS 3000
#define FIRST_FRAME_TIMEOUT_MS 40000
#define G2R_RESPAWN_MIN_MS 5000
#define G2R_RESPAWN_MAX_MS 60000
/* tile 持续置灰超过该时长 → 强制重建 go2rtc 实例
   （token 跟随：g2r_start 会重抽 mhcamera yaml 的最新 xiaomi 段） */
#define TILE_FORCE_RESTART_MS (10 * 60 * 1000)

static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_reload = 0;   /* 面板保存配置后置位：进程内 exec 重启 */
static char **g_argv;                         /* exec 重启复用命令行 */

static void on_signal(int signo) { (void)signo; g_stop = 1; }

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void fill_gray_tile(uint8_t *canvas, int canvas_w, int canvas_h,
                           int slot, int cols, int tile_w, int tile_h)
{
    int ox = (slot % cols) * tile_w, oy = (slot / cols) * tile_h;
    size_t ysz = (size_t)canvas_w * canvas_h;
    int cw = canvas_w / 2, ch = canvas_h / 2;
    uint8_t *y = canvas + (size_t)oy * canvas_w + ox;
    uint8_t *u = canvas + ysz + (size_t)(oy / 2) * cw + ox / 2;
    uint8_t *v = u + (size_t)cw * ch;
    for (int row = 0; row < tile_h; row++)
        memset(y + (size_t)row * canvas_w, 90, tile_w);
    for (int row = 0; row < tile_h / 2; row++) {
        memset(u + (size_t)row * cw, 128, tile_w / 2);
        memset(v + (size_t)row * cw, 128, tile_w / 2);
    }
}

static void fill_test_tile(uint8_t *tile, int w, int h, int seed, int frame)
{
    int off = (frame * 8 + seed * 40) % w;
    size_t ysize = (size_t)w * h;
    for (int y = 0; y < h; y++) {
        uint8_t row = (uint8_t)(y * 255 / h);
        for (int x = 0; x < w; x++) {
            int sx = (x + off) % w;
            tile[(size_t)y * w + x] = (uint8_t)((sx < w / 2 ? 60 : 200) ^ (row & 0x30) ^ (seed << 4));
        }
    }
    for (size_t i = 0; i < ysize / 4; i++)
        tile[ysize + i] = (uint8_t)(seed == 0 ? 128 + (i % 32) : 128 - (i % 32));
    memset(tile + ysize + ysize / 2, 128, ysize / 4); /* V 平面 */
}

/* go2rtc 子进程 RSS（statm 第 2 列 ×4KB；子进程在本命名空间可读） */
static long pid_rss_kb(long pid)
{
    char path[48];
    long total_pages = 0, rss_pages = 0;
    if (pid <= 0) return 0;
    snprintf(path, sizeof(path), "/proc/%ld/statm", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (fscanf(f, "%ld %ld", &total_pages, &rss_pages) != 2) rss_pages = 0;
    fclose(f);
    return rss_pages * 4;
}

static void default_config_path(char *out, size_t cap)
{
    const char *data = getenv("AINICE_PLUGIN_DATA");
    if (data && data[0]) snprintf(out, cap, "%s/multicam.json", data);
    else snprintf(out, cap, "etc/multicam.json");
}

static int set_input_mode_bitstream(void)
{
    char req[192];
    char *resp = NULL;
    snprintf(req, sizeof(req),
             "{\"id\":\"multicam-%ld\",\"method\":\"config.patch\","
             "\"params\":{\"key\":\"input.mode\",\"value\":\"bitstream\"}}",
             (long)time(NULL));
    if (ainice_api_call_json(req, 5000, &resp) != 0 || !resp) {
        fprintf(stderr, "multicam: config.patch input.mode failed\n");
        return -1;
    }
    if (!strstr(resp, "\"ok\":true")) {
        fprintf(stderr, "multicam: config.patch not ok: %s\n", resp);
        ainice_api_response_free(resp);
        return -1;
    }
    ainice_api_response_free(resp);
    fprintf(stderr, "multicam: input.mode=bitstream\n");
    return 0;
}

/* ---------- g2r 子进程守护：崩溃/OOM 被杀后自动重建（退避重试） ---------- */

typedef struct {
    g2r_instance_t *inst;
    int count;
} watcher_ctx_t;

static void *g2r_watcher(void *arg)
{
    watcher_ctx_t *ctx = arg;
    int64_t backoff[MULTICAM_MAX_TILES] = {0};
    int64_t next_try[MULTICAM_MAX_TILES] = {0};
    int64_t alive_since[MULTICAM_MAX_TILES] = {0};
    int consec[MULTICAM_MAX_TILES] = {0};

    while (!g_stop && !g_reload) {
        int64_t now = (int64_t)now_us() / 1000;
        for (int i = 0; i < ctx->count; i++) {
            g2r_instance_t *g = &ctx->inst[i];
            if (g->pid <= 0) continue;
            int st = 0;
            pid_t w = waitpid((pid_t)g->pid, &st, WNOHANG);
            if (w == 0) {
                if (!alive_since[i]) alive_since[i] = now;
                if (consec[i] && now - alive_since[i] >= 60000) {
                    consec[i] = 0;          /* 稳定存活，清空退避 */
                    backoff[i] = 0;
                }
                continue;
            }
            g->pid = 0;
            g->running = false;
            consec[i]++;
            if (consec[i] == 1) {
                backoff[i] = G2R_RESPAWN_MIN_MS;
                next_try[i] = now;        /* 首次崩溃立即重建 */
            } else {
                if (backoff[i] < G2R_RESPAWN_MAX_MS) backoff[i] *= 2;
                next_try[i] = now + backoff[i];
            }
            alive_since[i] = 0;
            fprintf(stderr, "multicam: g2r[%d] 子进程退出，准备第 %d 次重建（退避 %lldms）\n",
                    g->index, consec[i], (long long)backoff[i]);
        }
        for (int i = 0; i < ctx->count; i++) {
            g2r_instance_t *g = &ctx->inst[i];
            if (g->pid != 0 || !g->username[0] || g->frozen) continue;
            /* 重建来源：进程崩溃（上面 waitpid 已排程）或 restart_pending
               （初始启动失败降级 / 源长期不健康，由主循环置位） */
            if (g->restart_pending && next_try[i] == 0) {
                next_try[i] = (int64_t)now_us() / 1000;
                if (!backoff[i]) backoff[i] = G2R_RESPAWN_MIN_MS;
            }
            if (next_try[i] == 0) continue;
            int64_t now2 = (int64_t)now_us() / 1000;
            if (now2 < next_try[i]) continue;
            char err[256];
            if (g2r_start(g, err, sizeof(err)) == 0) {
                next_try[i] = 0;
                g->restart_pending = false;
                fprintf(stderr, "multicam: g2r[%d] 重建成功\n", g->index);
            } else {
                fprintf(stderr, "multicam: g2r[%d] 重建失败: %s（%lldms 后重试）\n",
                        g->index, err, (long long)backoff[i]);
                next_try[i] = now2 + backoff[i];
                if (backoff[i] < G2R_RESPAWN_MAX_MS) backoff[i] *= 2;
            }
        }
        for (int s = 0; s < 10 && !g_stop && !g_reload; s++) usleep(100 * 1000);
    }
    return NULL;
}

/* ---------- run 模式：真实拉流 → 软解 → 合成 → 推流 ---------- */

static int run(const multicam_config_t *cfg)
{
    int rc = -1;
    int cam_cfg[MULTICAM_MAX_TILES];    /* 启用路 → cfg->cameras 下标 */
    int cam_tile[MULTICAM_MAX_TILES];   /* 启用路 → tile 槽位（配置值，禁用不前移） */
    int slot_owner[MULTICAM_MAX_TILES]; /* tile 槽位 → 捕获下标，-1 = 空位灰块 */
    int n = 0, n_slots = 0;
    int cols = 1, rows = 1;             /* tile 网格：cols 列 × rows 行 */
    g2r_instance_t inst[MULTICAM_MAX_TILES];
    capture_t caps[MULTICAM_MAX_TILES];
    char urls[MULTICAM_MAX_TILES][512];
    bool cap_started[MULTICAM_MAX_TILES] = {false};
    uint8_t *canvas = NULL;
    push_t push = {0};
    int tile_w = 0, tile_h = 0, canvas_w = 0, canvas_h = 0;
    int src_w = 0, src_h = 0;
    bool push_opened = false;
    pthread_t watcher_th;
    bool watcher_started = false;
    watcher_ctx_t wctx = { inst, MULTICAM_MAX_TILES };

    memset(inst, 0, sizeof(inst));
    memset(caps, 0, sizeof(caps));
    for (int s = 0; s < MULTICAM_MAX_TILES; s++) slot_owner[s] = -1;

    /* 清掉上一代 app 的孤儿 go2rtc（占端口/旧凭证的祸根） */
    g2r_kill_stale();

    /* 配置是布局唯一事实源：tile 槽位取 cameras[].tile，禁用某路留灰位，
       防止后路前移导致 zone 串区 */
    for (int i = 0; i < cfg->camera_count; i++) {
        if (!cfg->cameras[i].enabled) continue;
        int t = cfg->cameras[i].tile;
        if (t < 0 || t >= MULTICAM_MAX_TILES) {
            fprintf(stderr, "multicam: cam[%d](%s) tile=%d 越界\n",
                    i, cfg->cameras[i].id, t);
            goto out;
        }
        if (slot_owner[t] >= 0) {
            fprintf(stderr, "multicam: cam[%d](%s) 与 cam[%d](%s) 都占用 tile %d\n",
                    i, cfg->cameras[i].id, cam_cfg[slot_owner[t]],
                    cfg->cameras[cam_cfg[slot_owner[t]]].id, t);
            goto out;
        }
        cam_cfg[n] = i;
        cam_tile[n] = t;
        slot_owner[t] = n;
        n++;
    }
    if (n == 0) { fprintf(stderr, "multicam: 没有启用的摄像头\n"); goto out; }
    for (int j = 0; j < n; j++)
        if (cam_tile[j] + 1 > n_slots) n_slots = cam_tile[j] + 1;

    for (int j = 0; j < n; j++) {
        const multicam_camera_t *cam = &cfg->cameras[cam_cfg[j]];
        /* rtsp:// 源直连（无需 go2rtc 认证包装）；其余（xiaomi://）走 go2rtc 实例 */
        if (!strncmp(cam->source, "rtsp://", 7)) {
            inst[j].pid = 0;
            fprintf(stderr, "multicam: cam[%d](%s) tile %d 直连 %s\n",
                    cam_cfg[j], cam->id, cam_tile[j], cam->source);
            if (capture_start(&caps[j], cam->source) != 0) {
                fprintf(stderr, "multicam: capture[%d] 启动失败\n", j);
                goto out;
            }
            cap_started[j] = true;
            continue;
        }
        g2r_instance_t *g = &inst[j];
        g->index = j;
        g->port = cfg->base_port + j;
        snprintf(g->source, sizeof(g->source), "%s", cam->source);
        snprintf(g->data_dir, sizeof(g->data_dir), "%s", cfg->data_dir);
        snprintf(g->g2r_bin, sizeof(g->g2r_bin), "%s", cfg->g2r_bin);
        snprintf(g->mh_yaml, sizeof(g->mh_yaml), "%s", cfg->mh_yaml);

        char err[256];
        if (g2r_start(g, err, sizeof(err)) != 0) {
            /* 降级启动：单路拨号失败（token 过期/摄像头拒绝等）不拖垮整个插件，
               标记 restart_pending 交给 watcher 周期重试（每次重抽最新 token） */
            fprintf(stderr, "multicam: cam[%d](%s) go2rtc 启动失败（降级）: %s\n",
                    cam_cfg[j], cam->id, err);
            g->restart_pending = true;
        }
        snprintf(urls[j], sizeof(urls[j]),
                 "rtsp://%s:%s@127.0.0.1:%d/stream", g->username, g->password, g->port);
        fprintf(stderr, "multicam: cam[%d](%s) tile %d go2rtc port=%d %s\n",
                cam_cfg[j], cam->id, cam_tile[j], g->port,
                g->restart_pending ? "(等待重建)" : "ok");
    }

    for (int j = 0; j < n; j++) {
        if (cap_started[j]) continue;
        if (capture_start(&caps[j], urls[j]) != 0) {
            fprintf(stderr, "multicam: capture[%d] 启动失败\n", j);
            goto out;
        }
        cap_started[j] = true;
    }
    /* 取景器快照绑定：tile → 捕获下标（bridge preview.full 用） */
    panel_bind_captures(caps, slot_owner, MULTICAM_MAX_TILES);

    /* 等首帧以确定 tile 尺寸；降级容忍：至少一路有帧即可（其余路由 watcher 补救） */
    {
        int64_t deadline = (int64_t)now_us() / 1000 + FIRST_FRAME_TIMEOUT_MS;
        while ((int64_t)now_us() / 1000 < deadline && !g_stop) {
            int ready = 0;
            for (int j = 0; j < n; j++) {
                int w, h;
                capture_dims(&caps[j], &w, &h);
                if (w > 0) ready++;
            }
            if (ready == n) break;
            usleep(300 * 1000);
        }
        for (int j = 0; j < n; j++) {
            int w, h;
            capture_dims(&caps[j], &w, &h);
            if (w == 0) {
                fprintf(stderr, "multicam: cam[%d](%s) 首帧未到（%s），tile 暂置灰\n",
                        cam_cfg[j], cfg->cameras[cam_cfg[j]].id, caps[j].last_error);
                continue;
            }
            if (src_w == 0) { src_w = w; src_h = h; }
            else if (w != src_w || h != src_h) {
                fprintf(stderr, "multicam: cam[%d](%s) 分辨率 %dx%d 与首路 %dx%d 不一致（v1 要求一致）\n",
                        cam_cfg[j], cfg->cameras[cam_cfg[j]].id, w, h, src_w, src_h);
                goto out;
            }
        }
        if (src_w == 0) {
            /* 全部降级：回退 848x480 */
            src_w = 848;
            src_h = 480;
            fprintf(stderr, "multicam: 所有摄像头均无首帧，按缺省 %dx%d 全灰运行\n",
                    src_w, src_h);
        }
        if (!strcmp(cfg->layout_type, "crop_1x2")) {
            tile_w = 400;
            tile_h = src_h;
            cols = 2;
            rows = 1;
            n_slots = 2;
            canvas_w = 800;
            canvas_h = tile_h;
            fprintf(stderr, "multicam: 左右并排裁切（源 %dx%d，各裁 400x%d）→ 画布 %dx%d(5:3) @%dfps\n",
                    src_w, src_h, tile_h, canvas_w, canvas_h, cfg->fps);
        } else {
            tile_w = src_w;
            tile_h = src_h;
            cols = cfg->layout_cols > 0 ? cfg->layout_cols : n_slots;
            if (cols > n_slots) cols = n_slots;
            if (cols < 1) cols = 1;
            rows = (n_slots + cols - 1) / cols;
            canvas_w = tile_w * cols;
            canvas_h = tile_h * rows;
            fprintf(stderr, "multicam: %d 路 %dx%d（%d 槽位，%d列×%d行）→ 画布 %dx%d @%dfps\n",
                    n, tile_w, tile_h, n_slots, cols, rows, canvas_w, canvas_h, cfg->fps);
        }
    }

    canvas = malloc((size_t)canvas_w * canvas_h * 3 / 2);
    if (!canvas) { fprintf(stderr, "multicam: oom\n"); goto out; }
    size_t ysz = (size_t)canvas_w * canvas_h;

    /* 检测区域由设备侧 zone profile 管理（总览界面绘制），插件不再下发 */

    if (pthread_create(&watcher_th, NULL, g2r_watcher, &wctx) == 0)
        watcher_started = true;
    if (panel_start() != 0)
        fprintf(stderr, "multicam: ⚠ 面板 bridge 线程启动失败（Web 设置不可用）\n");

    /* 推送主循环；send_frame 失败（管线复位/mhcamera 过渡等）时重建会话续推。
       快照直接按行距写进画布 tile 区域（无中间 tile 缓冲）。
       会话失败无限退避（3s→10s→30s 封顶）：监督器不重启就自己扛，绝不退出。 */
    {
        int64_t interval_ms = 1000 / cfg->fps;
        uint64_t frames = 0;
        int session_failures = 0;
        int64_t last_trim_ms = 0;
        bool slot_gray[MULTICAM_MAX_TILES] = {false};
        int64_t gray_since[MULTICAM_MAX_TILES] = {0};
        char slot_note[MULTICAM_MAX_TILES][64] = {{0}};
        time_t proc_start = time(NULL);

        /* GOP 窗口解码按配置一次性下到各捕获线程（keyint 实测后自动激活） */
        for (int j = 0; j < n; j++)
            capture_set_window(&caps[j], cfg->window_enabled, cfg->window_frames);

        for (int s = 0; s < n_slots; s++) {
            fill_gray_tile(canvas, canvas_w, canvas_h, s, cols, tile_w, tile_h);
            slot_gray[s] = true;
            gray_since[s] = (int64_t)now_us() / 1000;
            snprintf(slot_note[s], sizeof(slot_note[s]), "启动置灰");
        }

        while (!g_stop && !g_reload) {
            int delay = session_failures < 3 ? 3 : (session_failures < 5 ? 10 : 30);
            if (set_input_mode_bitstream() != 0 || push_open(&push) != 0) {
                push.session = NULL;
                session_failures++;
                panel_status_t pst0 = {0};
                pst0.valid = true;
                snprintf(pst0.layout_type, sizeof(pst0.layout_type), "%s", cfg->layout_type);
                pst0.canvas_w = canvas_w; pst0.canvas_h = canvas_h;
                pst0.tile_w = tile_w; pst0.tile_h = tile_h;
                pst0.layout_cols = cols;
                pst0.fps = cfg->fps; pst0.frames = frames;
                pst0.session_failures = session_failures;
                pst0.started_at = proc_start; pst0.n_slots = n_slots;
                for (int s = 0; s < n_slots; s++) {
                    int owner = slot_owner[s];
                    if (owner < 0) continue;
                    const multicam_camera_t *c = &cfg->cameras[cam_cfg[owner]];
                    snprintf(pst0.tiles[s].cam_id, sizeof(pst0.tiles[s].cam_id), "%s", c->id);
                    pst0.tiles[s].enabled = true;
                    pst0.tiles[s].gray = true;
                    pst0.tiles[s].age_ms = capture_age(&caps[owner]);
                    snprintf(pst0.tiles[s].note, sizeof(pst0.tiles[s].note), "会话建立失败×%d", session_failures);
                    pst0.tiles[s].reconnects = caps[owner].reconnects;
                    pst0.tiles[s].decoded = caps[owner].slot.frame_count;
                    pst0.tiles[s].keyint_ms = (int)capture_keyint_ms(&caps[owner]);
                    pst0.tiles[s].crop_x = c->crop_x;
                    pst0.tiles[s].src_w = src_w;
                    pst0.tiles[s].src_h = src_h;
                    pst0.tiles[s].g2r_port = inst[owner].port;
                    pst0.tiles[s].g2r_rss_kb = (int)pid_rss_kb(inst[owner].pid);
                    pst0.tiles[s].g2r_alive = inst[owner].pid > 0;
                }
                panel_status_publish(&pst0);
                usleep(delay * 1000 * 1000);
                continue;
            }
            push_opened = true;
            session_failures = 0;
            uint64_t started_at = frames;
            while (!g_stop && !g_reload) {
                uint64_t due = now_us() + (uint64_t)interval_ms * 1000ull;
                int64_t now0 = (int64_t)now_us() / 1000;
                /* 周期归还 glibc arena 碎片（157MB 设备，RSS 观测见面板） */
#if defined(__GLIBC__)
                if (now0 - last_trim_ms >= 60000) {
                    last_trim_ms = now0;
                    malloc_trim(0);
                }
#endif
                auth_idle_tick();   /* 授权实例空闲 10 分钟自动回收 */

                /* 面板下拉换源：就地重建该路 go2rtc/捕获，不重启进程。
                   重建会阻塞主循环数秒（拨号/收线程），期间画布暂挂（另一路
                   恢复推送后自动跟上）；watcher 经 g->frozen 让路防并发重建。 */
                {
                    int chg_tile = -1;
                    char chg_src[512];
                    if (panel_take_source_change(&chg_tile, chg_src, sizeof(chg_src))
                        && chg_tile >= 0 && chg_tile < MULTICAM_MAX_TILES) {
                        int owner = slot_owner[chg_tile];
                        if (owner >= 0) {
                            g2r_instance_t *g = &inst[owner];
                            multicam_camera_t *cam = &cfg->cameras[cam_cfg[owner]];
                            fprintf(stderr, "multicam: tile %d 运行时换源 → %s\n", chg_tile, chg_src);
                            snprintf(cam->source, sizeof(cam->source), "%s", chg_src);
                            capture_stop(&caps[owner]);
                            cap_started[owner] = false;
                            g->frozen = true;
                            g2r_stop(g);
                            char err[256];
                            if (!strncmp(chg_src, "rtsp://", 7)) {
                                g->pid = 0;   /* 转直连形态 */
                                if (capture_start(&caps[owner], chg_src) == 0)
                                    cap_started[owner] = true;
                            } else {
                                snprintf(g->source, sizeof(g->source), "%s", chg_src);
                                g->restart_pending = false;
                                if (g2r_start(g, err, sizeof(err)) == 0) {
                                    snprintf(urls[owner], sizeof(urls[owner]),
                                             "rtsp://%s:%s@127.0.0.1:%d/stream",
                                             g->username, g->password, g->port);
                                    if (capture_start(&caps[owner], urls[owner]) == 0)
                                        cap_started[owner] = true;
                                } else {
                                    fprintf(stderr, "multicam: 换源 g2r 启动失败: %s\n", err);
                                    g->restart_pending = true;
                                }
                            }
                            g->frozen = false;
                            fill_gray_tile(canvas, canvas_w, canvas_h, chg_tile, cols, tile_w, tile_h);
                            slot_gray[chg_tile] = true;
                            gray_since[chg_tile] = (int64_t)now_us() / 1000;
                            snprintf(slot_note[chg_tile], sizeof(slot_note[chg_tile]), "换源中");
                        }
                    }
                }

                /* 自愈：换源失败/启动失败导致捕获缺失的启用路，重试 capture_start。
                   capture 线程内部自带断流重连，这里只补"线程不存在"的洞；
                   xiaomi 路须等 go2rtc 实例就绪（watcher 负责重建）。 */
                for (int s = 0; s < n_slots; s++) {
                    int owner = slot_owner[s];
                    if (owner < 0 || cap_started[owner]) continue;
                    const multicam_camera_t *c = &cfg->cameras[cam_cfg[owner]];
                    const char *pull;
                    if (!strncmp(c->source, "rtsp://", 7)) {
                        pull = c->source;
                    } else {
                        g2r_instance_t *g = &inst[owner];
                        if (g->pid <= 0 || !g->username[0]) continue;
                        snprintf(urls[owner], sizeof(urls[owner]),
                                 "rtsp://%s:%s@127.0.0.1:%d/stream",
                                 g->username, g->password, g->port);
                        pull = urls[owner];
                    }
                    fprintf(stderr, "multicam: capture[%d] 自愈重启\n", owner);
                    if (capture_start(&caps[owner], pull) == 0)
                        cap_started[owner] = true;
                }
                for (int s = 0; s < n_slots; s++) {
                    int owner = slot_owner[s];
                    int64_t age = -1;
                    /* 置灰阈值联动 keyint：窗口解码下帧龄按 GOP 节拍增长 */
                    int64_t stale_limit = TILE_STALE_MS;
                    if (owner >= 0) {
                        int64_t kit = capture_keyint_ms(&caps[owner]);
                        if (kit > 0 && kit * 2 + 500 > stale_limit) stale_limit = kit * 2 + 500;
                        int ox = (s % cols) * tile_w, oy = (s / cols) * tile_h;
                        uint8_t *y = canvas + (size_t)oy * canvas_w + ox;
                        uint8_t *u = canvas + ysz + (size_t)(oy / 2) * (canvas_w / 2) + ox / 2;
                        uint8_t *v = u + (size_t)(canvas_w / 2) * (canvas_h / 2);
                        if (!strcmp(cfg->layout_type, "crop_1x2")) {
                            int cx = cfg->cameras[cam_cfg[owner]].crop_x;
                            age = capture_paint_crop(&caps[owner], y, u, v,
                                                     canvas_w, canvas_w / 2,
                                                     cx, 0, tile_w, tile_h);
                        } else {
                            age = capture_paint_tile(&caps[owner], y, u, v,
                                                     canvas_w, canvas_w / 2, tile_w, tile_h);
                        }
                    }
                    if (age < 0 || age > stale_limit) {
                        if (!slot_gray[s]) {
                            fill_gray_tile(canvas, canvas_w, canvas_h, s, cols, tile_w, tile_h);
                            slot_gray[s] = true;
                            gray_since[s] = now0;
                            snprintf(slot_note[s], sizeof(slot_note[s]), "%s",
                                     age == -2 ? "分辨率不符" :
                                     age < 0 ? "无帧（拨号/解码未就绪）" : "断流超时");
                            fprintf(stderr, "multicam: tile %d 置灰（%s）\n", s, slot_note[s]);
                        }
                    } else if (slot_gray[s]) {
                        slot_gray[s] = false;
                        gray_since[s] = 0;
                        slot_note[s][0] = 0;
                    }
                }
                /* token 跟随：xiaomi 路的 tile 持续置灰超时 → 重建实例
                   （g2r_start 重抽 mhcamera yaml 的最新 xiaomi 段，凭证复用） */
                for (int s = 0; s < n_slots; s++) {
                    int owner = slot_owner[s];
                    if (owner < 0 || !slot_gray[s] || !gray_since[s]) continue;
                    g2r_instance_t *g = &inst[owner];
                    if (g->pid <= 0 || !g->username[0]) continue; /* 直连路无实例 */
                    if (now0 - gray_since[s] < TILE_FORCE_RESTART_MS) continue;
                    fprintf(stderr, "multicam: tile %d 置灰超 %dmin，重建 g2r[%d] 换取最新 token\n",
                            s, TILE_FORCE_RESTART_MS / 60000, g->index);
                    g2r_stop(g);
                    g->restart_pending = true;
                    gray_since[s] = now0; /* 重置计时，避免重建风暴 */
                }
                if (push_yuv420p(&push, canvas, canvas_w, canvas_h,
                                 frames * (uint64_t)(1000000 / cfg->fps)) != 0) {
                    fprintf(stderr, "multicam: send_frame 失败于帧 %llu，重建会话\n",
                            (unsigned long long)frames);
                    break;
                }
                frames++;
                if (frames - started_at == 1 || (frames - started_at) % ((unsigned)cfg->fps * 10) == 0) {
                    fprintf(stderr, "multicam: pushed=%llu ages=", (unsigned long long)frames);
                    for (int s = 0; s < n_slots; s++) {
                        int owner = slot_owner[s];
                        fprintf(stderr, "%s%lldms", s ? "," : "",
                                owner >= 0 ? (long long)capture_age(&caps[owner]) : -1LL);
                    }
                    fprintf(stderr, "\n");
                }
                /* 面板状态发布（每帧，互斥锁内 ~1KB 拷贝，开销可忽略） */
                panel_status_t pst = {0};
                pst.valid = true;
                pst.pushing = true;
                snprintf(pst.layout_type, sizeof(pst.layout_type), "%s", cfg->layout_type);
                pst.canvas_w = canvas_w; pst.canvas_h = canvas_h;
                pst.tile_w = tile_w; pst.tile_h = tile_h;
                pst.layout_cols = cols;
                pst.fps = cfg->fps; pst.frames = frames;
                pst.started_at = proc_start; pst.n_slots = n_slots;
                for (int s = 0; s < n_slots; s++) {
                    int owner = slot_owner[s];
                    panel_tile_t *pt = &pst.tiles[s];
                    if (owner < 0) continue;
                    const multicam_camera_t *c = &cfg->cameras[cam_cfg[owner]];
                    snprintf(pt->cam_id, sizeof(pt->cam_id), "%s", c->id);
                    pt->enabled = true;
                    pt->gray = slot_gray[s];
                    pt->age_ms = capture_age(&caps[owner]);
                    snprintf(pt->note, sizeof(pt->note), "%s", slot_note[s]);
                    pt->reconnects = caps[owner].reconnects;
                    pt->decoded = caps[owner].slot.frame_count;
                    pt->keyint_ms = (int)capture_keyint_ms(&caps[owner]);
                    pt->crop_x = c->crop_x;
                    pt->src_w = src_w;
                    pt->src_h = src_h;
                    pt->g2r_port = inst[owner].port;
                    pt->g2r_rss_kb = (int)pid_rss_kb(inst[owner].pid);
                    pt->g2r_alive = inst[owner].pid > 0;
                }
                panel_status_publish(&pst);
                uint64_t now = now_us();
                if (due > now) usleep((useconds_t)(due - now));
            }
            if (push_opened) { push_close(&push); push_opened = false; }
            if (!g_stop && !g_reload) usleep(3 * 1000 * 1000);
        }
        rc = 0;
        fprintf(stderr, "multicam: run 结束 pushed=%llu%s\n", (unsigned long long)frames,
                g_reload ? "（配置变更）" : "");
    }

out:
    if (watcher_started) pthread_join(watcher_th, NULL);
    panel_bind_captures(NULL, NULL, 0);   /* 先解绑，bridge 不再摸到停用中的捕获 */
    for (int j = 0; j < n; j++)
        if (cap_started[j]) capture_stop(&caps[j]);
    for (int i = 0; i < MULTICAM_MAX_TILES; i++)
        if (inst[i].pid > 0) g2r_stop(&inst[i]);
    if (push_opened) push_close(&push);
    free(canvas);
    panel_stop();
    if (g_reload) {
        /* 面板保存配置后进程内重启（同 pid，监督器无感知） */
        char self[512];
        ssize_t l = readlink("/proc/self/exe", self, sizeof(self) - 1);
        fprintf(stderr, "multicam: 应用新配置，进程内重启\n");
        fflush(stderr);
        if (l > 0) {
            self[l] = 0;
            execv(self, g_argv);
        }
        fprintf(stderr, "multicam: exec 失败(%s)，退出\n", strerror(errno));
        return 1;
    }
    return rc;
}

/* ---------- selftest 模式（合成画布，无外设依赖） ---------- */

static int selftest(const multicam_config_t *cfg, int frames)
{
    uint8_t *tile_a = malloc((size_t)cfg->tile_w * cfg->tile_h * 3 / 2);
    uint8_t *tile_b = malloc((size_t)cfg->tile_w * cfg->tile_h * 3 / 2);
    uint8_t *canvas = malloc((size_t)cfg->canvas_w * cfg->canvas_h * 3 / 2);
    const uint8_t *srcs[2] = { tile_a, tile_b };
    push_t push = {0};
    uint64_t interval_us = 1000000ull / (uint64_t)cfg->fps;
    int cols = cfg->layout_cols > 0 ? cfg->layout_cols : 2;
    int sent = 0, rc = 0;

    if (cols > 2) cols = 2;
    if (!tile_a || !tile_b || !canvas) { fprintf(stderr, "multicam: oom\n"); rc = -1; goto out; }
    if (set_input_mode_bitstream() != 0) { rc = -1; goto out; }
    if (push_open(&push) != 0) { rc = -1; goto out; }

    for (int i = 0; i < frames && !g_stop; i++) {
        uint64_t due = now_us() + interval_us;
        fill_test_tile(tile_a, cfg->tile_w, cfg->tile_h, 0, i);
        fill_test_tile(tile_b, cfg->tile_w, cfg->tile_h, 1, i);
        compose_grid_yuv420p(canvas, cfg->canvas_w, cfg->canvas_h, cols,
                             srcs, cfg->tile_w, cfg->tile_h, 2);
        if (push_yuv420p(&push, canvas, cfg->canvas_w, cfg->canvas_h,
                         (uint64_t)i * interval_us) != 0) {
            fprintf(stderr, "multicam: send_frame failed at frame %d\n", i);
            rc = -1;
            break;
        }
        sent++;
        if (sent == 1 || sent % 30 == 0)
            fprintf(stderr, "multicam: selftest sent=%d/%d\n", sent, frames);
        struct timespec ts = { .tv_sec = (time_t)(due / 1000000ull),
                               .tv_nsec = (long)(due % 1000000ull) * 1000l };
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR && !g_stop) {}
    }
    fprintf(stderr, "multicam: selftest done sent=%d\n", sent);
out:
    push_close(&push);
    free(tile_a); free(tile_b); free(canvas);
    return rc;
}

int main(int argc, char **argv)
{
    multicam_config_t cfg;
    char cfg_path[512];
    int frames = 90, selftest_mode = 0;

    g_argv = argv;
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (geteuid() == 0) {
        fprintf(stderr, "multicam: refusing to run as root\n");
        return 1;
    }

#if defined(__GLIBC__)
    mallopt(M_ARENA_MAX, 2); /* 限制每线程 arena，控制碎片内存（157MB 设备） */
#endif

    /* 低优先级：单核上解码/取流让路给主应用推理，削 inference 尾时延；go2rtc 子进程继承 */
    setpriority(PRIO_PROCESS, 0, 10);

    default_config_path(cfg_path, sizeof(cfg_path));
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            snprintf(cfg_path, sizeof(cfg_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--selftest")) selftest_mode = 1;
        else {
            fprintf(stderr, "usage: multicam [--config <path>] [--selftest [--frames N]]\n");
            return 2;
        }
    }

    panel_init((volatile int *)&g_stop, (volatile int *)&g_reload, cfg_path);

    if (config_load(&cfg, cfg_path) != 0) return 1;
    if (selftest_mode) {
        int cols = cfg.layout_cols > 0 ? cfg.layout_cols : 2;
        if (cols > 2) cols = 2;
        if (cfg.canvas_w != cols * cfg.tile_w ||
            cfg.canvas_h != ((2 + cols - 1) / cols) * cfg.tile_h) {
            fprintf(stderr, "multicam: selftest 只支持等尺寸双 tile（画布 %dx%d 与布局不符）\n",
                    cfg.canvas_w, cfg.canvas_h);
            return 1;
        }
        return selftest(&cfg, frames);
    }
    return run(&cfg);
}
