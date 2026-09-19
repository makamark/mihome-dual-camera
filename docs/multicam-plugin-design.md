# multicam 插件设计

状态：v0.1 草案（2026-09-16）。上游决策见 [consult-firmware-vs-plugin.md](consult-firmware-vs-plugin.md)。

## 0. 已定决策

| 决策 | 结论 | 依据 |
|---|---|---|
| 路线 | 插件，不改固件 | Codex 评审；固件由另一任务并行解码中 |
| 解码 | **v1 软解（FFmpeg），硬解为后续升级项** | 设备 /rootfs 无任何 VDEC 用户态库，ainice SDK 头文件无解码 API → 插件进程摸不到硬件解码器；原厂 mhcamera 自己就是 FFmpeg 软解且预算已验证。待固件解码任务摸清媒体栈后再评估 VDEC 直连 |
| 取流 | 复用设备自带定制 go2rtc（第二实例） | BUILD-PROVENANCE 10 补丁覆盖小米认证全链路；静态链接；Unix socket 通信无端口冲突 |
| 转发 | 不动 migateway | presence 槽位协议不变 |
| 画布 | v1 双路 1×2 横排，tile 用原生 848×480 **不缩放** | 免掉 swscale 依赖和缩放 CPU；1696×480 YUV420P = 1.2MB/帧 ≪ 8MB 限制 |

## 1. 架构

```
小米云
  │ ①go2rtc 子进程（定制版，独立 go2rtc.yaml + 独立 unix socket）
  │    两路 xiaomi 源 → 本地 RTSP/TCP（或直读 media.sock，M1 定）
  ▼
multicam.app（唯一 SDK 视频生产者）
  ②avformat RTSP 客户端 ×2（M0 先用外部工具验证）
  ③avcodec 软解 H264 → 每路"最新帧缓冲"（丢帧不积压）
  ④compose：两路 YUV420P 逐行拷贝拼成 1696×480 画布（无缩放）
  ⑤ainice_video_send_frame 推 YUV420P（3–5fps 起步）
  ⑥按自身配置生成 zone profile → POST /api/zones/save（插件是布局唯一事实源）
  ▼
主管线（不动）：硬解收帧 → NPU 推理 ~83ms/帧 → presence global+Zone-A..H
  ▼
migateway（不动）→ 米家
```

**CPU 预算依据**：mhcamera 现状 = 单路 848×480@15fps 软解 + 推 YUV420P，设备运行正常。
本插件 = 双路 848×480@5fps ≈ 今天已验证负载的 2/3。M1 按 3/5/10fps 分档实测验收，门槛全机 CPU ≤70–75%。

## 2. 区域重叠语义（两台摄像头看到同一物理区域怎么办）

分两层，**画布层只管检测，语义层管归属**：

### v1：物理区域唯一归属（推荐，零额外代码）

- 每个物理区域（如"门口"）指定**唯一归属摄像头**（视野更好的那台）；
- zone 多边形只画在归属摄像头的 tile 上，另一台的同区域**不画**；
- 人站在重叠区 → 只触发归属 zone → 米家自动化语义干净（无人/仅A/仅B/都有 四态无歧义）；
- 可选优化：在非归属 tile 的重叠区配 `suppression_areas`（设备原生支持，REST 下发），压掉 global presence 的重复噪声。

代价：次要视角对同一区域不参与判定。对 presence 场景足够。

### v2（如需"任一摄像头看到即算"）：逻辑区域 OR

- 画布层允许两台各画一个 zone（如 Zone-A=门口@A，Zone-B=门口@B）；
- migateway 小幅扩展：配置"逻辑事件 = OR(slot_i, slot_j)"再转发米家；
- 8 个 zone 预算：双路各 ≤4，够用。

### 配置表达（plugin 配置是唯一事实源）

