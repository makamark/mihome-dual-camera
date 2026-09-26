# 米家双路摄像头（Mi Home Dual Camera）

把两路摄像头画面合成为一路，接入 AINICE 视觉传感器的分区有人检测，检测结果沿设备原有通道输出给米家 / Home Assistant。

一个插件解决三件事：**拉流**（米家账号授权后本地直连，或任意 RTSP 摄像头直连）、**合成**（左右并排 5:3 满幅画布）、**接入检测**（推送设备视觉管线，检测区域在设备「总览」界面绘制）。

## 功能

- **双路合成**：两路画面各取 400×480，左右并排成 800×480 画布（5:3），与设备检测模型的输入比例完全一致——全画面零盲区、零拉伸、零时延。
- **米家接入**：面板内完成账号授权（手机号 + 短信验证码），凭证由插件自持并在运行中自动续期，无需安装其他摄像头插件。
- **自行接入**：每路可切换为 `rtsp://` 直连（海康 / 大华 / TP-Link 等标准 RTSP 摄像头或 NAS 流媒体），填入地址即时生效，无需保存。
- **取景器**：面板内嵌每路完整原画，拖动蓝色取景框选择画面范围，支持靠左 / 居中 / 靠右预设，所见即所得。
- **检测区域**：在设备官方「总览」界面的合成画面上直接绘制 Zone-A..H，左半边对应第 1 路、右半边对应第 2 路。
- **可视化面板**：实时预览、每路状态与离线原因、帧率调节、深浅色跟随系统。
- **可靠运行**：首次启动自动生成默认配置，秒级就绪；单路离线自动降级置灰并在画面恢复后自动亮起；会话断流自动重连、实例崩溃自动重建。

## 安装

1. 在设备 Web 控制台 **插件 → 导入** 上传 `multicam-0.5.13.plugin`（也可以 `POST /api/plugins/import` 上传原始字节流，`Content-Type: application/vnd.ainice.plugin`）。
2. 启用插件。首次启动会自动生成默认配置，一两秒内进入就绪状态。
3. 控制台打开插件面板 → **账号授权** → 输入手机号与短信验证码完成米家授权。
4. 在每路的设备下拉中选择米家摄像头；或切换「自行接入」填入 `rtsp://` 地址。
5. 在设备官方「总览」界面上，对合成画面绘制检测区域。

## 使用注意

- **一台摄像头同一时间只能被一台设备拉流。** 米家摄像头对并发视频会话会做轮换，两台设备同时拉同一台摄像头会导致画面异常，请先停用另一台设备上的拉流。
- 启用本插件后，设备的视觉检测输入即切换为合成画面；停用插件后自动恢复设备原有摄像头画面。
- RTSP 源要求画面不小于 400×480（480p 及以上），取景裁切为原样裁切、不做缩放。
- 设备内存有限，输出帧率建议保持默认 3fps。
- 合成画面会替换设备原摄像头画面参与检测；单独某一路离线不影响另一路检测。

## 工作原理

插件随包自带 go2rtc（启用米家源）与 FFmpeg 运行库：米家授权完成后经云端密钥交换与摄像头建立本地直连会话，拉取 848×480 HEVC 子码流；FFmpeg 软解并按取景框裁切合成 YUV420P 画布，经设备 SDK 以 bitstream 模式推送视觉管线；go2rtc 会话由守护线程自动重建，凭证运行中自动续期并原子持久化。

## 从源码构建

仓库提供交叉编译环境（`build-env/`）。两个第三方二进制因许可与体积原因不随仓库分发，构建前需自行放置：

- go2rtc（在其基础上启用米家源与本地 RTSP 输出的构建）→ `reference/mhcamera/bin/go2rtc`
- aarch64 FFmpeg 库（libavcodec / libavformat / libavutil）→ `thirdparty/ffmpeg-aarch64/lib`

```bash
docker build -t ainice-build build-env
docker run --rm -v "$(pwd)":/work ainice-build make -C multicam package
```

产物为 `multicam/multicam-<版本>.plugin`。

## 目录结构

```
multicam/     插件 C 源码、Web 设置面板、打包配置
release/      release.json（版本号与更新日志）
build-env/    交叉编译 Dockerfile
docs/         设计文档
tools/        设备部署辅助脚本
```

## 致谢与许可

- [go2rtc](https://github.com/AlexxIT/go2rtc)（MIT）— 流媒体核心
- [mhcamera](https://github.com/wade-hello/mhcamera)（MIT）— 米家授权实现（`src/vendor/xiaomi_api_client`）
- [FFmpeg](https://ffmpeg.org)（LGPL）— 视频解码
- [cJSON](https://github.com/DaveGamble/cJSON)（MIT）— JSON 解析
- AINICE SDK 与设备平台 — 设备厂商提供

本项目为个人设备使用而开发，与小米、AINICE 官方无关。
