# AINICE .ota 固件包格式规范（黑盒逆向，2026-09-17）

来源：对官方包（2026.09.04.1 / 2026.09.15.2 两版）的字节分析 + 对设备流式验证器的
21 次构造包实验（错误信息 oracle）。本文档是自定义固件的格式基础。

## 1. 容器层

`.ota` = POSIX **ustar** tar（无压缩），成员**顺序锁死**（错序/缺员/多员/软链/PAX 头一律拒绝）：

```
1. manifest.json        （第 0 块，99 字节级别的小 JSON）
2. encinfo.bin          （72 字节，每包随机 —— 密钥封装材料）
3. SHA256SUMS           （文本，"hex␣␣name\n" ×4 行，覆盖其余全部成员）
4. SIGNATURE            （71 字节 DER，ECDSA P-256 over SHA256SUMS 原文字节）
5. rootfs.spinand.enc   （= plain_size + 16×ceil(plain_size/262144)）
6. boot.spinand.enc     （同上公式）
```

实测拒收案例：缺 SIGNATURE → `ota tar member order invalid`；
成员名改为 rootfs.spinand（无 .enc）→ 同上；成员类型非普通文件 → 拒。
（macOS 打包必须 `--format=ustar`；bsdtar 默认 PAX 会触发 order invalid。）

## 2. manifest.json

```json
{"storage":"spinand-ab","version":"2026.09.15.2","plain_sizes":{"rootfs":34996224,"boot":2719684}}
```
- 官方版带**尾随 `\n`**（99 字节）。
- `version` 必须**严格大于**设备当前运行版本（同版本、旧版本均报 `ota manifest invalid`）。
- `plain_sizes` 必须与成员尺寸满足开销公式（下节），不符报 `ota manifest invalid`。
- 版本号格式 `YYYY.MM.DD.N`（四段数字）。

## 3. 载荷加密层（rootfs.spinand.enc / boot.spinand.enc）

**分块流加密**：明文按 **262144 B（256KB）** 分块，每块前缀 **16 字节**头部（推测 nonce/IV），
密文块长 = 明文块长（无填充，CTR/OFB/CFB 类）。总尺寸公式：

```
enc_size = plain_size + 16 × ceil(plain_size / 262144)
```

两版官方包双重验证：rootfs 34,996,224 → +2,144（134 块）；boot 2,719,684 → +176（11 块）。
明文目标格式：rootfs 为 squashfs（`hsqs` 魔数，CVITEK spinand 常规），boot 为 UBI 镜像（`UBI#`）。

**encinfo.bin**：72 字节高熵。已排除裸 key/IV 直存（对 16/24/32 位密钥 × CBC/CTR/ECB ×
全滑窗位置共 10,825 组合暴力验证，无一命中 `UBI#`/`hsqs` 魔数）。每包全新随机 →
内容密钥逐包生成，encinfo = 密钥封装（KEK/设备侧密钥在 sys scope 更新器内）。

## 4. 验证管线（流式，边收边验，包不落盘）

| 阶段 | 检查 | 失败错误 |
|---|---|---|
| 1 | tar 成员序列（名字+顺序+类型） | `ota tar member order invalid` |
| 2 | manifest 解析 + 版本严格更新 + plain_sizes 与成员尺寸公式一致 | `ota manifest invalid` |
| 3 | SIGNATURE 用固定公钥 ECDSA-P256 验签（覆盖完整 SHA256SUMS） | `ota signature invalid` |
| 4 | （推测）逐成员 sha256 对账 SHA256SUMS | 未单独观测 |
| 5 | encinfo 解封装 → 分块解密 → 写入非活动 slot | 未观测到 |

观测法：HTTP 409 响应体 `last_error` 字段即当前阶段错误；上传中断报
`ota stream aborted`。进度粒度：`progress.phase` 2=validating（后续 writing/syncing/
verifying/erasing/finalizing 见 Web JS 文案）。

## 5. 签名验证器特性（ECDSA 容忍度实测）

- raw r||s (64B)：拒
- high-s 翻转：拒
- BER 长度扩展 (81 81)：拒
→ 标准 OpenSSL 式严格 `ECDSA_verify`，无实现层弱点。公钥在 sys scope 更新器二进制内。

## 6. 自定义固件的三道墙与对应路径