```json
{
  "layout": { "version": 1, "type": "1x2", "tile": [848, 480] },
  "cameras": [
    { "id": "cam-a", "tile": 0, "primary_for": ["doorway", "hall"] },
    { "id": "cam-b", "tile": 1, "primary_for": ["kitchen"] }
  ],
  "areas": [
    { "name": "doorway", "owner": "cam-a", "polygon_on_owner": [[...]], "mijia_slot": "A" }
  ]
}
```

- `areas[].owner` 即唯一归属；插件据此生成 zone profile（多边形平移到归属 tile 的画布坐标）；
- `layout.version` 写进 zone profile 的 revision：**布局变更必须重新标定 zone**，插件拒绝在布局不匹配时下发（防串区）。

## 3. 关键机制（本次调研确认）

- mhcamera 运行时模式（strings 反推）：数据目录 `/data/plugins/<id>/`，写 `go2rtc.yaml`，spawn `<dir>/bin/go2rtc`，走 `go2rtc-api.sock` + `tmp/go2rtc-media.sock` **Unix socket**（无 TCP 端口冲突）；token 状态由 mhcamera.app 管理（`xiaomi_token_*`/`passport_token_*` 错误串）。
- 插件可随包带 `.so`（mhcamera 自带 libavcodec.so.58 / libavutil.so.56，含 H264 解码器，aarch64 ABI 已验证）。
- FFmpeg 路线 M1 需补 libavformat（RTSP 客户端）：自建一致的 FFmpeg 套件，不与 vendor .so 混链。
- **独占生产者**：以实测为准（EBUSY），启动流程 = REST 确认/停用 mhcamera → open；停止时尝试恢复 mhcamera。

## 4. 断流语义（M3）

冻结最后一帧会假报"有人"，填黑会假报"无人"。方案：tile 断流 ≤3s 冻结，>3s 置灰 + 插件 REST 暴露每路新鲜度；migateway 是否加"有效性门控"（presence 事件携带新鲜度，门控转发）留到 M3 评估。

## 5. 里程碑

| 阶段 | 内容 | 门槛 |
|---|---|---|
| **M0 双路取流**（无代码，设备实验） | 独立跑定制 go2rtc 第二实例：自己的 yaml + socket 路径，双 xiaomi 源；摸清 token 引导（复用 mhcamera 持久 token 或交互引导）；核实实际 codec/分辨率/fps | 两台同时在线 ≥24h，断流自动恢复 |
| M1 性能样机 | FFmpeg 交叉编译（avformat+avcodec NEON）；RTSP 拉流→软解→compose→推 YUV420P；3/5/10fps 分档 | 全机 CPU ≤70–75%，不积压，帧龄 ≤2×帧间隔 |
| M2 区域闭环 | zone profile 程序化下发 + tile 映射；四态验证到米家 | 无人/仅A/仅B/都有 全对 |
| M3 可靠性 | 断流语义、重启恢复、mhcamera 归属协商 | 24–72h 长稳，串区=0 |

## 6. 风险登记

1. **go2rtc 第二实例的 token 引导**（M0 最大未知）：vendor 补丁是否允许独立配置双源、token 是否从 mhcamera 数据目录复用——M0 首验。
2. 拼图后小目标检出率：tile 不缩放已是最优；M2 用真人实测漏检。
3. 1696×480 帧对 NPU 预处理（letterbox/缩放）的额外开销：M1 的 /api/media/metrics 观察。
4. 画布上限提醒：4 路原生 tile 拼图会超 8MB packet 限制（3392×1920=9.8MB），4 路以上必须缩放或分屏，v1 不涉及。

---

# M0+M1 执行结果（2026-09-17，v0.2）

## 已达成

- **M0 取流机制全部验证**：第二 go2rtc 实例（自备 yaml+socket）拉真实小米流 running；
  账号 4 台设备枚举成功（餐厅/宝宝/客厅监控 + 门铃）；子码流实测 HEVC Main 848×480@20fps + Opus。
- **端口硬编码已破解**：camera 模块监听目标是 rodata 常量字符串 `video:8554`（0x455a2f）。
  插件启动时逐实例补丁成 `video:855{4+i}`（tmp 副本，原文件不动），实测 8554+8555 双实例并存，
  各自认证 RTSP 服务 HEVC 流。上限 6 路（8554..8559）。
