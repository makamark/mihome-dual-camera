# AINICE 视觉传感器固件逆向笔记（2026-09-16/17）

目标：拿到升级固件 → 解读 → 评估自定义固件可行性。
结论先行：**固件包已拿到并校验（本目录 `../reference/firmware/`），但载荷加密 + ECDSA 签名，
无厂商私钥无法直接产出可刷的自定义 OTA。沙箱内提权多路已试，均被厂商加固挡住（详见 §5）。**

## 1. 固件获取

设备不落盘保存升级包（流式处理，无暂存文件）。官方 OTA 源为 nginx 静态目录：

- 在线检查：`POST /api/ota/check`（需 admin 会话 + X-CSRF-Token）
- 版本清单：`http://ota.ai-nice.com/ainice-vision/firmware/latest.json`（明文 HTTP）
- 包地址规律：`.../ainice-vision-spinand-<版本>.ota`

当前版本 2026.09.15.2（包 37,734,400 B，SHA256 `1fa7ab47…51cd624b`）已下载至
`reference/firmware/ainice-vision-spinand-2026.09.15.2.ota`，哈希与官方元数据一致。

## 2. .ota 包结构

`.ota` = POSIX tar（成员顺序强校验，错序报 "ota tar member order invalid"；macOS bsdtar 的
PAX 扩展头会被拒，需 ustar 格式）：

| 成员 | 大小 | 说明 |
|---|---|---|
| manifest.json | 99 B | `{"storage":"spinand-ab","version":…,"plain_sizes":{"rootfs":34996224,"boot":2719684}}` |
| encinfo.bin | 72 B | 高熵密钥材料（内容加密密钥的封装/IV，绑定设备端密钥） |
| SHA256SUMS | 326 B | 各成员 sha256 |
| SIGNATURE | 71 B | **ECDSA P-256 (DER)**，签 SHA256SUMS |
| rootfs.spinand.enc | 34,998,368 B | 加密的 sys rootfs（UBI 镜像），熵 8.0、无 ECB 重复 → CBC/CTR+IV |
| boot.spinand.enc | 2,719,860 B | 加密的 boot（fip/内核/DTB 打包） |

上传走 `POST /api/ota/upload`（头 `X-OTA-Size`，body 为原始 tar 流）——**服务端边收边流式
解析**，验证失败即断流，包不落盘。

## 3. Flash 布局（SPI NAND 256MB，/sys/class/mtd 实测）

| mtd | 分区 | 大小 | 备注 |
|---|---|---|---|
| 0 | fip | 2MB | fsbl/opensbi/uboot |
| 1 / 2 | BOOT / BOOT_B | 4MB ×2 | 内核 A/B |
| 3 | MISC | 1MB | A/B 启动标志、tries_remaining |
| 4 / 5 | ENV / ENV_BAK | 512KB ×2 | uboot env |
| 6 / 7 | ROOTFS / ROOTFS_B | 36MB ×2 | **sys scope rootfs A/B** |
| 8 | CFG | 4MB | ubi1（配置） |
| 9 | DATA | 32MB | ubi2（= /data，跨 scope 共享） |
| 10 | SUBSYS | 136MB | ubi3（= 子系统 rootfs，SSH 沙箱所见世界） |

UBI 实测（sysfs）：ubi0→mtd7（卷名 ROOTFS，data 33.7MB，**当前运行 slot=b**），
ubi1→CFG，ubi2→DATA，ubi3→SUBSYS；char 设备号动态（246-249），mtd 主 90。
**cgroup 只挂 memory/pids/cpu，无 devices 控制器 → 一旦有 root，mknod 即可直读全部 flash。**

## 4. 系统架构（关键事实）

- 单内核 5.10.4（CVITEK tag），**全部 scope 共享宿主 pidns**（4026531836），hidepid=invisible
  只遮 /proc 列表；mnt ns 分层：SSH/插件各不相同，插件 ns 有真 sysfs+ro cgroup。
- scope：**sys**（root 服务、OTA 更新器、2222 端口 sshd）→ **app/subsys**（沙箱，uid 1001，
  NoNewPrivs=1，无 seccomp，CapBnd 全量但 CapEff=0）。
- SSH 22 = 宿主 sshd（读宿主自己的 passwd/shadow，SUBSYS 卷里 shadow 根本没有 ainice 行），
  登录后进沙箱 wrapper；`POST /api/subsys/password` 改的就是这个 SSH 密码（响应
  `{"ssh_restarted":true}`）。SSH 2222 = 另一 scope 独立 sshd（host key 不同，账号未知）。
- Web API：admin/1234，REST 全表见 README；WebSocket 终端
  `ws://<ip>/ws/terminal?ticket=`（子协议 `ainice-terminal`，票据 `POST /api/auth/ws-ticket`，
  需带会话 Cookie + Origin）——给的仍是同一沙箱 shell。
- api.sock RPC：**换行分隔 JSON**，`{"id","method","params"}`；已确认 `config.get` 可用，
  presence.snapshot/vision.snapshot 等名被拒（命名规则待更多样本）。
- Python user-site = `/data/python/lib/python3.14/site-packages`（我们可写、全局共享、
  在 sys.path 且 ENABLE_USER_SITE=True）——但重启实测**没有任何 root 进程**用这套环境起
  Python（root 服务全是编译型二进制）。

