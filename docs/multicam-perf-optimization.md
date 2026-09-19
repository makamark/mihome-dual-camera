# multicam 性能优化设计（CPU / 内存）

2026-09-19 现状：**整机 CPU 90.8%，内存 136.8/157.4 MiB（87%）**（Web 系统页，0.3.4 双路 848×960@5fps）。
单核 A53（SG2002）已近饱和；157MB 总内存余量 ~20MB，接近历史 fork 饥荒红线（90%+）。
本文给出分级优化方案，含预估收益、改动面与风险。

## 0. 负载从哪来（估算，单核 A53）

| 组件 | 现状 | 估算 CPU | 估算 RSS |
|---|---|---|---|
| 双路 HEVC 848×480@**20fps** 软解（capture.c，每包必送解码器） | 源 20fps，画布只要 5fps，**4 倍浪费** | **35–50%** | ~10–12MB（每路 DPB 4–6 帧 + 槽位缓冲） |
| go2rtc ×2 实例（Go runtime + CS2 会话 + RTSP 服务） | 一路一实例 | **10–15%** | ~10–15MB×1（GOMEMLIMIT=16MiB 已设） |
| 主应用：YUV420P 收帧 + 模型缩放 + NPU 前后处理 @5fps | 推理 76ms/帧 | 15–20% | 固定（基线 ~107MB） |
| 主应用：`/mjpeg/stream` JPEG 编码 848×960 | **面板 `<img>` 从页面加载即拉流**（display:none 也在拉） | 5–15%（仅面板开着时） | — |
| multicam 自身（合成 memcpy + push + 状态） | 848×960@5fps ≈ 6MB/s | ~3–5% | ~2MB |
| 系统/其他 | — | ~5% | — |

**关键平台事实（已实测）**：插件沙箱 /dev 只有 null/zero 等 7 个基础节点，无视频设备节点，
sysroot 仅 ainice 头——**硬件 VDEC 对插件路线不可用**，软件解码只能"少解"，不能"硬解"。
SSH 沙箱无 /proc/stat、/proc/loadavg、meminfo——整机观测只有 Web 系统页一条路。

## 1. 方案矩阵

| # | 方案 | 预期收益 | 改动面 | 风险 | 优先级 |
|---|---|---|---|---|---|
| 1 | 预览懒加载（面板 MJPEG 按需拉流） | CPU −5~15%（面板开着时） | app.js/index.html ~15 行 | 无 | **P0 立即** |
| 2 | 输出 fps 5→3 | CPU −3~5% | 配置 output.fps | presence 时延 +~200ms，可忽略 | **P0 立即** |
| 3 | 插件树 renice(+10) | 不省总量，保推理时延 | main.c 3 行 | 无 | **P0 立即** |
| 4 | 面板 status 增加 RSS/解码帧计数 | 可观测性（防泄漏） | panel/main ~20 行 | 无 | **P0 立即** |
| 5 | **GOP 窗口解码**（每 GOP 只解关键帧后 m 帧，其余 AVDISCARD_NONKEY） | **CPU −25~40%（解码 ÷4~÷8）** | capture.c ~40 行 | 时延 +GOP；需实测 GOP 间隔；TILE_STALE_MS 联动 | **P1 核心** |
| 6 | **双 go2rtc 合一**（单实例双 source） | RSS −8~15MB，CPU −3~8%，进程 −1 | g2r.c/catalog.c ~80 行 | 单实例故障=双路断；重建逻辑适配 | **P1** |
| 7 | 源头降帧率（米家 App 子码流"流畅度"设置，若该型号支持） | 线性下降（15fps→−25%） | 零代码（App 设置） | 需逐型号试 | P1（先试） |
| 8 | ffmpeg 会话瘦身（probesize/analyzeduration/max_delay、收 TCP 缓冲） | 峰值 RSS −1~2MB | capture.c ~10 行 | 低 | P2 |
| 9 | GOMEMLIMIT 16→10MiB（合并单实例后） | RSS −2~5MB | g2r.c 1 行 | GC 换内存 | P2 |
| 10 | NAS ffmpeg 外部合成（方案 A 定论复活） | 设备 CPU −50%+、RSS −25MB | 独立部署 | 依赖 NAS 常开；失去单机独立 | Plan B |

## 2. P0 细节

### 2.1 预览懒加载（#1）
`www/index.html` 的 `<img id="pv-src" src="/mjpeg/stream">` 在页面加载时立即建立 multipart
连接；主应用随即持续对 848×960 画布做软件 JPEG 编码——即使预览根本没显示（CSS display:none）
或用户只是开着控制台页面。改为：
- `src` 初始为空；进入"标定顶点"模式或点"开启预览"时才赋值，退出 30s 无交互自动清空；
- `drawOverlay` 无图时只画多边形底图（灰底 + 网格），标定照常（先点"开启预览"）。

### 2.2 fps 降档（#2）
`output.fps: 3`。依据：OFF 侧去抖固定 1000ms，3fps 采样（333ms）远小于去抖粒度；
ON 侧是"持续在场"判定，不依赖 100ms 级响应。历史实测 3fps 时插件树 CPU 33.3% vs 5fps 36.1%，
主应用侧 resize/前处理同步下降。