- **M1 双路管线全通**：餐厅摄像头（go2rtc+HEVC 软解）+ 第二路 RTSP 源（直连软解）→
  1696×480@5fps 推送零失败，预览帧双 tile 视觉确认，推理 5fps（NPU 76.6ms）。
- **M1 指标**：5fps 进程树 CPU 36.1%（multicam 33.7% = 双 HEVC 软解主体），RSS 37.9MB；
  3fps CPU 33.3%，帧龄 59-72ms（≤2×源帧间隔 100ms ✓）；10fps 90s 无积压。
  温度：沙箱 /sys/class/thermal 不可读（限制项）。
- **会话韧性**：send_frame 失败自动重建会话续推（mhcamera 过渡/管线复位场景），
  断流 tile 3s 后置灰。

## 未竟与阻塞

1. **真双摄取流测试未做**：宝宝监控(did=2000000003)与客厅监控(did=2000000002)
   经 52 个局域网候选 IP 逐一验证均 source_dial_failed —— 两台都不在本局域网（远程/离线）。
   **需要 Mark 把任一台摄像头接入本网络**。软件侧已就绪：填入 source 即可，
   插件自动打端口补丁起第二实例（8555）。
2. 24-72h 长稳（M3）：可将 multicam enable、mhcamera disable 后持续积累。
3. 断流"有人"语义门控（M3）。

## 给后续会话的操作要点

- 帧格式 YUV420P 整帧；mhcamera 停用→实验→启用（独占锁）；SSh 工具 `tools/*.exp`。
- 设备配置 `/data/plugins/multicam/multicam.json`（当前 fps=3 双 xiaomi 同源测试配置，
  恢复标准配置见 multicam/etc/multicam.json）。

## 长稳与补测状态（2026-09-17 03:15 起算）

- **24-72h 长稳已启动**：multicam enabled（ainice-video 监督、自动重启），
  配置 = 餐厅摄像头双实例（8554+8555 补丁端口）@5fps，mhcamera disabled。
  恢复方法：multicam disable → mhcamera enable。
  观测：`/api/media/runtime`（1696×480）、`/api/media/metrics`（~4.95fps）、Web 系统页 CPU/内存。
- **M1 补测完成**：3fps CPU 33.3% / 5fps 36.1%（双实例+双软解），RSS 37.9-38.6MB，
  帧龄 59-72ms，10fps 90s 无积压。
- **温度定性为平台不可读**：SSH 沙箱与 Web 终端（terminal.html）用户态均无
  /sys/class/thermal、hwmon、/proc/loadavg、/proc/uptime（/sys/class 仅 memp+net）。
  需厂商在特权主应用暴露接口。全机 CPU/内存可从 Web 系统页读取。
- **真双摄扫描**：宝宝/客厅监控三轮全量扫描（52-53 候选 IP × did 验证）均不在本局域网。
  摄像头接入后：跑 multicam/tools/find_cam_b.py（改 did/model）定 IP → 填 cameras[1]。
- **M2 提前完成**：zone profile 已程序化下发（revision 5）：Zone-A=餐厅(cam-a) 左 tile、
  Zone-B=第二路(cam-b) 右 tile；save 用当前 revision（乐观锁），成功后服务端 +1。
  旧单摄 profile 备份在 experiments/zones-backup-*.json。

## 内存约束发现（2026-09-17 深夜，长稳运行期间）

- 设备总内存 **157.4MB**（Web 系统页实测），mhcamera 常规运行时已用 65-70%；
  multicam 双实例树（app+2×go2rtc）RSS 约 38MB。
- **在 multicam 运行中再跑重扫脚本会撞进程/内存限制**（fork/线程创建报
  Resource temporarily unavailable）——扫描与长稳不可同时跑，需先停用 multicam。
