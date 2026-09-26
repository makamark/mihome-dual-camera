import { getBridgeClient } from "/platform/sdk/bridge.js";

const bridge = getBridgeClient();
const $ = (id) => document.getElementById(id);

const TILE_W = 848, TILE_H = 480;

const state = {
  cfg: null,          // config.get 的完整文档（可编辑副本）
  devices: [],        // [{id,name,model,localip}]
  status: null,
  dirty: false,
};

/* ---------- 平台 iframe 协议 ----------
   控制台在 iframe load 时读 <html data-ui-state>：booting→ready/error，
   缺失或其它值直接判失败；postMessage 作为补充通道。 */
function setUiState(st, reason) {
  document.documentElement.setAttribute("data-ui-state", st);
  parent.postMessage({ type: "ainice.ui.lifecycle", version: 1, state: st, ...(reason ? { reason } : {}) }, "*");
}
function notifyReady() {
  setUiState("ready");
}
window.addEventListener("error", (e) => {
  if (document.documentElement.getAttribute("data-ui-state") === "booting")
    setUiState("error", e.message || "脚本错误");
});
function postHeight() {
  parent.postMessage({ type: "ainice.ui.frame-size", version: 1, height: document.documentElement.scrollHeight }, "*");
}
new ResizeObserver(postHeight).observe(document.documentElement);

/* ---------- bridge 封装 ---------- */
async function call(module, method, params, timeoutMs = 8000) {
  const r = await bridge.request({ module, method, params }, { timeoutMs });
  if (r && r.ok !== undefined && !r.ok) throw new Error(r.error || "bridge error");
  return r;
}

/* ---------- 配置工具 ---------- */
function accountFromCfg() {
  for (const c of state.cfg.cameras || []) {
    const m = /xiaomi:\/\/([^:]+):/.exec(c.source || "");
    if (m) return m[1];
  }
  return "";
}
function devIp(dev) {
  const m = /@([0-9.]+)\?/.exec(dev.url || "");
  if (m) return m[1];
  const m2 = /ip: ([0-9.]+)/.exec(dev.info || "");
  return m2 ? m2[1] : "";
}
function sourceFor(dev, acct) {
  if (dev.url && dev.url.startsWith("xiaomi://"))
    return dev.url + "&subtype=1&audio=0&transport=tcp";
  return `xiaomi://${acct}:cn@${devIp(dev)}?did=${dev.id}&model=${dev.model}&subtype=1&audio=0&transport=tcp`;
}
function camDid(c) {
  const m = /did=(\d+)/.exec(c.source || "");
  return m ? m[1] : "";
}
function nSlots() {
  let m = 1;
  for (const c of state.cfg.cameras || []) if ((c.tile || 0) + 1 > m) m = (c.tile || 0) + 1;
  return m;
}

/* ---------- 渲染：状态（只给用户关心的：每路是否正常 + 运行时长） ---------- */
function camLabel(slot) {
  const c = (state.cfg && state.cfg.cameras || []).find(c => (c.tile || 0) === slot);
  if (!c) return "摄像头 " + (slot + 1);
  if (c.name) return c.name;
  const src = String(c.source || "");
  if (src.startsWith("rtsp://")) {
    const m = /@([0-9.]+)[:/?]/.exec(src) || /^rtsp:\/\/([0-9.]+)/.exec(src);
    return m ? "自行接入 " + m[1] : "自行接入";
  }
  const did = camDid(c);
  const dev = (state.devices || []).find(d => String(d.id) === did);
  return dev ? dev.name : (c.id || "摄像头 " + (slot + 1));
}
function renderStatus() {
  const s = state.status;
  if (!s || !s.valid) { $("state-badge").textContent = "未就绪"; $("state-badge").className = "state warn"; return; }
  canvasResize();
  const bad = (s.tiles || []).some(t => t.enabled && t.gray);
  const badge = $("state-badge");
  badge.textContent = s.pushing ? (bad ? "部分画面离线" : "运行中") : "连接中";
  badge.className = "state " + (s.pushing ? (bad ? "warn" : "ok") : "warn");

  const up = s.uptime_s || 0;
  const d = Math.floor(up / 86400), h = Math.floor(up % 86400 / 3600), m = Math.floor(up % 3600 / 60);
  const line = $("uptime-line");
  if (line) line.textContent = up > 60 ? `已连续运行 ${d ? d + " 天 " : ""}${h ? h + " 小时 " : ""}${m} 分钟` : "正在启动…";
  const row = $("tiles-row");
  if (row) row.innerHTML = (s.tiles || []).map(t => {
    const live = t.enabled && !t.gray;
    const tip = live
      ? "画面正常"
      : (t.note || "等待画面");
    return `<div class="tilechip" title="${tip}">
      <span class="dot ${t.enabled ? (t.gray ? "gray" : "live") : ""}"></span>
      <div><b>${camLabel(t.slot)}</b><small>${t.gray ? (t.note || "等待画面") : "画面正常"}</small></div>
    </div>`;
  }).join("");
}

