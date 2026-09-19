import { getBridgeClient } from "/platform/sdk/bridge.js";

const bridge = getBridgeClient();
const $ = (id) => document.getElementById(id);

const TILE_W = 848, TILE_H = 480;

const state = {
  cfg: null,          // config.get 的完整文档（可编辑副本）
  devices: [],        // [{id,name,model,localip}]
  status: null,
  mapping: null,      // {prefix, events:[{key,present,absent}]}
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

/* ---------- 渲染：状态 ---------- */
function renderStatus() {
  const s = state.status;
  if (!s || !s.valid) { $("state-badge").textContent = "未就绪"; $("state-badge").className = "state warn"; return; }
  const bad = (s.tiles || []).some(t => t.enabled && t.gray);
  const badge = $("state-badge");
  badge.textContent = s.pushing ? (bad ? "运行中（部分画面置灰）" : "运行中") : "连接管线中…";
  badge.className = "state " + (s.pushing ? (bad ? "warn" : "ok") : "warn");

  const fmt = (n) => n == null ? "—" : n;
  $("status-grid").innerHTML = `
    <div class="stat"><div class="k">画布</div><div class="v">${fmt(s.canvas_w)}×${fmt(s.canvas_h)}</div></div>
    <div class="stat"><div class="k">帧率</div><div class="v">${fmt(s.fps)} fps</div></div>
    <div class="stat"><div class="k">已推帧</div><div class="v">${fmt(s.frames)}</div></div>
    ${s.rss_kb ? `<div class="stat"><div class="k">插件内存</div><div class="v">${Math.round(s.rss_kb / 1024)} MB</div></div>` : ""}
    <div class="stat"><div class="k">运行时长</div><div class="v">${Math.floor(fmt(s.uptime_s) / 60)} 分钟</div></div>
    <div class="tiles-row">${ (s.tiles || []).map(t => `
      <div class="tilechip">
        <span class="dot ${t.enabled ? (t.gray ? "gray" : "live") : ""}"></span>
        <div><b>Tile ${t.slot}</b> · ${t.cam_id || "—"}<br>
        <small title="${t.gray ? (t.note || "置灰") : `帧龄 ${t.age_ms}ms · 解 ${fmt(t.decoded)}${t.keyint_ms ? ` · GOP ${t.keyint_ms}ms` : ""}`}${t.g2r_port ? ` · g2rtc:${t.g2r_port}${t.g2r_alive ? (t.g2r_rss_kb ? `(${Math.round(t.g2r_rss_kb / 1024)}MB)` : "†") : "†"}` : ""}">${t.gray ? (t.note || "置灰") : `帧龄 ${t.age_ms}ms · 解 ${fmt(t.decoded)}${t.keyint_ms ? ` · GOP ${t.keyint_ms}ms` : ""}`}${t.g2r_port ? ` · g2rtc:${t.g2r_port}${t.g2r_alive ? (t.g2r_rss_kb ? `(${Math.round(t.g2r_rss_kb / 1024)}MB)` : "†") : "†"}` : ""}</small></div>
      </div>`).join("") }</div>`;
}

/* ---------- 渲染：摄像头 ---------- */
function renderCameras() {
  const acct = accountFromCfg();
  const wrap = $("cam-list");
  wrap.innerHTML = "";
  (state.cfg.cameras || []).forEach((cam, i) => {
    const row = document.createElement("div");
    row.className = "cam-row";
    const did = camDid(cam);
    const opts = ['<option value="__manual__">手动源…</option>']
      .concat(state.devices.map(d =>
        `<option value="${d.id}" ${String(d.id) === did ? "selected" : ""}>${d.name || d.id}（${d.model || "?"} @ ${devIp(d) || "?"}）</option>`).join(""));
    row.innerHTML = `
      <input type="checkbox" ${cam.enabled !== false ? "checked" : ""} title="启用">
      <span class="tile-label">Tile</span>
      <input type="number" min="0" max="7" value="${cam.tile ?? i}" title="槽位">
      <select class="dev">${opts}</select>
      <input class="src" type="text" placeholder="xiaomi:// 或 rtsp:// 源" value="${cam.source || ""}">`;
    row.querySelector('input[type=checkbox]').onchange = e => { cam.enabled = e.target.checked; markDirty(); };
    row.querySelector('input[type=number]').onchange = e => { cam.tile = Math.max(0, Math.min(7, e.target.value | 0)); markDirty(); };
    row.querySelector(".dev").onchange = e => {
      if (e.target.value === "__manual__") return;
      const dev = state.devices.find(d => String(d.id) === e.target.value);
      if (dev) {
        cam.source = sourceFor(dev, acct);
        cam.did = String(dev.id); cam.name = dev.name; cam.model = dev.model; cam.localip = dev.localip;
        row.querySelector(".src").value = cam.source;
        markDirty();
      }
    };
    row.querySelector(".src").onchange = e => { cam.source = e.target.value.trim(); markDirty(); };
    wrap.appendChild(row);
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
      ? `设备检测区域 revision ${prof.revision}：${zones.map(z => `${z.id} ${z.name || "未命名"}`).join("、")}（在总览界面修改）`
      : "设备尚未配置检测区域（在总览界面的总览画面上绘制）";
  } catch (e) {
    const info = $("zone-info");
    if (info && !deviceZones) info.textContent = "检测区域读取失败（/api/zones）";
  }
}

/* ---------- 画布预览 + 区域叠加 ---------- */
const canvas = $("pv-canvas"), ctx = canvas.getContext("2d");
/* 预览懒加载：MJPEG 拉流会让主应用持续 JPEG 编码（约 5–15% CPU），
   默认关闭；闲置 60s 自动断开。 */
let previewOn = false, pvTimer = 0;
function previewStart() {
  if (previewOn) return;
  previewOn = true;
  $("pv-src").src = "/mjpeg/stream";
  $("pv-toggle").textContent = "关闭预览";
  $("pv-hint").textContent = "预览已开启（闲置自动关闭）";
  pvKeep();
}
function previewStop() {
  if (!previewOn) return;
  previewOn = false;
  $("pv-src").removeAttribute("src");
  $("pv-toggle").textContent = "开启预览";
  $("pv-hint").textContent = "预览已关闭";
  clearTimeout(pvTimer);
}
function pvKeep() {
  if (!previewOn) return;
  clearTimeout(pvTimer);
  pvTimer = setTimeout(() => previewStop(), 60000);
}
$("pv-toggle").onclick = () => (previewOn ? previewStop() : previewStart());
function canvasResize() {
  if (!state.status) return;
  canvas.width = state.status.canvas_w || TILE_W * 2;
  canvas.height = state.status.canvas_h || TILE_H;
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
    ctx.fillText("预览未开启（检测区域仍显示）", 12, 24);
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

/* ---------- 渲染：米家映射 ---------- */
function renderMapping() {
  const m = state.mapping;
  const tb = $("mapping-table").querySelector("tbody");
  if (!m || !m.events) { tb.innerHTML = '<tr><td colspan="3">读取失败（米家网关插件未启用？）</td></tr>'; return; }
  const keys = ["global", ..."ABCDEFGH".split("").map(c => "Zone-" + c)];
  tb.innerHTML = keys.map(k => {
    const e = m.events.find(x => x.key === k) || { key: k, present: "", absent: "" };
    return `<tr><td>${k}</td>
      <td><input data-k="${k}" data-f="present" value="${e.present || ""}" placeholder="有人事件名"></td>
      <td><input data-k="${k}" data-f="absent" value="${e.absent || ""}" placeholder="无人事件名"></td></tr>`;
  }).join("");
  tb.querySelectorAll("input").forEach(inp => inp.onchange = () => {
    const e = state.mapping.events.find(x => x.key === inp.dataset.k);
    if (e) { e[inp.dataset.f] = inp.value.trim(); }
  });
}
/* migateway /gateway/virtual-events 保存契约（virtual_events_save 校验）：
   body 恰好 {prefix, events} 两个键；events 恰好 9 槽（global + Zone-A..H），
   每项恰好 {key, present, absent} 三个键且 present/absent 去空白后非空。
   GET 返回的 {prefix, events, status} 与清空的输入都不能直接回传。 */
const MAPPING_KEYS = ["global", ..."ABCDEFGH".split("").map(c => "Zone-" + c)];
const MAPPING_DEFAULTS = {
  "global": ["全局有人", "全局无人"], "Zone-A": ["A区有人", "A区无人"],
  "Zone-B": ["B区有人", "B区无人"], "Zone-C": ["C区有人", "C区无人"],
  "Zone-D": ["D区有人", "D区无人"], "Zone-E": ["E区有人", "E区无人"],
  "Zone-F": ["F区有人", "F区无人"], "Zone-G": ["G区有人", "G区无人"],
  "Zone-H": ["H区有人", "H区无人"],
};
$("mapping-save").onclick = async () => {
  try {
    const m = state.mapping || { prefix: "", events: [] };
    const events = MAPPING_KEYS.map(k => {
      const e = (m.events || []).find(x => x.key === k) || {};
      return {
        key: k,
        present: (e.present || "").trim() || MAPPING_DEFAULTS[k][0],
        absent: (e.absent || "").trim() || MAPPING_DEFAULTS[k][1],
      };
    });
    const prefix = (m.prefix || "").trim();
    await call("migateway", "route",
      { path: "/gateway/virtual-events", http_method: "POST", body: { prefix, events } }, 8000);
    state.mapping = { prefix, events };
    renderMapping();
    flash("米家映射已保存");
  } catch (e) { flash("米家映射保存失败：" + e.message); }
};

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
    document.title = "多摄像头拼图 · 设置";
    delete state.cfg.areas;      /* 已废弃：检测区域由设备侧 zone profile 管理 */
    delete state.cfg.zones;
    if (!state.cfg.cameras) state.cfg.cameras = [];
    $("fps").value = String((state.cfg.output && state.cfg.output.fps) || 3);
    renderCameras();
    refreshDeviceZones();
    notifyReady();   /* 配置就绪即完成启动握手；状态/映射继续后台轮询 */
    refreshAuth();   /* 授权状态轻量；设备目录不自动加载（157MB 设备防 fork 风暴），点按钮手动拉 */
  } catch (e) {
    flash("读取配置失败：" + e.message);
    setUiState("error", "配置读取失败");
  }
  try {
    const r = await withRetry(() => call("migateway", "route", { path: "/gateway/virtual-events", http_method: "GET" }, 6000), 3);
    state.mapping = r.virtual_events || r;
    renderMapping();
  } catch (e) { state.mapping = null; renderMapping(); }
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
let authRetryTimer = 0;
async function refreshAuth(retry) {
  clearTimeout(authRetryTimer);
  const el = $("auth-body");
  let st = null;
  try { st = (await call("multicam", "auth.status", {}, 8000)).auth; }
  catch (e) {
    /* 清除/应用配置会触发插件 reload（bridge 窗口期不可用）：自动重试直到恢复 */
    el.innerHTML = '<p class="hint">插件重启中，授权服务稍候自动恢复…</p>';
    if (retry !== -1)
      authRetryTimer = setTimeout(() => refreshAuth((retry || 0) + 1), 3000);
    return;
  }
  if (st && st.state_text === "authenticated") {
    el.innerHTML = `
      <div class="auth-ok">
        <span class="mi-badge">MI</span>
        <div><div class="k">当前授权账号</div><b>已授权（token 由本插件自持）</b></div>
        <button class="btn danger" id="auth-clear">清除授权</button>
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
  el.innerHTML = `
    <p class="hint">输入米家手机号，获取短信验证码完成授权（内置授权服务，自动按需启动）。验证成功后所有摄像头自动可用。</p>
    <div class="auth-form">
      <select id="auth-cc"><option>+86</option><option>+852</option><option>+853</option><option>+886</option><option>+65</option></select>
      <input id="auth-phone" type="tel" placeholder="手机号" maxlength="15">
      <button class="btn primary" id="auth-send">发送验证码</button>
    </div>
    <div class="auth-form" id="auth-verify-row" style="display:none">
      <input id="auth-code" type="text" placeholder="6 位验证码" maxlength="8">
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
function markDirty() { state.dirty = true; }
function flash(msg) { $("save-msg").textContent = msg; setTimeout(() => { if ($("save-msg").textContent === msg) $("save-msg").textContent = ""; }, 5000); }

$("fps").onchange = e => { state.cfg.output = state.cfg.output || {}; state.cfg.output.fps = +e.target.value; markDirty(); };

$("save").onclick = async () => {
  const b = $("save");
  b.disabled = true;
  try {
    const r = await call("multicam", "config.set", { config: state.cfg }, 8000);
    if (r.error) throw new Error(r.error);
    flash("已保存，插件重启中…");
    state.dirty = false;
    /* 等 bridge 随进程 exec 短暂中断后恢复 */
    let ok = false;
    for (let i = 0; i < 30 && !ok; i++) {
      await new Promise(res => setTimeout(res, 1500));
      try { await refreshStatus(); ok = !!(state.status && state.status.valid); } catch (e) {}
    }
    flash(ok ? "已应用新配置" : "应用超时，请刷新页面查看状态");
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

loadAll().then(() => { postHeight(); });
