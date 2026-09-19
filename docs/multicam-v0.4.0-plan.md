# multicam v0.4.0 方案：去掉 areas、摆脱 mhcamera 运行依赖、内存优化

2026-09-19。三个目标：①设备侧 zone 成为唯一事实源，插件不再存 areas；②新设备部署只装
multicam 一个插件（mhcamera 仅授权时临时在场）；③内存再挤一轮。均已验证前提，未实施。

## 一、去掉插件内 areas（zone 单一事实源）

**现状问题**：插件配置存一份 areas（tile 本地坐标）+ 启动时 zones.save 下发（该 RPC 链路
本来就不通）；总览界面画的 zone 是另一条路。两份记录迟早打架。

**方案**：
- 配置删除 `areas`、`zones` 两个段（config.c / multicam.h / config_validate 同步删；
  旧配置里的残留键直接被忽略，面板下次保存即清理）。
- 删除 zones.c/zones.h 及 main.c 的同步调用；decode.full 标定旁路一并清理
  （没有标定就没有旁路需求，bridge 方法与 panel 调用都删）。
- 面板"实时画面与检测区域"卡改为**只读叠加**：直接 `fetch('/api/zones?input=bitstream')`
  （同源 GET 无需 CSRF），`x/65535*canvas_w` 反归一化画多边形，颜色/名称用 profile 里的
  zone.color/zone.name——面板看到的即检测实际看到的，比旧 areas 叠加更可信。
- 区域绘制统一走设备总览界面（唯一写入方）；槽位与米家映射的对应关系在插件映射表维护。

**影响**：插件侧少一块 UI 和一条失效链路；换布局（layout.type）后区域错位需在总览重画
（设备侧坐标绑定画布几何，本来如此）。

## 二、新设备免单独安装 mhcamera

**前提修正（2026-09-19 Mark 确认）**：mhcamera **不是出厂件**，是 Mark 后续自行安装的
（subsys-rootfs.tgz 是设备状态快照而非出厂清单，后装插件都在其中——勿再误判）。
新设备上没有 mhcamera，而授权（短信验证码）硬依赖 mhcamera.app 在场：vendor go2rtc
的 xiaomi-phone API 有属主门（反汇编 0x337370 三重门，f(req) 签名未破），复刻协议已被
否决，mhcamera.app 是唯一能过的受支持路径。

**运行依赖其实只有一个：token 源**（g2r.c:196 唯一调用点，每次实例重建抽 mh yaml）。
而 multicam 的 go2rtc 实例运行中**自续 token 并写回自己的 go2rtc-N.yaml**（vendor 行为，
已确认）——mhcamera 停用期间它的 yaml 不续期，我们实例自己的副本才最新。

**改造分两层**：

*层 1（必做）：token 自持* —— 与原方案一致：
1. 抽取顺序：实例自身 `go2rtc-{i}.yaml`（自续，最新）→ `data_dir/xiaomi-token.yaml`
   （授权后快照）→ mh yaml（回退）→ 失败报错；
2. 授权完成后自动从 mh yaml 落一份自有快照（g2r.c 内做）。

*层 2（可选，实现"只装一个包"）：内嵌授权组件自动导入*：
- 构建时把 mhcamera 的 .plugin 包（从设备 `/plugins/mhcamera/` 重构：plugin.json +
  mhcamera.app + bin + libs，tar.gz ≈9-10MB）作为资产放进 multicam 的 www/；
- 面板授权卡升级：检测 mhcamera 未安装 → "导入授权组件"按钮 → 浏览器
  `fetch('/plugins/multicam/assets/mhcamera.plugin')` → 原始字节 POST
  `/api/plugins/import`（面板在已登录 iframe 内，有 cookie/CSRF；Content-Type 用
  vnd.ainice.plugin 原始流，multipart 会 8116）→ enable → 短信验证（现有 v0.3.3 编排）
  → token 迁移确认 → disable；
- **卸载编排（可选按钮）**：仅在 multicam 双路 live + token 快照确认后提供 uninstall
  （时序错会丢 token）；不做也行——mhcamera 留 disabled 只占 18.7MB 磁盘不占内存；
- 代价与风险：multicam 包 4.5MB→~14MB，/plugins 分区配额（历史 8104 大包坑）需实测；
  再分发厂商二进制的合规自担（自用场景）。

**新设备交付形态**（层 1+2）：拿到一个 multicam.plugin → 导入 → 授权卡自动装授权组件、
短信验证、自动停回 → 完成。mhcamera 全程不被用户感知。
（只做层 1：用户需手动装一次 mhcamera 走授权，之后停用即可。）

**验证**：把 mh yaml 改名藏起 → 触发实例重建 → 拨号仍 running、token 来自自身副本；
新设备模拟（卸载 mhcamera → 面板自动导入 → 授权 → 双路 live）。

**外部风险**：厂商未来改动 mhcamera 或 xiaomi-phone 门，内嵌包版本可能失配——升级时
需重新导出重构资产。

## 三、内存优化（现状 135MiB/157.4MiB，free ~22MiB）

**拆解（可归因部分）**：主应用基线 ~107MiB（模型/NPU/服务，动不了）+ multicam 树 ~29MiB
（multicam.app 11.6MiB：双路解码 DPB ~6MiB + 画布 1.2MiB + 杂项；go2rtc ×2 ~8.5MiB/个）。

**做**：
1. **观测补齐**：status.get 增加各 go2rtc 子进程 RSS（fork 出的子进程在本命名空间，
   /proc/<pid>/statm 可读）——先看清再调；
2. **GOMEMLIMIT 12→8MiB 实验**（×2 实例，预期 −4~6MiB）：CPU 现在有大量余量
   （~27%），GC 频率换内存划算；跑 24h 看 GC 抖动是否推高 CPU 超过 35%；
3. ffmpeg 会话参数压缩（probesize/analyzeduration 减启动瞬时峰值，~1MiB，顺手）。

**不做（已论证排除）**：
- DPB 减半：HEVC 的 DPB 由 SPS 决定，ffmpeg 无公开开关，改需 patch 解码器——不值；
- 自编译裁剪 ffmpeg：RSS 只算触达页，未用代码页不计入，收益 << 构建维护成本；
- go2rtc 合并实例：vendor 裁剪版无 streams/标准 RTSP，已实验否决（0.3.7 记录）；
- 自研 CS2 客户端：授权签名门未破，工程量与风险不成比例；
- zram/swap：需固件级 root，该路线 2026-09-16 已否决。

**结构性出路（唯一大额）**：NAS/主机外部合成（原方案 A）——设备只留推理，内存树整体
-25MiB 以上。保持 Plan B。

**预期（诚实）**：插件侧只能再挤 ~5MiB（free 22→~27MiB）。好处主要是把 fork 饥荒红线
（历史事故在 used>90%）拉远，以及观测到位后异常早发现；想质变只能 Plan B。

## 实施切版

一个版本 **0.4.0**（含一二三全部）：删 areas/zones/标定旁路 → 面板只读叠加 → token 自持
→ go2rtc RSS 观测 + GOMEMLIMIT 8MiB（带 24h 验证，超限回 12）。
**验证清单**：导入新设备模拟流程（藏 mh yaml 重建实例）；面板叠加与总览所画一致；
/api/zones revision 只被总览改动；双路 live + CPU/内存 24h 曲线；米家映射保存正常。
**回滚**：0.3.7 包保留，行为差异仅面板与配置段，无数据风险。