/* ---------- 渲染：摄像头 ---------- */
/* 取景器状态（每 tile 一份）：img=最新原画快照、x=框位置、busy 防重入。
   renderCameras 重跑（选机/刷新目录）时保留 img，画面不闪。 */
const framers = {};
/* 接入方式（每路）：米家 / 自行接入（rtsp:// 直连，无需米家授权）。
   编辑态与输入草稿存模块级，renderCameras 重跑（加载设备目录）时不丢。 */
const camModes = {}, srcDrafts = {};
function modeOf(cam) {
  return camModes[cam.tile]
    || (String(cam.source || "").startsWith("rtsp://") ? "rtsp" : "xiaomi");
}
function drawFramer(tile) {
  const f = framers[tile];
  if (!f || !f.ctx) return;
  const cx = f.ctx, W = 848, H = 480, BW = 400;
  if (f.img) cx.drawImage(f.img, 0, 0, W, H);
  else {
    cx.fillStyle = "#101214"; cx.fillRect(0, 0, W, H);
    cx.fillStyle = "#7a828c"; cx.font = "14px sans-serif";
    cx.fillText("取景画面加载中…", 12, 24);
  }
  const x = f.x;
  cx.fillStyle = "rgba(0, 0, 0, 0.55)";
  cx.fillRect(0, 0, x, H);
  cx.fillRect(x + BW, 0, W - x - BW, H);
  cx.strokeStyle = "#4ca7ff";
  cx.lineWidth = 3;
  cx.strokeRect(x + 1.5, 1.5, BW - 3, H - 3);
  cx.fillStyle = "rgba(76, 167, 255, 0.95)";
  cx.font = "16px sans-serif";
  cx.fillText(`画框 ${x}`, x + 10, 30);
}
async function refreshFramer(tile) {
  const f = framers[tile];
  if (!f || f.busy) return;
  f.busy = true;
  try {
    /* cam 必须是数字：for...in 遍历键是字符串，后端只认数值 tile */
    const r = await call("multicam", "preview.full", { cam: Number(tile) }, 8000);
    if (!r.jpeg) throw new Error(r.error || "no jpeg");
    const img = new Image();
    await new Promise((res, rej) => {
      img.onload = res; img.onerror = rej;
      img.src = "data:image/jpeg;base64," + r.jpeg;
    });
    f.img = img;
    drawFramer(tile);
  } catch (e) { /* 静默：下轮/下次操作重试 */ }
  f.busy = false;
}
setInterval(() => {
  if (document.visibilityState !== "visible") return;
  for (const t in framers) refreshFramer(t);
}, 2500);