- **双摄部署内存推算**：基线 ~107MB + multicam 树 38MB + 第二路 go2rtc/解码增量
  ~15-25MB ≈ 160-170MB，**将贴近甚至超过 157MB 总量**。真双摄接入前必须做内存硬化：
  降低 go2rtc 子实例缓存、解码直接写入 tile 缓冲（去双重缓冲）、必要时降源 fps。
- 温度指标定性为平台不可读（用户态无 thermal/hwmon/loadavg），需厂商在主应用暴露。

## 长稳补充观测（2026-09-17 06:40）

- 当前长稳形态为"同一路摄像头 × 两个并发实例"（替代验证用）：推理速率在 1.6-5.0fps 间波动，
  实例日志零错误——判定为**摄像头对同 did 双会话的授权轮换**（CS2 并发会话额度限制），
  属同源替代测试的固有现象。
- **推论（对真双摄是利好）**：两台不同 did 的摄像头各占独立会话，互不竞争，
  预期每路均可获得稳定满帧率——真双摄接入后无此降速因素。
- 管线全程 running、无积压、无崩溃，监督自动重启与会话韧性持续有效。

## 部署形态决策（2026-09-17 晚）

当前只有餐厅监控在本网络 → multicam 以**单实例稳定形态**常驻（848×480 @4.98fps，cameras[1] 停用）。
同源双实例会触发摄像头对同 did 并发会话的授权轮换（实测速率 1.6-5.0fps 抖动），
真双摄接入后两台不同 did 各占独立会话，无此竞争。
第二台摄像头接入当天：运行 finalize_dual_cam.py 自动定位 IP 并启用 cameras[1] → 升级双路。

## Token 自续期观测（长稳期间复核点）

- 03:15 起长稳；06:23 基线：实例 yaml 的 xiaomi token 与 mhcamera yaml **完全一致**（md5 8430bf4a），尚未轮换。
- mhcamera 已停用 → 无人主动轮换；实例依赖长时效 service token 自持。
- **复核点**：长稳期间若实例源掉线且 token 分叉/失效，恢复方法 = 短暂 enable mhcamera（其 app 登录刷新 token 写回自身 yaml）→ 拷贝新 token 到实例 yaml → 重启实例。
- 厂商补丁语义（0006：single-flight renewal + atomic rotated-token persistence）表明
  go2rtc 模块自身具备续期能力，刷新触发条件待长稳期间持续观察。

## M2 预检：米家虚拟事件槽位映射（2026-09-17）

migateway bridge `/gateway/virtual-events`（GET）返回当前映射（已实测在线、presence_valid=true）：
- global → 全局有人/无人
- Zone-A → **客厅**有人/无人
- Zone-B → **餐厅**有人/无人
- Zone-C..H → 卫生间/D/E/F/G 区

⚠️ 注意：该命名是米家侧既有语义（Mark 原单摄分区），与 multicam 双 tile 的
Zone-A=餐厅(cam-a)/Zone-B=第二路(cam-b) 计划**不一致**。真双摄接入后需在
migateway 里重新对应事件名（改 `/gateway/virtual-events` POST 即可，在线生效），
避免"客厅"事件实际来自餐厅摄像头 tile 的语义错位。

## 摄像头选择功能（v0.3 方向，已离线实现待联调）

参考 mhcamera（selection.json + go2rtc xiaomi 目录 API）实现"自选米家哪两路视频"：

- **设备侧工具** `multicam/tools/select_cameras.py`：
  - `--list`：起发现实例（仅 xiaomi 模块，独立 sock/yaml，不影响推流实例）→
    枚举账号设备（did/name/model/localip，含分页与多端点探测）
  - `--set <did0>,<did1>`：解析 localip → 构造 xiaomi:// 源 → 写入 multicam.json
    cameras[0]/[1] → 自动重启 multicam
- **C 后端基础** `src/catalog.c/h + httputil.c/h`：unix socket HTTP 客户端 +
  米家目录拉取/分页/通用设备采集（为未来 www 设置页准备的桥接后端）
