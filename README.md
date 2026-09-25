# AINICE 多摄像头接入开发

把多台小米云摄像头接入 AINICE 视觉传感器，做分区（zone）有人检测并转发米家。
前序逆向与实机验证已完成（2026-09-16），本仓库承载后续开发。

## 设备与访问

| 项 | 值 |
|---|---|
| 硬件 | Sophgo SG2002（单核 A53 + NPU 0.5T + 硬件 VDEC/JPEG 解码） |
| 系统 | Buildroot，kernel 5.10，**无板端 gcc**，Python3 可用 |
| 地址 | 设备局域网 IP（见部署说明） |
| SSH | `ainice` / `$AINICE_SSH_PASS`（需 PTY，用 `tools/ssh-run.exp "<cmd>"`） |
| 文件 | `tools/scp-push.exp <本地> <设备路径>`（scp 需 `-O`；打包用 BusyBox `tar cf`） |
| Web | admin / `$AINICE_WEB_PASS`；`POST /api/auth/login` 返回 csrf_token，写操作带 `X-CSRF-Token` 头 |
| 预览 | `/mjpeg/stream`（需 session cookie） |
| 开发文档 | 设备上 `~/DEVELOPER.md`，SDK 示例在 `/examples`（已镜像到 `reference/device/`） |

## 目录结构

```
reference/                  逆向资料（原 /tmp/plugin-analysis，只读参考）
  device/rootfs/usr/include/ainice/   SDK 头文件（video/vision/event/api/bridge/device…）
  device/examples/          官方插件示例：c-bridge、sdk/bitstream（推流关键）、vision、presence…
  migateway/                米家网关插件完整 C 源码（插件骨架参考）
  mhcamera/ + mhcamera.dis  原厂摄像头插件逆向（拉流/token 逻辑）
tools/                      ssh/scp expect 脚本（密码经环境变量 AINICE_SSH_PASS 注入）
experiments/                2026-09-16 拼图推送实验产物
docs/                       方案与设计文档（待写）
```

## 已验证的关键接口事实

**SDK 视频**：`ainice_video_open / send_frame / close`；帧信封 `{format,width,height,size,pts,data}`；format：H264=1 / MJPEG=2 / YUV420P=3；packet 上限 8MB。MJPEG/YUV 走硬件解码。

**输入模式**：`config.patch {"key":"input.mode","value":"rtsp"|"uvc"|"bitstream"}`；bitstream 模式外部推流，H264 可任意分片，MJPEG/YUV420P 必须整帧。

**⚠️ 独占生产者锁**：视频输入是独占的。mhcamera 插件占用通道时 `video.open()` 返回 EBUSY；实验前必须先 `POST /api/plugins/disable` 停用 mhcamera，实验后 enable 恢复（自动重新接管 848×480 子码流）。

**Zone**：上限 8 个；`POST /api/zones/save` 下发 profile（input/revision/focus_zones[{id,name,color,label_points,polygon}]/suppression_areas），坐标 0-65535 归一化多边形；presence 输出固定 global + Zone-A..H 九槽（1000ms OFF 去抖）。

**REST API**：`/api/config[/save]`、`/api/zones[/save|/clear]`、`/api/rules*`、`/api/media/{runtime,metrics}`、`/api/model/runtime`、`/api/vision/snapshot`、`/api/plugins/{import,enable,disable,list,…}`。

**插件部署**：tar.gz 上传 `/api/plugins/import`；包内必须 `plugin.json` + 入口 `<id>.app`（可执行）。

**migateway 链路**：virtual_event 9 slot → MQTT action RPC（QoS0）→ 米家中枢（siid/aiid/piid）。设备另有 HA 插件把 presence 以 MQTT retained 发给 Home Assistant。

## 实验记录（2026-09-16，已通过，设备已恢复）

停用 mhcamera 约 40s，从 Mac 经 bitstream 模式推送 960×540 的 2×2 MJPEG 拼图画布 3fps × 12s：
**36 帧零失败，硬件解码，推理稳定 ~83ms/帧（NPU 76ms，≈11fps 上限，与摄像头路数无关）**。
→ 拼图合成方案在管线层验证通过，推荐 2×2 拼法（≤8 路）。

## 实施方案（待 Mark 选定）

**方案 A — 设备端 multicam 插件（正式）**：复用 `reference/migateway` 的 bridge/persist 骨架 + `device/examples/sdk/bitstream` 推流参考。插件内拉 N 路小米子码流 → 软解 → 拼 YUV420P 画布 → `ainice_video_send_frame`。难点：小米云多路拉流认证（go2rtc `xiaomi://` + passToken，或读 mhcamera 持久 token）。Zone 用 `/api/zones/save` 编程对齐 tile。

**方案 B — 外部拼流（最快验证，零插件开发）**：NAS/小主机 ffmpeg `xstack` 把多路 RTSP 拼成一路 → 设备只改 `input.mode=rtsp` + `input.rtsp_url`。前提：有常驻主机 + 摄像头可拉 RTSP（优先选支持原生 RTSP 的小米型号）。

**交叉编译**：Arm GNU Toolchain 13.2 aarch64（未安装）+ sysroot 用 `reference/device/rootfs/usr/{include,lib}`。

## 下一步

- [ ] Mark 选定方案 A / B（唯一阻塞项）
- [ ] 端到端验证：人站不同摄像头前 → 对应 zone 变有人 → 米家收到虚拟事件