function renderCameras() {
  const acct = accountFromCfg();
  const wrap = $("cam-list");
  wrap.innerHTML = "";
  (state.cfg.cameras || []).forEach((cam, i) => {
    const card = document.createElement("div");
    card.className = "cam-card";
    const did = camDid(cam);
    const opts = state.devices.map(d =>
      `<option value="${d.id}" ${String(d.id) === did ? "selected" : ""}>${d.name || d.id}</option>`).join("");
    const cropX = cam.crop_x != null && cam.crop_x >= 0 ? cam.crop_x : 224;
    cam.crop_x = cropX;
    const mode = modeOf(cam);
    const srcVal = srcDrafts[cam.tile] != null ? srcDrafts[cam.tile]
      : (String(cam.source || "").startsWith("rtsp://") ? cam.source : "");

    card.innerHTML = `
      <div class="cam-row">
        <input type="checkbox" class="cam-en" ${cam.enabled !== false ? "checked" : ""} title="启用">
        <span class="tile-label">第 ${(cam.tile ?? i) + 1} 路</span>
        <div class="modeseg" role="group" aria-label="接入方式">
          <button type="button" class="seg" data-mode="xiaomi">米家</button>
          <button type="button" class="seg" data-mode="rtsp">自行接入</button>
        </div>
      </div>
      <div class="src-row">
        <select class="dev">
          <option value="" ${did ? "" : "selected"}>选择米家设备…</option>
          ${opts}
        </select>
        <div class="rtsp-part">
          <input class="src" type="text" placeholder="rtsp://账号:密码@摄像头地址:554/路径" value="${srcVal.replace(/"/g, "&quot;")}" spellcheck="false">
          <button type="button" class="btn micro src-go">连接</button>
        </div>
      </div>
      <div class="framer">
        <canvas class="framer-cv" width="848" height="480"></canvas>
        <div class="framer-bar">
          <span class="hint">左右拖动蓝框选取画面，保存后生效</span>
          <button type="button" class="btn micro framer-rl">刷新画面</button>
        </div>
      </div>
      <div class="crop-control">
        <div class="crop-header">
          <span>取景位置</span>
          <span class="crop-val">${cropX === 0 ? "靠左" : cropX === 448 ? "靠右" : cropX === 224 ? "居中" : "自定义"}</span>
          <div class="crop-presets">
            <button type="button" class="btn micro" data-x="0">靠左</button>
            <button type="button" class="btn micro" data-x="224">居中</button>
            <button type="button" class="btn micro" data-x="448">靠右</button>
          </div>
        </div>
        <input type="range" class="crop-slider" min="0" max="448" step="2" value="${cropX}">
      </div>`;
    card.querySelector(".cam-en").onchange = e => { cam.enabled = e.target.checked; markDirty(); };
    /* 接入方式切换：只切编辑视图，不改源——米家模式选中设备、自行接入点「连接」才真正换源生效 */
    card.querySelectorAll(".modeseg .seg").forEach(b => {
      b.classList.toggle("active", b.dataset.mode === mode);
      b.onclick = () => {
        if (modeOf(cam) === b.dataset.mode) return;
        camModes[cam.tile] = b.dataset.mode;
        card.classList.toggle("mode-rtsp", b.dataset.mode === "rtsp");
        card.querySelectorAll(".modeseg .seg").forEach(x =>
          x.classList.toggle("active", x.dataset.mode === b.dataset.mode));
        if (b.dataset.mode === "rtsp") card.querySelector(".src").focus();
      };
    });
    card.classList.toggle("mode-rtsp", mode === "rtsp");
    /* 换源自动生效：调 source.set（后端落盘+就地重建该路，不重启进程），无需保存 */
    async function applySource(tile, src, label) {
      const name = `第 ${tile + 1} 路`;
      $("save-msg").textContent = `${name}正在切换${label ? "到「" + label + "」" : ""}…`;
      try {
        await call("multicam", "source.set", { tile, source: src }, 10000);
        $("save-msg").textContent = `${name}已切换，画面刷新中…`;
        setTimeout(() => refreshFramer(tile), 4000);
      } catch (e) {
        $("save-msg").textContent = `${name}切换失败：${e.message}`;
      }
    }
    card.querySelector(".dev").onchange = e => {
      const dev = state.devices.find(d => String(d.id) === e.target.value);
      if (!dev) return;
      cam.source = sourceFor(dev, acct);
      cam.did = String(dev.id); cam.name = dev.name; cam.model = dev.model; cam.localip = dev.localip;
      applySource(cam.tile, cam.source, dev.name || dev.id);
    };
    /* 自行接入：填 rtsp:// 地址，回车/失焦或点「连接」即生效 */
    function applyRtsp() {
      const v = card.querySelector(".src").value.trim();
      srcDrafts[cam.tile] = v;
      if (!/^rtsp:\/\/\S+/.test(v)) {
        $("save-msg").textContent = "请填写 rtsp:// 开头的摄像头地址";
        return;
      }
      cam.source = v;
      /* 清掉残留的米家设备元数据，状态芯片改按 rtsp 地址显示 */
      cam.name = ""; cam.did = ""; cam.model = ""; cam.localip = "";
      applySource(cam.tile, v, null);
    }
    card.querySelector(".src").onchange = applyRtsp;
    card.querySelector(".src-go").onclick = applyRtsp;

    const slider = card.querySelector(".crop-slider");
    const valLabel = card.querySelector(".crop-val");
    const framer = framers[cam.tile] = framers[cam.tile] || { img: null, busy: false, x: cropX };
    framer.canvas = card.querySelector(".framer-cv");
    framer.ctx = framer.canvas.getContext("2d");
    framer.x = cropX;
    function updateCrop(x) {
      x = Math.max(0, Math.min(448, parseInt(x, 10) & ~1));
      cam.crop_x = x;
      slider.value = x;
      valLabel.textContent = x === 0 ? "靠左" : x === 448 ? "靠右" : x === 224 ? "居中" : "自定义";
      framer.x = x;
      drawFramer(cam.tile);
      markDirty();
      $("save-msg").textContent = "取景已调整，点「保存并应用」后生效";
    }
    slider.oninput = e => updateCrop(e.target.value);
    card.querySelectorAll(".crop-presets button").forEach(btn => {
      btn.onclick = () => updateCrop(btn.dataset.x);
    });

    /* 取景框拖动：指针位置=框中心；拖动纯前端画框（零延迟），松手后拉新帧 */
    {
      const cv = framer.canvas;
      let dragging = false;
      const dragTo = e => {
        const r = cv.getBoundingClientRect();
        const px = (e.clientX - r.left) * (848 / r.width);
        updateCrop(Math.round(px) - 200);
      };
      cv.onpointerdown = e => { dragging = true; cv.setPointerCapture(e.pointerId); dragTo(e); };
      cv.onpointermove = e => { if (dragging) dragTo(e); };
      cv.onpointerup = () => {
        if (!dragging) return;
        dragging = false;
        setTimeout(() => refreshFramer(cam.tile), 600);
      };
      cv.onpointercancel = () => { dragging = false; };
    }
    card.querySelector(".framer-rl").onclick = () => refreshFramer(cam.tile);
    refreshFramer(cam.tile);
    wrap.appendChild(card);
  });
}