- ⚠️ 目录 API 具体路径需在线探测确认（二进制内多候选已内置：/api/xiaomi/devices、
  /api/xiaomi/{acct}/devices 等，start_did/next_start_did 分页标签来自逆向）
- 语法/编译已离线验证（容器交叉编译通过）；联调待回局域网

# v0.2.2/0.2.3 韧性重做 + 授权链逆向（2026-09-18 凌晨）

## 真双摄接入实验结论

1. **原生 RTSP 不可行**：餐厅(81ac1)/宝宝(021a04)/客厅(086ac1) 在局域网**零监听端口**
   （554/8554/1935/80/443/8899/8888/5543 全关），小米云摄像头只走 CS2 私有协议
   （云密钥交换后直连动态端口）。要 RTSP 直连只能换硬件（Tapo/海康等）。
2. **宝宝(107)/客厅(187) CS2 拨号持续 source_dial_failed，与 token 无关**：
   mhcamera（厂商插件、认证状态正常）拨宝宝同样 `media_session_failed`。
   餐厅(81ac1) 一切正常。怀疑摄像头侧拒绝新会话（远端查看占用/型号会话策略）。
   **待 Mark 排查**：米家 App 是否有人在看宝宝/客厅、或重启这两台摄像头再试。
3. 云端目录（select_cameras.py --list）实时确认三台都在本局域网（154/107/187）。

## mhcamera 授权链全貌（逆向 mhcamera.app + go2rtc 双侧）

```
①引导(一次性)  mhcamera 面板 /auth/sms/start {calling_code, national_number}
              → go2rtc(厂商补丁0003) POST api/xiaomi-phone?action=start
              → account.xiaomi.com globalmiaccount/pass/phoneInfo（查号）
              → fe/service/login/phone?_locale=zh_CN（发短信, 状态 sending_sms）
②验证         面板 /auth/sms/verify {code} → action=verify
              → identity/auth/sendtls|verifytls（2FA ticket, ticket_auth_* 错误族）
              → pass/serviceLoginAuth2 → pass/serviceLogin（换 iov serviceToken）
③持久化       token = yaml xiaomi: 段 <account>: V1:<256B 加密 blob>（自包含，非明文JSON）
              go2rtc 原子写回自己 -config 的 yaml（补丁0006 单飞续期+轮换持久化）
④目录         GET api/xiaomi?id=<acct>&region=cn（分页 next_start_did）
⑤拨号         POST api/camera/source {xiaomi://…} → MISS 云协商 + CS2/TUTK 直连
⑥RTSP输出     POST api/camera/rtsp {"enabled":true,"username":"viewer","password":…}
⑦限流         mhcamera 侧 phone-rate-limit.json（SMS 重试窗口, 墙钟时间戳）
```

- **请求线格式**（mhcamera→go2rtc，已在二进制证实）：`POST /api/xiaomi-phone?action=<start|verify|resend|cancel|clear>` + `application/x-www-form-urlencoded` body（start: calling_code/national_number；verify: code）。
- **api/xiaomi-phone 对裸请求一律 `{"state":"error","error":"input"}`**：action/字段/父进程身份全试过仍拒——go2rtc 串簇含 `phone_sign_json`/`Ticket_nonce&data=`，疑似带签名/票据校验，**协议复刻暂缓**（留档）。
- **multicam 的授权自持（已实现，token 跟随）**：g2r_start 每次重抽 mhcamera yaml 的最新 xiaomi 段（含 watcher 重建）；tile 置灰 >10min 自动重建实例换 token。彻底失效（reauth_required）时的标准操作：enable mhcamera → 面板短信登录 → disable → multicam 自动跟随（≤10min）。

## v0.2.3 存活韧性（当晚两次部署失败换来的结论）

- **平台监督器对快速退出的插件不再重启**（重启上限后躺平）→ multicam 是唯一视频生产者，
  **绝不主动退出**：会话失败无限退避（3s→10s→30s 封顶）。
