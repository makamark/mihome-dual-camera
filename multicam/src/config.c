#include "multicam.h"
#include "vendor/cJSON.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* 全新安装引导默认配置（与包内 etc/multicam.json 保持一致）。
   平台导入插件不落数据配置，首次启动时配置文件不存在——旧版直接退出，
   监督器对快速退出的插件不再拉起，表现为"无法启用"；现自动落盘默认配置
   并按默认运行（灰画布常驻），用户在面板选择设备/授权后即出画面。 */
static const char *CONFIG_DEFAULT_JSON =
    "{\n"
    "  \"layout\": { \"version\": 3, \"type\": \"crop_1x2\", \"tile\": [400, 480] },\n"
    "  \"output\": { \"fps\": 3, \"format\": \"yuv420p\" },\n"
    "  \"capture\": { \"window\": true, \"window_frames\": 4 },\n"
    "  \"go2rtc\": {\n"
    "    \"binary\": \"\",\n"
    "    \"mhcamera_yaml\": \"/data/plugins/mhcamera/go2rtc.yaml\",\n"
    "    \"base_port\": 8554\n"
    "  },\n"
    "  \"cameras\": [\n"
    "    { \"id\": \"cam-a\", \"tile\": 0, \"crop_x\": 224, \"enabled\": true, \"source\": \"xiaomi://\" },\n"
    "    { \"id\": \"cam-b\", \"tile\": 1, \"crop_x\": 224, \"enabled\": false, \"source\": \"xiaomi://\" }\n"
    "  ]\n"
    "}\n";