### 2.3 renice（#3）
`main.c` 启动早期 `setpriority(PRIO_PROCESS, 0, 10)`；go2rtc 子进程 execl 后继承。
解码线程与主应用争核时让路，削 inference 尾时延。

### 2.4 观测（#4）
插件沙箱里 `/proc/self/statm` 可用：status.get 增加 `rss_kb`（multicam 自身）与每路
`decoded_frames`（capture 已有 frame_count）。24h 曲线对照，泄漏早发现。

## 3. P1 核心：GOP 窗口解码（#5）

**原理**：HEVC 子码流是 IPPP… 的 GOP 结构（子码流 keyint 通常 1–2s）。只有 GOP 首个 I 帧是
随机接入点；之后丢解任何 P 帧都会让后续参考错位，但**下一个 I 帧会完全恢复**。因此合法的
省力姿势是"窗口解码"：每个 GOP 从 I 帧起连解 m 帧（如 I+3P ≈ 200ms 的画面），GOP 内其余帧
设 `AVDISCARD_NONKEY` 让解码器只解析不输出（省掉熵解码之外的重建全流程），下一 GOP 自动干净恢复。

```
每 GOP: [I P P P] 全解（4 帧）  ────────  其余 16 帧 discard（≈0 成本） ────────→ 下一 GOP
20fps 全解 ≈ 100%   →   窗口 4 帧 = ÷5 解码量；窗口 8 帧 = ÷2.5（画质更平滑）
```

实现要点（capture.c）：
- 读循环统计 `pkt->flags & AV_PKT_FLAG_KEY` 间隔，首次连接时**实测 keyint** 打日志；
- 状态机：GOP_HEAD（全解 m 帧）→ GOP_SKIP（`dec->skip_frame = AVDISCARD_NONKEY`）→ 遇 key 帧回 HEAD；
- `capture_paint_tile` 不变（每路仍是"最新一帧"语义，只是更新频率变成 GOP 节拍）；
- **标定模式旁路**：面板标定时临时置 run-in（全解），保证预览连续流畅，退出恢复；
- 联动：`TILE_STALE_MS 3000` 需 > keyint×1.5（keyint 2s 时改 4000–5000），否则 tile 会闪灰；
  tile 帧龄显示同理解读；
- 命中条件：小米子码流 keyint ≤2s（待实测；若 4s 则窗口内帧数加半，收益仍 ≥÷2.5）。

**多路扩展的战略意义**：2×2（≤8 路）方案里推理成本与路数无关（NPU 76ms 恒定），
瓶颈全在"每路 20fps 软解"。20fps×4 路全解在单核上是绝对不可行的；GOP 窗口解码后
每路边际成本 ÷5，**这是多路方案的门票**，而不只是省电。

时延代价：画面/检测采样节拍变成 GOP 级（1–2s）。presence 是"持续在场"语义 +
1000ms 去抖，可接受；ON 事件最坏多 ~2s。

## 4. P1：双 go2rtc 合一（#6）

现状每路一个 go2rtc 实例的根源是端口硬编码 `video:8554`（已破解为逐实例补丁 `855{4+i}`）。
但单实例本可承载多路 source（`streams: {cam-a: xiaomi://…, cam-b: xiaomi://…}`），
RTSP 拉流端按 path 取流。收益：
- 少一个 Go runtime（RSS −8~15MB）、少一份 CS2 云会话保活（CPU −3~8%）、进程数 −1（fork 余量）；
- kill_stale/补丁副本/互相误杀一类问题面整体消失一半。

改动：g2r.c 生成单实例 yaml（多 source），caps 的 url 改 `…/cam-a|cam-b`；watcher 从
"每槽位一实例"改为"共享实例 + 单路 source 重建"（POST /api/camera/source 单独增删）。
风险：实例级故障双路同断（原本就同生共死于同一 token，实际风险变化不大）。

## 5. 预期汇总

| 阶段 | 整机 CPU | 内存 | 说明 |
|---|---|---|---|
| 现状（0.3.4 双路@5fps） | 90.8% | 87% | 近饱和 |
| +P0（#1–4） | ~80–85% | 87% | 立竿见影，半小时改动 |
| +P1（#5 GOP 解码 + #6 合一 + #2 已含） | **~45–55%** | **~76–80%** | 结构性缓解 |
| +Plan B（NAS 合成） | ~30–40% | ~70% | 多路扩展不再受限 |

## 6. 验证方法

- 基线/每步改动后：Web 系统页 CPU/内存读数 ×24h（早晚各一张）；
- `/api/media/runtime`：推理 fps、NPU 时延不回退（76ms 基线）；
- 面板 status.get：帧龄、rss_kb、reconnects 无异常增长；
- presence 端到端：走进/走出 Zone-A/B，米家事件时延手感对照；
- GOP 实测：capture 日志 keyint 打印值。

## 7. 实施顺序建议

1. **当天可做**：#2 配置降 fps → #1 预览懒加载（发 0.3.5）；
2. **一个晚上**：#5 GOP 窗口解码（先加 keyint 实测日志跑一天）→ #3/#4 顺手带上（0.3.6）；
3. **第二个晚上**：#6 go2rtc 合一（0.3.7，改动最大、单独发版便于回滚）；
4. **随时**：#7 米家 App 子码流设置试探（零风险）；
5. Plan B 保持备胎：若 P1 后 CPU 仍 >65% 或要上 4 路。