| 墙 | 性质 | 路径 |
|---|---|---|
| W1 签名（固定 P-256 公钥） | 密码学 | ①拿到更新器二进制做验证逻辑 RE（找绕过/公钥位置）②UART+uboot 直写 flash **完全绕开 OTA 层** ③在线模式 MITM（HTTP 明文，但在线流程大概率同样验签，未测——需要 ARP/DNS 欺骗，局域网侵入性强，未获授权不做） |
| W2 encinfo 密钥封装 | 密码学 | 同①；或 UART 直写**明文** rootfs（若 at-rest 为明文——大概率成立，否则 A/B 回滚需持久密钥上下文） |
| W3 读取 sys rootfs（更新器二进制所在 ubi0/mtd7） | 权限 | root / mknod（无 devices cgroup，root 即全通）/ UART dump |

**结论：最短路径 = UART/uboot 直写明文镜像，一次性绕过 W1+W2+W3。软件路径全部依赖
先拿到更新器二进制（回到 W3）。**

## 7. 实验记录索引（可复现）

- 构造器模式：python `tarfile`（USTAR_FORMAT）+ `cryptography` P-256 自签
- 已测样本：/tmp/oracle/（p1-p13, q1-q3, t1-t4）；关键对照：p4（尺寸公式）p6b（换行+新版本→过 manifest）p7（缺签名→order invalid）p9（plain 成员名→order invalid）p10（官方版本号→manifest invalid）p11/p12（版本规则）p13（签名覆盖）
- 官方包参照：`reference/firmware/extracted{,-0904}/`（两版，SHA256SUMS/SIGNATURE/encinfo 齐全）

## 8. codex 复核意见（2026-09-17，外部模型审查）

对本文档的**修正/降级**（原结论 → 应表述为假设）：
- 每块 16B 开销"推测是 nonce" → 也可能是 **AEAD tag**（隐式 nonce/末尾 tag），未能区分
- "encinfo 每包随机 → 内容密钥逐包随机" → **不成立**：随机化封装可以每次包同一个密钥
- 滑窗暴力只排除了已测布局/模式，未排除所有明文密钥或派生密钥构造
- 载荷镜像类型（squashfs vs UBI vs FIT+厂商包装）在拿到明文前都保留为备选

**encinfo 72B 的长度相容假设**（按可判别证据排序，均未证实）：

| 假设 | 解释 | 判别证据 |
|---|---|---|
| XChaCha20-Poly1305 封装 32B 密钥 | 24 nonce + 32 ct + 16 tag = 72 | 更新器里的 API/常量 |
| AES-KW 封装 64B 材料 | 64 + 8 开销 = 72 | RFC3394 解包完整性检查 + KEK |
| salt + AES-KW(32B key) | 32 salt + 40 wrapped | KDF 与 key-wrap 调用点 |
| 自定义信封 | version/keyID/salt/IV/ct/tag | 大样本语料中的稳定字段 |

**已知明文攻击的现实评估**：CTR 类流加密下已知明文只能恢复对应位置的 keystream，
不能恢复密钥、不能解其他块/版本；只有发现 **nonce/密钥重用或弱生成器** 才有价值。
拿到一份明文（恢复介质或 flash dump）后做跨版本对齐比对才有意义，值得 time-box 但
不值得为它收集几十个版本。已确认 OTA 服务器仅存 2026.09.04.1 与 2026.09.15.2 两版
（又探 15 个候选版本号全 404）。

**本地存档复核**：完整 SUBSYS 镜像 + mhcamera/migateway 二进制中均无 OTA 错误字符串
/ spinand-ab 常量 → 更新器代码确实只在 sys scope。

**uboot 是否解析 .ota**：无证据表明 boot 链解析此容器（签名验证在用户态更新器）；
uboot 的 verified-FIT 是否启用需看 boot 产物。→ **uboot 直写明文镜像的路线不受 OTA
签名墙影响**（除非 FSBL/uboot 层另有 FIT 验证，UART 首次接入时验证）。

**MISC 分区**：先按 Android bootloader_message 假设检验（offset 2048，magic
`0x42414342`，slot 元数据 + CRC32）；注意只读检验，某些读取操作会动 tries 计数器。

**优先级建议（codex）**：签名格式探测收益已尽；下一步最大价值 = ①UART 启动抓取 +
可重复的小块 SYS 读取（mtd/nand read + tftpput/md.b）②向厂商索取工厂恢复镜像 +
开源组件包（索取话术见 codex 原文，docs 里留档：要求 recovery/factory image、
Linux/U-Boot 源码与板级配置、离线 OTA 校验工具、SYS scope 开发文档）。
拿到更新器后注意：解密可能委托给共享库/守护进程/硬件密钥服务，届时跑通原装解密器
可能比抠密钥更实际。