static int config_parse(multicam_config_t *cfg, const char *buf)
{
    cJSON *root, *item, *sub, *it;
    int canvas_w = 0;

    memset(cfg, 0, sizeof(*cfg));
    root = cJSON_Parse(buf);
    if (!root) { fprintf(stderr, "multicam: config parse failed\n"); return -1; }

    item = cJSON_GetObjectItem(root, "layout");
    if (cJSON_IsObject(item)) {
        sub = cJSON_GetObjectItem(item, "version");
        if (cJSON_IsNumber(sub)) cfg->layout_version = sub->valueint;
        sub = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(sub)) {
            snprintf(cfg->layout_type, sizeof(cfg->layout_type), "%s", sub->valuestring);
            int rows = 0, cols = 0;
            if (sscanf(sub->valuestring, "%dx%d", &rows, &cols) == 2 &&
                rows > 0 && cols > 0)
                cfg->layout_cols = cols;
        }
        sub = cJSON_GetObjectItem(item, "tile");
        if (cJSON_IsArray(sub) && cJSON_GetArraySize(sub) == 2) {
            cfg->tile_w = cJSON_GetArrayItem(sub, 0)->valueint;
            cfg->tile_h = cJSON_GetArrayItem(sub, 1)->valueint;
        }
    }
    /* 默认使用 crop_1x2 左右并排裁切（400x480 × 2 = 800x480，精确匹配 5:3 模型输入） */
    if (!cfg->layout_type[0] || !strcmp(cfg->layout_type, "crop_1x2")) {
        snprintf(cfg->layout_type, sizeof(cfg->layout_type), "crop_1x2");
        cfg->tile_w = 400;
        cfg->tile_h = 480;
        cfg->layout_cols = 2;
    } else if (cfg->tile_w <= 0 || cfg->tile_h <= 0) {
        cfg->tile_w = 848;
        cfg->tile_h = 480;
    }

    cfg->fps = 3;
    item = cJSON_GetObjectItem(root, "output");
    if (cJSON_IsObject(item)) {
        sub = cJSON_GetObjectItem(item, "fps");
        if (cJSON_IsNumber(sub) && sub->valueint > 0 && sub->valueint <= 30) cfg->fps = sub->valueint;
    }

    /* GOP 窗口解码（capture.window / capture.window_frames） */
    cfg->window_enabled = 1;
    cfg->window_frames = 4;
    item = cJSON_GetObjectItem(root, "capture");
    if (cJSON_IsObject(item)) {
        sub = cJSON_GetObjectItem(item, "window");
        if (cJSON_IsBool(sub)) cfg->window_enabled = cJSON_IsTrue(sub);
        sub = cJSON_GetObjectItem(item, "window_frames");
        if (cJSON_IsNumber(sub) && sub->valueint >= 1 && sub->valueint <= 60)
            cfg->window_frames = sub->valueint;
    }

    /* go2rtc 管理配置（run 模式）；默认值含部署环境推断 */
    {
        const char *plugin_dir = getenv("AINICE_PLUGIN_DIR");
        const char *plugin_data = getenv("AINICE_PLUGIN_DATA");
        snprintf(cfg->data_dir, sizeof(cfg->data_dir), "%s",
                 plugin_data && plugin_data[0] ? plugin_data : ".");
        snprintf(cfg->mh_yaml, sizeof(cfg->mh_yaml), "%s", "/data/plugins/mhcamera/go2rtc.yaml");
        snprintf(cfg->g2r_bin, sizeof(cfg->g2r_bin), "%s",
                 plugin_dir && plugin_dir[0] ? plugin_dir : "");
        if (cfg->g2r_bin[0]) strncat(cfg->g2r_bin, "/bin/go2rtc", sizeof(cfg->g2r_bin) - strlen(cfg->g2r_bin) - 1);
        else snprintf(cfg->g2r_bin, sizeof(cfg->g2r_bin), "/plugins/multicam/bin/go2rtc");
        cfg->base_port = 8554;
        item = cJSON_GetObjectItem(root, "go2rtc");
        if (cJSON_IsObject(item)) {
            sub = cJSON_GetObjectItem(item, "binary");
            if (cJSON_IsString(sub)) snprintf(cfg->g2r_bin, sizeof(cfg->g2r_bin), "%s", sub->valuestring);
            sub = cJSON_GetObjectItem(item, "mhcamera_yaml");
            if (cJSON_IsString(sub)) snprintf(cfg->mh_yaml, sizeof(cfg->mh_yaml), "%s", sub->valuestring);
            sub = cJSON_GetObjectItem(item, "base_port");
            if (cJSON_IsNumber(sub) && sub->valueint > 1024) cfg->base_port = sub->valueint;
        }
        if (access(cfg->g2r_bin, X_OK) != 0) {
            /* 回退：借用 mhcamera 安装目录的 go2rtc */
            struct stat st;
            if (stat("/plugins/mhcamera/bin/go2rtc", &st) == 0)
                snprintf(cfg->g2r_bin, sizeof(cfg->g2r_bin), "/plugins/mhcamera/bin/go2rtc");
        }
    }

    item = cJSON_GetObjectItem(root, "cameras");
    cJSON_ArrayForEach(it, item) {
        if (cfg->camera_count >= MULTICAM_MAX_TILES) break;
        multicam_camera_t *cam = &cfg->cameras[cfg->camera_count];
        sub = cJSON_GetObjectItem(it, "id");
        if (cJSON_IsString(sub)) snprintf(cam->id, sizeof(cam->id), "%s", sub->valuestring);
        sub = cJSON_GetObjectItem(it, "source");
        if (cJSON_IsString(sub)) snprintf(cam->source, sizeof(cam->source), "%s", sub->valuestring);
        sub = cJSON_GetObjectItem(it, "tile");
        cam->tile = cJSON_IsNumber(sub) ? sub->valueint : cfg->camera_count;
        sub = cJSON_GetObjectItem(it, "crop_x");
        cam->crop_x = cJSON_IsNumber(sub) ? sub->valueint : -1;
        sub = cJSON_GetObjectItem(it, "enabled");
        cam->enabled = !cJSON_IsBool(sub) || cJSON_IsTrue(sub);
        if (cam->tile + 1 > canvas_w) canvas_w = cam->tile + 1;
        cfg->camera_count++;
    }
    if (!strcmp(cfg->layout_type, "crop_1x2")) {
        cfg->canvas_w = 800;
        cfg->canvas_h = 480;
        cfg->tile_w = 400;
        cfg->tile_h = 480;
        cfg->layout_cols = 2;
    } else {
        canvas_w = canvas_w ? canvas_w : 2;   /* n_slots = 最大 tile 槽位 + 1 */
        int cols = cfg->layout_cols > 0 ? cfg->layout_cols : canvas_w;
        int rows;
        if (cols > canvas_w) cols = canvas_w;
        rows = (canvas_w + cols - 1) / cols;
        cfg->layout_cols = cols;
        cfg->canvas_w = cols * cfg->tile_w;
        cfg->canvas_h = rows * cfg->tile_h;
    }

    cJSON_Delete(root);
    fprintf(stderr, "multicam: config ok canvas=%dx%d tiles=%d cols=%d fps=%d\n",
            cfg->canvas_w, cfg->canvas_h, cfg->camera_count, cfg->layout_cols,
            cfg->fps);
    return 0;
}

int config_load(multicam_config_t *cfg, const char *path)
{
    char buf[16384];
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        /* 首次启动无数据配置：落盘默认配置并按默认运行（绝不退出） */
        fprintf(stderr, "multicam: 配置 %s 不存在（全新安装），写入默认配置\n", path);
        FILE *w = fopen(path, "wb");
        if (w) {
            size_t len = strlen(CONFIG_DEFAULT_JSON);
            if (fwrite(CONFIG_DEFAULT_JSON, 1, len, w) != len)
                fprintf(stderr, "multicam: ⚠ 默认配置写盘不完整\n");
            fclose(w);
        } else {
            fprintf(stderr, "multicam: ⚠ 默认配置写盘失败(%s)，仅内存生效\n", strerror(errno));
        }
        return config_parse(cfg, CONFIG_DEFAULT_JSON);
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = 0;
    if (config_parse(cfg, buf) != 0) {
        /* 配置损坏也按默认运行（不覆盖原文件，留待用户在面板重存） */
        fprintf(stderr, "multicam: 配置 %s 解析失败，按默认配置运行\n", path);
        return config_parse(cfg, CONFIG_DEFAULT_JSON);
    }
    return 0;
}