/* ---------- 检测区域（设备侧 zone profile，只读） ----------
   唯一事实源是设备 zone profile（总览界面绘制）；面板经同源 REST 读取，
   0-65535 归一化坐标反算到画布像素叠加显示，约 30s 刷新。 */
let deviceZones = null;   /* {revision, zones:[{id,name,color,poly:[[x,y]px],label:[x,y]px}]} */
async function refreshDeviceZones() {
  try {
    const r = await fetch("/api/zones?input=bitstream", { credentials: "same-origin" });
    if (!r.ok) throw new Error("HTTP " + r.status);
    const d = await r.json();
    const prof = d.profile || d;
    const zones = (prof.focus_zones || []).map(z => {
      const regs = (z.polygon && z.polygon.regions) || [];
      const poly = (regs[0] || []).map(([x, y]) => [x / 65535, y / 65535]);
      const lp = (z.label_points || [{}])[0] || {};
      return { id: z.id, name: z.name || "", color: z.color || "#6FBA73", poly,
               label: [lp.x != null ? lp.x / 65535 : 0.5, lp.y != null ? lp.y / 65535 : 0.5] };
    });
    deviceZones = { revision: prof.revision, zones };
    const info = $("zone-info");
    if (info) info.textContent = zones.length
      ? `检测区域：${zones.map(z => `${z.id} ${z.name || "未命名"}`).join("、")}（在设备总览界面修改）`
      : "暂无检测区域，可在设备总览界面绘制";
  } catch (e) {
    /* 读取失败不打扰：下轮自动重试 */
  }
}