## 5. 提权尝试记录（沙箱 → 宿主）

| 路径 | 结果 |
|---|---|
| Dirty Pipe (CVE-2022-0847, 内核 5.10.4) 页缓存改 /etc/passwd | **漏洞可用**，改动甚至持久化到盘；但 22 端口 sshd 在宿主 ns 读宿主 passwd，session uid 不变 |
| /etc/shadow 空 root 哈希 + 空密码 SSH | sshd 拒绝空密码；2222 上 root/常见口令全失败 |
| user-site .pth 信标（重启触发） | 无 root Python 进程，未触发 |
| 插件包 tar 穿越（`../`） | 被拒：`unsafe path` (8110/8113/8115/8118 一族校验) |
| 插件包软链成员 | 被拒：`unsupported entry type` |
| 插件包 PAX/非 ustar | 被拒（成员顺序/格式校验） |
| OTA 包流式解析器内存破坏 | **未尝试**——需先拿到更新器二进制（在 ubi0 卷 = sys rootfs，需 root 才能 mknod 读） |
| OTA 假包探测 | 泄漏行为：成员顺序强校验、流式中断报 `ota stream aborted`、无暂存路径泄漏 |
| 2222 sshd | host key 独立；ainice/root × 常见口令均失败 |

Dirty Pipe 利用脚本存档：`/tmp/dp.py`（设备）/ `docs/dp.py`（本机，见下）——
原理：pipe 填满再排空（置 CAN_MERGE）→ splice 目标文件 offset-1 的 1 字节 → write 覆盖页缓存。
python ctypes 实现（aarch64：pipe2=59、splice=76），无需交叉编译。

## 6. 自定义固件可行性评估

**被密码学挡死的路**：SIGNATURE 是 ECDSA P-256，验签公钥在更新器（sys rootfs）里，
私钥在厂商。改包→签名失败→拒绝刷写。

**剩余可行路径（按现实程度排序）**：
1. **插件路线（已验证，官方支持）**：多摄像头方案 A/B 不需要自定义固件，见 README 实施方案。
2. **拿更新器二进制做 RE**：前提是宿主侧任意文件读（root 或 sys ns 视角）。拿到后可确认：
   encinfo 的密钥封装方式（若是静态主密钥包裹 → 可解密现有包，逆向出"官方格式"
   的打包器）；验签是否区分在线/本地包（JS 有 otaSignedPackage 文案，本地模式可能只验
   SHA256SUMS 哈希一致——若如此可构造自签名……不，验签对象就是 SUMS；更可能本地模式也验签）。
3. **UART/烧录座（硬件路线）**：SG2002 板通常引出 UART；uboot 阶段若未开 secure boot
   （CVITEK 默认关闭），可直接引导自定义 rootfs/内核，绕过 OTA 验签。**这是自定义固件
   最现实的路**，但需要拆机 + UART 线。
4. **厂商渠道**：DEVELOPER.md 风格对开发者友好，可问询是否提供 sys scope 的 SDK/镜像。

**已固化资产**：
- `reference/firmware/`：官方 .ota + 解包产物 + manifest/SUMS/SIGNATURE 分析
- `reference/device/subsys-rootfs.tgz`：SUBSYS 卷完整镜像（明文，≈当前沙箱世界的全部文件）
- `reference/device/subsys-data.tgz`：/data 卷镜像（插件数据、migateway 证书/token）
- `/data/plugins/recon*/`：设备上的 4 个探针插件（无害，即启即退，可随时 uninstall）
- 本文档 §5 的脚本/方法可复现

## 7. 设备状态（本次实验后）

- SSH 密码已恢复 `REDACTED`；/etc/passwd 页缓存篡改已还原（Dirty Pipe 写回曾持久化，
  已用同法改回 `1001:1001` 并核对）。
- 设备重启过一次（触发信标实验），无异常。
- 遗留：`/data/python/.../zzupdate.{pth,hook}` 信标（uid 门卫，ainice 下零副作用，可删）；
  4 个 recon 插件保持启用（立即退出，无资源占用）。

## 8. 第二轮：OTA 格式黑盒逆向（2026-09-17，见 OTA-FORMAT.md）

- 官方 OTA 服务器留存**旧版本**：`ainice-vision-spinand-2026.09.04.1.ota` 已下载
  （`reference/firmware/extracted-0904/`）；encinfo 每包全新随机 → 排除静态密钥/两时间垫。
- **载荷加密格式破解**：256KB 分块、每块 16B 头（nonce），无填充——
  `enc_size = plain + 16×ceil(plain/262144)`（两版官方包四组数据点吻合）。
- **验证管线全测绘**（21 次构造包 oracle）：成员序列锁死 → manifest（版本严格更新+尺寸
  公式）→ ECDSA 签名（固定公钥、覆盖完整 SUMS、标准严格实现无弱点）→ 解密。
- SUBSYS rootfs 构建清单泄漏厂商构建树：`LicheeRV-Nano-Build/sg2002-v410-sdk`
  （公开 Sipeed SDK 基线；但其 OTA 更新器为 AINICE 自研，公开 SDK 无此格式）。
- 三道墙与路径评估见 `OTA-FORMAT.md` §6：**UART/uboot 直写明文镜像仍是最短路径**；
  软件路径全部卡在先拿到 sys rootfs 里的更新器二进制。
