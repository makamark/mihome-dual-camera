#ifndef MULTICAM_CATALOG_H
#define MULTICAM_CATALOG_H

#include "../src/vendor/cJSON.h"
#include <stddef.h>

/* 米家设备目录：经 go2rtc xiaomi 模块的 API 查询账号设备列表
   （含分页 start_did/next_start_did 与 localip 解析）。
   参考 mhcamera：设备列表来自云目录，localip 由云在局域网时下发。 */

/* 确保发现实例（仅 xiaomi 模块，无 camera 源）在运行；
   使用未补丁的 go2rtc 二进制，sock 为 {data_dir}/tmp/g2r-disc.sock。 */
int catalog_ensure_instance(const char *data_dir, const char *mh_yaml,
                            const char *g2r_bin, char *err, size_t err_cap);

/* 拉取设备目录，返回 cJSON 数组：[{id,name,model,localip},...]
   失败返回 NULL（err 填充原因）。调用方负责 cJSON_Delete。 */
cJSON *catalog_fetch_devices(const char *sock, char *err, size_t err_cap);

/* 从目录数组中按 did 查找设备，返回设备对象（无所有权），找不到返回 NULL。 */
cJSON *catalog_find_device(cJSON *devices, const char *did);

/* 停止发现实例（目录查询完成后调用，回收 ~14MB 内存）。 */
void catalog_stop_instance(const char *data_dir);

#endif