/* ---------- 画布预览 + 区域叠加 ---------- */
const canvas = $("pv-canvas"), ctx = canvas.getContext("2d");
/* 预览常开：面板打开即拉流、不自动关闭（旧版 60s 闲置定时器是一次性的，
   到点必掐断，导致框选画面无法常显）。拉流期间主应用持续 JPEG 编码
   （约 5–15% CPU），关闭面板或点「关闭预览」即停；插件重启/断流 3s 自动重连。 */
let previewOn = false, pvRetryTimer = 0;
function previewStart() {
  if (previewOn) return;
  previewOn = true;
  $("pv-src").src = "/mjpeg/stream";
  $("pv-toggle").textContent = "关闭预览";
}
function previewStop() {
  if (!previewOn) return;
  previewOn = false;
  $("pv-src").removeAttribute("src");
  $("pv-toggle").textContent = "开启预览";
  clearTimeout(pvRetryTimer);
}
$("pv-src").onerror = () => {
  if (!previewOn) return;
  clearTimeout(pvRetryTimer);
  pvRetryTimer = setTimeout(() => { if (previewOn) $("pv-src").src = "/mjpeg/stream"; }, 3000);
};
$("pv-toggle").onclick = () => (previewOn ? previewStop() : previewStart());
function canvasResize() {
  if (!state.status) return;
  const w = state.status.canvas_w || 800, h = state.status.canvas_h || 480;
  /* 同尺寸重复赋值也会清空画布（状态 3s 轮询一次 → 预览每 3s 闪一次的根因） */
  if (canvas.width === w && canvas.height === h) return;
  canvas.width = w;
  canvas.height = h;
}
function drawOverlay() {
  const img = $("pv-src");
  if (img.complete && img.naturalWidth > 0) {
    ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
  } else {
    ctx.fillStyle = "#101214";
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    ctx.fillStyle = "#7a828c";
    ctx.font = "14px sans-serif";
    ctx.fillText(previewOn ? "预览连接中…" : "预览已关闭（检测区域仍显示）", 12, 24);
  }
  /* crop_1x2 模式：常显左右路分界与每路框选状态 */
  if (canvas.width === 800 && canvas.height === 480) {
    const tiles = (state.status && state.status.tiles) || [];
    const lab = (t, fb) => t
      ? `${camLabel(t.slot)} · 画框 ${t.crop_x != null ? t.crop_x : "?"}${t.gray ? " · 离线" : ""}`
      : fb;
    ctx.save();
    ctx.strokeStyle = "rgba(255, 255, 255, 0.25)";
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(400, 0);
    ctx.lineTo(400, 480);
    ctx.stroke();
    ctx.font = "12px sans-serif";
    ctx.fillStyle = "rgba(255, 255, 255, 0.55)";
    ctx.fillText(lab(tiles[0], "左路"), 8, 18);
    ctx.fillText(lab(tiles[1], "右路"), 408, 18);
    ctx.restore();
  }
  if (!deviceZones) return;
  deviceZones.zones.forEach(z => {
    if (z.poly.length < 1) return;
    ctx.strokeStyle = z.color;
    ctx.fillStyle = z.color + "33";
    ctx.lineWidth = 2;
    ctx.beginPath();
    z.poly.forEach(([fx, fy], i) => {
      const px = fx * canvas.width, py = fy * canvas.height;
      i ? ctx.lineTo(px, py) : ctx.moveTo(px, py);
    });
    ctx.closePath();
    if (z.poly.length >= 3) ctx.fill();
    ctx.stroke();
    ctx.fillStyle = z.color;
    z.poly.forEach(([fx, fy]) => ctx.fillRect(fx * canvas.width - 2, fy * canvas.height - 2, 4, 4));
    ctx.font = "13px sans-serif";
    ctx.fillText(`${z.id} ${z.name}`.trim(),
                 z.label[0] * canvas.width + 6, z.label[1] * canvas.height - 6);
  });
}
setInterval(() => { try { drawOverlay(); } catch (e) {} }, 500);
setInterval(() => { refreshDeviceZones(); }, 30000);

