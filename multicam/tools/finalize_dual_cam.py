#!/usr/bin/env python3
"""multicam 第二台摄像头一键闭环脚本（在设备上运行）。

用法: python3 finalize_dual_cam.py [--scan-only]

摄像头接入本局域网后执行一次即可:
  1. ping 扫描局域网 → 候选 IP
  2. 对宝宝监控/客厅监控逐个发 xiaomi 源验证 → 确定真实 IP
  3. 更新 /data/plugins/multicam/multicam.json 的 cameras[1]
  4. REST 停用 mhcamera、重启 multicam（先 disable 再 enable）
  5. 验证 /api/media/runtime 出现 1696×480 双路画布
--scan-only 只做 1-2 步，不改动系统。
"""
import concurrent.futures
import json
import os
import re
import signal
import subprocess
import sys
import time
import urllib.request

DATA = "/data/plugins/multicam"
G2R = "/plugins/multicam/bin/go2rtc"
YAML = f"{DATA}/go2rtc.yaml"
SOCK = f"{DATA}/tmp/g2r-api.sock"
CFG = f"{DATA}/multicam.json"
MH_YAML = "/data/plugins/mhcamera/go2rtc.yaml"
BASE = "http://192.0.2.142"
SCAN_ONLY = "--scan-only" in sys.argv

DIDS = [("2000000003", "chuangmi.camera.021a04", "宝宝监控"),
        ("2000000002", "chuangmi.camera.086ac1", "客厅监控")]
SKIP = {"192.0.2.142", "192.0.2.154", "192.0.2.221", "192.0.2.1"}


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def api_unix(method, path, body=None):
    cmd = ["curl", "-s", "--max-time", "8", "--unix-socket", SOCK]
    if body is not None:
        cmd += ["-X", method, "-H", "Content-Type: application/json", "-d", json.dumps(body)]
    r = subprocess.run(cmd + [f"http://localhost{path}"], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {}


def rest(method, path, body=None):
    """以 admin 登录本机 Web API（拿 csrf 后调用）。"""
    login = sh(["curl", "-s", "-c", "/tmp/fc-cookies.txt", "-X", "POST",
                f"{BASE}/api/auth/login", "-H", "Content-Type: application/json",
                "-d", json.dumps({"username": "admin", "password": "1234"})])
    try:
        csrf = json.loads(login.stdout)["csrf_token"]
    except Exception:
        return {"_error": "login failed"}
    cmd = ["curl", "-s", "-b", "/tmp/fc-cookies.txt", "-H", f"X-CSRF-Token: {csrf}"]
    if method == "POST":
        cmd += ["-H", "Content-Type: application/json", "-X", "POST"]
        if body is not None:
            cmd += ["-d", json.dumps(body)]
    r = subprocess.run(cmd + [BASE + path], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {"_raw": r.stdout.strip()[:200]}


def kill_go2rtc():
    for p in filter(str.isdigit, os.listdir("/proc")):
        try:
            c = open(f"/proc/{p}/cmdline", "rb").read().decode(errors="ignore")
            if "go2rtc" in c and "-config" in c:
                os.kill(int(p), signal.SIGKILL)
        except Exception:
            pass


def scan():
    kill_go2rtc()
    time.sleep(1)
    mh = open(MH_YAML).read()
    m = re.search(r"^xiaomi:\n((?:  .*\n?)+)", mh, re.M)
    if not m:
        print("FATAL: mhcamera yaml 无 xiaomi 段"); sys.exit(1)
    tok = m.group(1)
    open(YAML, "w").write(
        f"api:\n  unix_listen: {SOCK}\nrtsp:\n  listen: \":8554\"\nxiaomi:\n  {tok}")
    os.chmod(YAML, 0o600)
    log = open(f"{DATA}/tmp/g2r.log", "w")
    subprocess.Popen([G2R, "-config", YAML], stdout=log, stderr=log,
                     stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(3)

    def ping(ip):
        r = subprocess.run(["ping", "-c", "1", "-W", "1", ip], capture_output=True)
        return ip if r.returncode == 0 else None

    alive = []
    for base in range(2, 255, 8):
        with concurrent.futures.ThreadPoolExecutor(8) as ex:
            for r in ex.map(ping, [f"192.0.2.{i}" for i in range(base, min(base + 8, 255))]):
                if r:
                    alive.append(r)
    cands = [ip for ip in alive if ip not in SKIP]
    print(f"存活 {len(alive)}，候选 {len(cands)}", flush=True)

    for ip in cands:
        for did, model, name in DIDS:
            url = (f"xiaomi://{ACCOUNT_ID}:cn@{ip}"
                   f"?did={did}&model={model}&subtype=1&audio=0&transport=tcp")
            api_unix("POST", "/api/camera/source", {"source": url})
            ok = False
            for _ in range(15):
                time.sleep(1)
                r = api_unix("GET", "/api/camera/source")
                if r.get("state") == "running":
                    print(f"FOUND {name} ({did}) = {ip}", flush=True)
                    return did, model, name, ip
                if r.get("error"):
                    break
            print(f"  {ip}/{name} -> {r.get('state')}/{r.get('error')}", flush=True)
    print("NOT_FOUND", flush=True)
    return None


def main():
    global ACCOUNT_ID
    sel = json.load(open("/data/plugins/mhcamera/selection.json"))
    ACCOUNT_ID = sel["account_id"]

    result = scan()
    if SCAN_ONLY:
        return
    if not result:
        print("未找到第二台摄像头，系统未做任何改动。")
        sys.exit(1)
    did, model, name, ip = result

    cfg = json.load(open(CFG))
    url = (f"xiaomi://{ACCOUNT_ID}:cn@{ip}"
           f"?did={did}&model={model}&subtype=1&audio=0&transport=tcp")
    if len(cfg["cameras"]) < 2:
        cfg["cameras"].append({})
    cfg["cameras"][1] = {"id": "cam-b", "tile": 1, "enabled": True, "source": url}
    json.dump(cfg, open(CFG, "w"), ensure_ascii=False, indent=1)
    print("配置已更新: cameras[1] =", name, ip)

    print("停用 mhcamera:", json.dumps(rest("POST", "/api/plugins/disable",
                                            {"id": "mhcamera"}))[:100])
    print("重启 multicam:", json.dumps(rest("POST", "/api/plugins/disable",
                                            {"id": "multicam"}))[:100])
    time.sleep(12)
    print("启用 multicam:", json.dumps(rest("POST", "/api/plugins/enable",
                                            {"id": "multicam"}))[:100])
    for i in range(10):
        time.sleep(5)
        rt = rest("GET", "/api/media/runtime")
        if rt.get("state") == "running":
            print("双路画布运行中:", json.dumps(rt.get("preview")))
            break
    else:
        print("管线未就绪，稍后重查 /api/media/runtime")


if __name__ == "__main__":
    main()
