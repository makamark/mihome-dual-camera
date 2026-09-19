# 架构咨询：改固件 vs 开发插件（提交给 Codex 评审的材料）

## 需求

AINICE 视觉传感器接入**至少 2 台小米云摄像头**，各自配置**独立检测区域（zone）**，判断各区域内是否有人，presence 结果转发到米家。

## 硬件与系统（已实测确认）

- SoC：Sophgo SG2002，Linux 跑在 Cortex-A53（单核可用），NPU 实测推理一帧 ~83ms（其中 NPU 76ms，≈11fps 上限）
- 硬件解码：VDEC（H264）+ JPEG 硬解，由厂商主应用内部使用；**插件进程能否直接访问 VDEC 未确认**
- 系统：Buildroot，内核 5.10，**无板端 gcc**，有 Python3
- 厂商主应用（视觉管线）为**闭源二进制**；无固件源码、无 Buildroot 构建环境，只有 rootfs 文件系统镜像

## 主管线架构（逆向 + 实机验证）

- 视频输入是**独占生产者**（第二家 open() 返回 EBUSY），三种输入模式：
  - `rtsp`：设备自己拉一路流
  - `uvc`：USB 摄像头
  - `bitstream`：外部进程经 SDK 推流（H264 可任意分片；MJPEG/YUV420P 须整帧）
- SDK C API（头文件已提取到 reference/device/rootfs/usr/include/ainice/）：`ainice_video_open/send_frame/close`，帧信封 {format,width,height,size,pts,data}，H264=1/MJPEG=2/YUV420P=3，packet ≤8MB
- 推理固定对**单路输入画面**进行，输出 global + Zone-A..H 九槽 presence；zone 上限 8 个，多边形归一化坐标，REST `/api/zones/save` 可编程下发
- 推理成本与画面内容无关：不管画布里是 1 台还是 4 台摄像头的画面，都是 ~83ms/帧

## 插件体系（已验证）

- 插件 = tar.gz（plugin.json + 入口 `<id>.app` 可执行），REST `/api/plugins/import` 上传，可 enable/disable
- **migateway 插件完整 C 源码已在手**（bridge/persist/MQTT 转发米家），可作骨架
- 原厂 mhcamera 插件：从小米云拉 848×480 子码流、占用视频输入、token 持久化机制已逆向（mhcamera 反汇编 + www 前端 JS 在手）

## 已通过的实验（2026-09-16）

停用 mhcamera 后，外部进程经 bitstream 推送 960×540 的 2×2 MJPEG 拼图画布 3fps×12s：**36 帧零失败，MJPEG 走硬件解码，推理稳定**。即"多路画面拼成一张画布送推理"的管线层已验证可行。

## 待决策的两条路径

**路径一（改固件）**：二进制 patch 厂商主应用/系统，使其原生支持多路输入（如多 RTSP 源多画布）。

**路径二（开发插件）**：新增 multicam 插件：进程内拉 2+ 路小米子码流 → 软解 → 拼画布 → `ainice_video_send_frame` 推 YUV420P（或拼好后编码 MJPEG 走硬解）；zone 用 REST 对齐各 tile；米家转发继续走现有 migateway 插件，不重复造。

## 请回答

1. 明确推荐哪条路径？从可行性、风险（变砖、OTA 覆盖、可维护性）、工作量、需求匹配度逐项给出理由。
2. 若推荐插件：单核 A53 上软解 2 路 640×360 H264 子码流 + YUV 拼接 + ~10fps 推送的 CPU 预算是否够？给出模块划分与里程碑切分。
3. 最大技术风险是什么（尤其小米云拉流认证），有什么已验证的绕行方案？
4. 是否存在第三条更优路径？

本目录 `reference/` 下有 SDK 头文件、官方示例（device/examples/sdk/bitstream、vision、presence）、migateway 完整源码，可自行查阅验证后再下结论。

---

# 评审结论（2026-09-16，Codex 独立评审 + 人工复核）

## 判断：**不改固件，开发插件**

改固件/二进制 patch 厂商主应用被否决：闭源无构建环境、变砖与 OTA 覆盖风险、工作量无边界（持续逆向）；
且需求本身不需要主应用理解多摄像头——拼图布局 + zone 稳定映射即可满足"两路各自独立检测区域"。

若接受一台常开 NAS/小主机，外部 ffmpeg 合成一路 RTSP（前序方案 B）仍是**交付最快**的路径；
若要求单机独立运行，则开发 multicam 插件（方案 A），**以双路 3–5fps presence 起步验收，不按 10fps 排期**。

## 关键新发现：直接复用设备上的定制 go2rtc

`reference/mhcamera/bin/go2rtc`（linux-arm64，上游 v1.9.14）+ `reference/mhcamera/legal/go2rtc/BUILD-PROVENANCE.json`
显示原厂已解决最难的小米认证链路，补丁包括：
- 0003 小米 Passport 登录 + 轮换 token 交接
- 0004/0005 设备列表分页、房屋/房间元数据（摄像头发现）
- 0006 云会话续期、原子化 token 持久化
- 0007–0009 CS2（小米摄像头流协议）命令排空/帧安全/关闭消息
- 0010 小米源以**本地 RTSP/TCP 输出**（默认关闭、认证、单观看者）

→ multicam 插件可把此二进制作为子进程跑第二实例（独立端口 + 独立配置），拉 2 路小米流，
插件内软解 → 拼 YUV420P → `ainice_video_send_frame`。**M0 首项验证：该二进制能否独立跑双 xiaomi 源。**

## 里程碑（Codex 建议，人工采纳）

| 阶段 | 内容 | 通过门槛 |
|---|---|---|
| M0 双路取流 | go2rtc 独立实例、token 续期、两个流会话 | 两台摄像头同时取流；核实实际 codec/分辨率/fps；重连与续期 |
| M1 性能样机 | 软解、最新帧缓冲、YUV 合成、唯一 producer | 3/5/10fps 分档测 CPU/内存/温度/帧龄，持续运行不积压 |
| M2 区域闭环 | 摄像头→tile→zone 固定映射 + migateway 事件 | 无人/仅A/仅B/都有 四态正确到达米家 |
| M3 可靠性 | 断流、过期帧、重启恢复 | 24–72h 运行，断网/摄像头重启后自动恢复不串区 |

工程门槛：全机持续 CPU ≤70–75%（为重连留余量）。媒体路径用 C/C++ 交叉编译，Python 只做验证/配置。

## 风险清单

1. **双路小米认证长期稳定性**（最大风险）——go2rtc 复用是捷径但未实测多实例
2. **拼图后识别质量**——tile 缩小后远/小人检出率下降，需对照模型输入尺寸验证；紧凑布局优于留空 tile
3. **断流语义**——冻结帧会假报"有人"，填黑会假报"无人"；migateway 现有代码不检查帧新鲜度，可能需小幅扩展有效性门控
4. 注意：bitstream 示例 README 称"新会话可接管"，与实测 EBUSY 不符——**以实测为准，显式停用 mhcamera 再接管**
5. 源流实为 848×480（非 640×360）；输出 3fps 不减少 H264 解码量（解码在先，丢帧在后）