/* ---------- 拉取 ---------- */
async function withRetry(fn, times = 8, gap = 2500) {
  for (let i = 0; ; i++) {
    try { return await fn(); } catch (e) { if (i >= times - 1) throw e; await new Promise(r => setTimeout(r, gap)); }
  }
}

async function loadAll() {
  try {
    const r = await withRetry(() => call("multicam", "config.get"));
    state.cfg = r.config;
    document.title = "米家双路摄像头";
    delete state.cfg.areas;      /* 已废弃：检测区域由设备侧 zone profile 管理 */
    delete state.cfg.zones;
    if (!state.cfg.cameras) state.cfg.cameras = [];
    $("fps").value = String((state.cfg.output && state.cfg.output.fps) || 3);
    renderCameras();
    refreshDeviceZones();
    notifyReady();   /* 配置就绪即完成启动握手；状态/映射继续后台轮询 */
    refreshAuth();   /* 授权状态轻量；设备目录打开面板自动加载（后台静默，失败不打扰） */
    refreshDevices(true);
  } catch (e) {
    flash("读取配置失败：" + e.message);
    setUiState("error", "配置读取失败");
  }
  try { await refreshStatus(); } catch (e) {}
}

async function refreshStatus() {
  try {
    const r = await call("multicam", "status.get", {}, 4000);
    state.status = r.status;
    if (!canvas.width) canvasResize();
    renderStatus();
  } catch (e) { /* 重启间隙 */ }
}
setInterval(refreshStatus, 3000);

/* 授权错误文案（1:1 复刻 mhcamera 前端 zh 表） */
const AUTH_ERROR_TEXT = {
  "provider": "小米服务拒绝了请求，请稍后重试",
  "protocol": "服务响应无法识别，请稍后重试",
  "network": "网络连接失败，请稍后重试",
  "internal": "插件内部错误，请稍后重试",
  "conflict": "操作冲突，请稍后重试",
  "rate_limit:rate_limit": "发送过于频繁，请稍候再试",
  "rate_limit:sms_retry_later": "发送过于频繁，请稍候再试",
  "rate_limit:sms_send_limit_tomorrow": "验证码发送过多，请明天再试",
  "input:input": "验证码无效或已过期，请点「重新发送」获取新验证码",
  "input:phone_account_not_found": "该手机号未关联小米账号",
  "protocol:xiaomi_token_response_invalid": "小米授权响应无法识别，请稍后重试",
  "protocol:network": "网络连接失败，请稍后重试",
  "additional_verification_required:additional_verification_required": "小米账号需要额外验证，暂时无法完成授权",
};
function authErrText(m) {
  const t = String(m || "");
  if (AUTH_ERROR_TEXT[t]) return AUTH_ERROR_TEXT[t];
  const cat = t.split(":")[0];
  return AUTH_ERROR_TEXT[cat] ? AUTH_ERROR_TEXT[cat] + "（" + t + "）" : t;
}