- **监督器把插件跑在独立 PID 命名空间**（SSH 沙箱 ps 看不到！只有 /api/media/* 和文件系统
  是共享观测面）→ 进程树排查别再用 ps。
- **父进程死后 go2rtc 孤儿化**：占 855x 端口（下代 "RTSP port is unavailable"）+ 旧代凭证
  （RTSP 401）。修复：子进程 `prctl(PR_SET_PDEATHSIG, SIGKILL)` + g2r_start 前
  `/proc` 扫杀 `tmp/go2rtc-p*` 残留（不依赖 killall）。
- 降级启动：任一路拨号失败仅 tile 置灰由 watcher 补救；全部失败按配置 tile 尺寸发灰画布常驻。
- 实测 0.2.3：1696×480 双 tile（餐厅左 tile 活流 + 宝宝灰），5.00fps，NPU 74.7ms/帧。

## zone 自动同步遗留

`zones.get` RPC 存在但对任何参数形状返回 bad_request（REST /api/zones 正常）——
参数姿势待破。当前 zone 同步为 best-effort（失败仅告警）；设备现存 revision 5
恰好与当前布局一致，无实际影响。

# v0.2.1 加固与内存优化（2026-09-17 深夜，代码审查驱动）

审查发现并修复（详见当次会话）：

1. **P1 堆溢出**：捕获槽位现按首帧分辨率**上锁**——运行期流分辨率变化只丢帧计数
   （capture.size_drops），不再重新分配，快照永远按锁定的 tile 尺寸拷贝；
   画布侧 capture_paint_tile 再校验一次（不符返回 -2 → tile 置灰）。
2. **P2 tile 串区**：run 按 `cameras[].tile` 归位，禁用路留灰位不前移；
   画布宽 = 启用路最大 tile+1。
3. **P2 go2rtc 无守护**：新增 watcher 线程——waitpid(WNOHANG) 探测子进程退出，
   首次立即重建、其后 5s→60s 退避；稳定 60s 清零退避；凭证复用（启用态不可改凭证）。
4. **P2 zone 无下发**：新增 `src/zones.c`——启动时 `zones.get` 取 revision（乐观锁）
   → `zones.save` 从 `areas` 生成 profile（多边形按 owner tile 平移 + 0-65535 归一化，
   质心做 label_point）。失败仅告警不断流；`zones.auto_sync:false` 关闭。
   设备配置 areas[1] owner=cam-b 与当前 cameras[1]（cam-a2-dual-test）不匹配时自动跳过，
   真双摄接入（finalize_dual_cam 写入 cam-b）后恢复双 zone。
5. **P3**：yuvj420p 接受（同布局）；会话耗尽 rc=1；http_unix 5s 超时 + 修响应体切分；
   g2r_stop 清理补丁副本；日志不打印 RTSP 明文密码。

内存削减（已实现）：解码 `thread_count=1`（单核 A53，去帧级多线程参考帧池 ~MB 级/路）、
快照按行距直写画布（删 tile 双重缓冲，−0.6MB/路 + 少一次全帧拷贝）、
go2rtc 子进程 `GOMEMLIMIT=16MiB GOGC=40`（上游 v1.9.14，Go≥1.19）、`M_ARENA_MAX=2`。
内存削减（后续候选）：VDEC 硬解（等固件媒体栈结论，替换 FFmpeg 解码上下文 ~3-5MB/路）、
第二路若用支持原生 RTSP 的摄像头可省一个 go2rtc 实例（~12-14MB，vendor 单源/实例限制）、
按需启停发现实例。**部署提示**：真双摄前用 Web 系统页核对全机余量（总 157.4MB，
推算双摄 ~155-165MB，已逼近上限）。

构建变化：`VERSION ?= 0.2.1`（Makefile），`make clean` 现在会删 `multicam-*.plugin`
（0.2.0 本地包已因此丢失，设备 /plugins/multicam/multicam.app 上仍有该版本可回捞）。
发布：release/multicam-0.2.1.plugin（sha256 178d6e62…），release.json 已指向。