/* ---------- 账号授权（原生：multicam 内置 xiaomi-phone 客户端，无需 mhcamera） ---------- */
function authSummary(text, cls) {
  const el = $("auth-summary-state");
  if (el) { el.textContent = text; el.className = "state " + (cls || ""); }
}
let authRetryTimer = 0;
async function refreshAuth(retry) {
  clearTimeout(authRetryTimer);
  const el = $("auth-body");
  let st = null;
  try { st = (await call("multicam", "auth.status", {}, 8000)).auth; }
  catch (e) {
    /* 清除/应用配置会触发插件 reload（bridge 窗口期不可用）：自动重试直到恢复 */
    authSummary("重启中", "warn");
    el.innerHTML = '<p class="hint">插件重启中，授权服务稍候自动恢复…</p>';
    if (retry !== -1)
      authRetryTimer = setTimeout(() => refreshAuth((retry || 0) + 1), 3000);
    return;
  }
  if (st && st.state_text === "authenticated") {
    authSummary("已授权", "ok");
    el.innerHTML = `
      <div class="auth-ok">
        <span class="mi-badge">MI</span>
        <div><b>已授权</b></div>
        <button class="btn danger small" id="auth-clear">清除授权</button>
      </div>`;
    $("auth-clear").onclick = async () => {
      if (!confirm("清除后将删除账号授权，所有摄像头停止，需重新短信验证。确定？")) return;
      try {
        await call("multicam", "auth.clear", {}, 8000);
        flash("授权已清除，插件重启中…");
        setTimeout(() => { refreshAuth(); }, 4000);
      } catch (e) { flash("清除失败：" + e.message); }
    };
    return;
  }
  authSummary("未授权", "warn");
  el.innerHTML = `
    <p class="hint">输入米家手机号，完成短信验证后摄像头自动可用。</p>
    <div class="auth-form">
      <select id="auth-cc"><option>+86</option><option>+852</option><option>+853</option><option>+886</option><option>+65</option></select>
      <input id="auth-phone" type="tel" placeholder="手机号" maxlength="15" aria-label="手机号">
      <button class="btn primary" id="auth-send">发送验证码</button>
    </div>
    <div class="auth-form" id="auth-verify-row" style="display:none">
      <input id="auth-code" type="text" placeholder="6 位验证码" maxlength="8" aria-label="验证码">
      <button class="btn primary" id="auth-verify">验证并授权</button>
      <button class="btn" id="auth-resend">重新发送</button>
      <button class="btn" id="auth-cancel">取消</button>
    </div>
    <p class="hint" id="auth-msg"></p>`;
  const msg = (t) => { $("auth-msg").textContent = t; };
  let started = false;
  $("auth-send").onclick = async () => {
    const cc = $("auth-cc").value, nn = $("auth-phone").value.trim();
    if (!/^[0-9]+$/.test(nn)) return msg("请输入正确手机号");
    $("auth-send").disabled = true; msg("启动授权服务并发送…");
    try {
      const r = await call("multicam", "auth.session",
        { action: "start", calling_code: cc, national_number: nn }, 40000);
      const a = r.auth || {};
      if (a.state_text !== "sms_required")
        throw new Error(a.error || a.state_text || "未知响应");
      msg(`验证码已发送至 ${a.masked_target || cc + nn}，请查收短信`);
      $("auth-verify-row").style.display = "flex";
      started = true;
    } catch (e) { msg("发送失败：" + authErrText(e.message)); }
    $("auth-send").disabled = false;
  };
  $("auth-verify").onclick = async () => {
    const code = $("auth-code").value.trim();
    if (!code) return msg("请输入验证码");
    $("auth-verify").disabled = true; msg("验证中…");
    try {
      const r = await call("multicam", "auth.session", { action: "verify", code }, 40000);
      const a = r.auth || {};
      if (a.state_text !== "authenticated")
        throw new Error(a.error || a.state_text || "验证未通过");
      msg("✓ 授权成功，凭证已迁移并重启采集…");
      setTimeout(() => { refreshAuth(); }, 5000);
    } catch (e) {
      msg("验证失败：" + authErrText(e.message));
      $("auth-verify").disabled = false;
    }
  };
  $("auth-resend").onclick = async () => {
    $("auth-resend").disabled = true; msg("重新发送…");
    try {
      const r = await call("multicam", "auth.session", { action: "resend" }, 40000);
      const a = r.auth || {};
      if (a.state_text === "sms_required") msg("验证码已重新发送，请查收");
      else msg(a.error || "重新发送未生效，请稍后重试或取消后重新开始");
    } catch (e) { msg("重新发送失败：" + authErrText(e.message)); }
    $("auth-resend").disabled = false;
  };
  $("auth-cancel").onclick = async () => {
    try { await call("multicam", "auth.session", { action: "cancel" }, 30000); } catch (e) {}
    refreshAuth();
  };
}

/* ---------- 摄像头目录（go2rtc xiaomi 目录，含源地址） ---------- */
async function refreshDevices(auto) {
  const b = $("cam-refresh");
  b.disabled = true; b.textContent = auto ? "自动加载中…" : "查询中…";
  try {
    const r = await withRetry(() => call("multicam", "cameras.list", {}, 30000), 2, 3000);
    if (r.error) throw new Error(r.error);
    state.devices = r.devices || [];
    renderCameras();
    flash(`米家设备 ${state.devices.length} 台已加载`);
  } catch (e) {
    if (!auto) flash("设备列表失败：" + e.message);
  }
  b.disabled = false; b.textContent = "刷新设备列表";
}
$("cam-refresh").onclick = () => refreshDevices(false);

$("cam-add").onclick = () => {
  const n = (state.cfg.cameras || []).length;
  if (n >= 8) return flash("最多 8 路");
  state.cfg.cameras.push({ id: "cam-" + (n + 1), tile: n, enabled: false, source: "" });
  markDirty(); renderCameras();
};

/* 区域编辑已移除：检测区域在设备总览界面管理，面板只读叠加 */

/* ---------- 保存 ---------- */
function markDirty() {
  state.dirty = true;
  const b = $("save");
  if (b) b.classList.add("attention");
}
function markClean() {
  state.dirty = false;
  const b = $("save");
  if (b) b.classList.remove("attention");
}
function flash(msg) { $("save-msg").textContent = msg; setTimeout(() => { if ($("save-msg").textContent === msg) $("save-msg").textContent = ""; }, 5000); }

$("fps").onchange = e => { state.cfg.output = state.cfg.output || {}; state.cfg.output.fps = +e.target.value; markDirty(); };

$("save").onclick = async () => {
  const b = $("save");
  b.disabled = true;
  try {
    const r = await call("multicam", "config.set", { config: state.cfg }, 8000);
    if (r.error) throw new Error(r.error);
    flash("已保存，正在应用…");
    markClean();
    /* 等 bridge 随进程 exec 短暂中断后恢复 */
    let ok = false;
    for (let i = 0; i < 30 && !ok; i++) {
      await new Promise(res => setTimeout(res, 1500));
      try { await refreshStatus(); ok = !!(state.status && state.status.valid); } catch (e) {}
    }
    flash(ok ? "已应用新配置" : "应用超时，请刷新页面查看状态");
    /* 插件 exec 重启会掐断 /mjpeg/stream，浏览器 <img> 会冻结在最后一帧
       （不会自动重连）——保存应用后强制重开预览，否则画面看起来"无变化" */
    if (ok && previewOn) { previewStop(); previewStart(); }
  } catch (e) { flash("保存失败：" + e.message); }
  b.disabled = false;
};

window.addEventListener("keydown", e => {
  if (e.key === "Escape") canvas.classList.remove("drawing");
});

fetch("/api/plugins").then(r => r.json()).then(d => {
  const p = (d.plugins || []).find(x => x.id === "multicam");
  if (p) $("ver").textContent = "v" + p.version;
}).catch(() => {});

/* 预览常开：面板打开即拉流（不再默认关闭/闲置掐断），状态就绪前按 800×480 画布 */
canvas.width = 800; canvas.height = 480;
previewStart();
loadAll().then(() => { postHeight(); });
