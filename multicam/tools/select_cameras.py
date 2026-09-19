#!/usr/bin/env python3
"""选择米家摄像头接入 multicam 双路画布（参考 mhcamera selection 机制）。

用法（设备上，/data/plugins/multicam/ 下）:
  python3 select_cameras.py --list              # 列出账号内设备（did/name/model/localip）
  python3 select_cameras.py --set <did0>,<did1> # 选择两路（按 did）
  python3 select_cameras.py --set <did0>        # 单路（第二路摄像头待接入时）

流程: 起一个仅含 xiaomi 模块的 go2rtc 发现实例（不影响推流实例端口）→
查米家设备目录（含分页/ localip）→ 写入 multicam.json → 重启 multicam。
"""
import argparse
import http.client
import json
import os
import re
import socket
import subprocess
import sys
import time

DATA = "/data/plugins/multicam"
CFG = f"{DATA}/multicam.json"
G2R = "/plugins/multicam/bin/go2rtc"
DISC_YAML = f"{DATA}/go2rtc-disc.yaml"
DISC_SOCK = f"{DATA}/tmp/g2r-disc.sock"
MH_YAML = "/data/plugins/mhcamera/go2rtc.yaml"
BASE = "http://192.0.2.142"

CATALOG_CANDIDATES = ["/api/xiaomi?id={acct}&region=cn"]  # 已实测确认


def unix_http(sock, method, path, body=None, timeout=6):
    """对 unix socket 发 HTTP 请求（go2rtc api）。"""
    c = http.client.HTTPConnection("localhost")
    c.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.sock.settimeout(timeout)
    c.sock.connect(sock)
    headers = {"Content-Type": "application/json"}
    c.request(method, path, body=json.dumps(body) if body is not None else None,
              headers=headers)
    r = c.getresponse()
    data = r.read().decode(errors="replace")
    c.close()
    try:
        return json.loads(data)
    except Exception:
        return {"_raw": data[:200]}


def rest(method, path, body=None):
    login = subprocess.run(
        ["curl", "-s", "-c", "/tmp/sc-cookies.txt", "-X", "POST",
         f"{BASE}/api/auth/login", "-H", "Content-Type: application/json",
         "-d", json.dumps({"username": "admin", "password": "1234"})],
        capture_output=True, text=True)
    csrf = json.loads(login.stdout)["csrf_token"]
    cmd = ["curl", "-s", "--max-time", "10", "-b", "/tmp/sc-cookies.txt",
           "-H", f"X-CSRF-Token: {csrf}"]
    if method == "POST":
        cmd += ["-H", "Content-Type: application/json", "-X", "POST"]
        if body is not None:
            cmd += ["-d", json.dumps(body)]
    r = subprocess.run(cmd + [BASE + path], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {"_raw": r.stdout.strip()[:200]}


def kill_disc():
    for p in filter(str.isdigit, os.listdir("/proc")):
        try:
            c = open(f"/proc/{p}/cmdline", "rb").read().decode(errors="ignore")
            if "go2rtc-disc.yaml" in c or "go2rtc_8556" in c or "go2rtc-scan" in c:
                os.kill(int(p), 9)
        except Exception:
            pass


def ensure_disc_instance():
    mh = open(MH_YAML).read()
    m = re.search(r"^xiaomi:\n((?:  .*\n?)+)", mh, re.M)
    if not m:
        sys.exit("mhcamera yaml 无 xiaomi token 段")
    os.makedirs(f"{DATA}/tmp", exist_ok=True)
    open(DISC_YAML, "w").write(
        f"api:\n  listen: \"\"\n  unix_listen: {DISC_SOCK}\nxiaomi:\n  {m.group(1)}")
    os.chmod(DISC_YAML, 0o600)

    if os.path.exists(DISC_SOCK):
        try:
            unix_http(DISC_SOCK, "GET", "/api/xiaomi")
            return  # 已在运行
        except Exception:
            pass
    if os.path.exists(DISC_SOCK):
        os.unlink(DISC_SOCK)
    log = open(f"{DATA}/tmp/g2r-disc.log", "w")
    subprocess.Popen([G2R, "-config", DISC_YAML], stdout=log, stderr=log,
                     stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(25):
        time.sleep(0.4)
        if os.path.exists(DISC_SOCK):
            try:
                unix_http(DISC_SOCK, "GET", "/api/xiaomi")
                return
            except Exception:
                pass
    sys.exit("发现实例启动超时（看 /data/plugins/multicam/tmp/g2r-disc.log）")


def walk_devices(node, out):
    """通用采集：收集含 id + (model|name) 的对象。"""
    if isinstance(node, dict):
        if "id" in node and ("model" in node or "name" in node):
            did = str(node["id"])
            if did and not any(d["id"] == did for d in out):
                out.append({"id": did,
                            "name": str(node.get("name", "")),
                            "model": str(node.get("model", "")),
                            "localip": str(node.get("localip", ""))})
        for v in node.values():
            walk_devices(v, out)
    elif isinstance(node, list):
        for v in node:
            walk_devices(v, out)


def walk_cursor(node):
    """提取分页游标（has_more + next_start_did）。"""
    if isinstance(node, dict):
        if node.get("has_more") and node.get("next_start_did") is not None:
            return str(node["next_start_did"])
        for v in node.values():
            r = walk_cursor(v)
            if r:
                return r
    elif isinstance(node, list):
        for v in node:
            r = walk_cursor(v)
            if r:
                return r
    return None


def fetch_devices():
    """返回 (devices, account_id)。devices 含目录现成 source url。"""
    ensure_disc_instance()
    acct_resp = unix_http(DISC_SOCK, "GET", "/api/xiaomi")
    acct = ""
    if isinstance(acct_resp, list) and acct_resp:
        acct = str(acct_resp[0])
    elif isinstance(acct_resp, dict):
        acct = str(acct_resp.get("id", ""))

    resp = unix_http(DISC_SOCK, "GET", f"/api/xiaomi?id={acct}&region=cn")
    devices = []
    for s in resp.get("sources", []):
        info = s.get("info", "")
        m = re.search(r"ip: ([0-9.]+)", info)
        devices.append({"id": str(s.get("id", "")),
                        "name": str(s.get("name", "")),
                        "model": str(s.get("model", "")),
                        "localip": m.group(1) if m else "",
                        "url": str(s.get("url", ""))})
    return devices, acct


def update_config(sel):
    cfg = json.load(open(CFG))
    while len(cfg["cameras"]) < 2:
        cfg["cameras"].append({})
    for i, did in enumerate(sel):
        dev = next(d for d in DEVICES if d["id"] == did)
        if not dev["localip"]:
            sys.exit(f"{dev['name']}({did}) 无 localip —— 不在本网段或离线")
        url = dev["url"]
        if "subtype=" not in url:
            sep = "&" if "?" in url else "?"
            url = f"{url}{sep}subtype=1&audio=0&transport=tcp"
        cfg["cameras"][i] = {
            "id": f"cam-{'ab'[i]}",
            "tile": i,
            "enabled": True,
            "source": url,
            "did": did, "name": dev["name"], "model": dev["model"],
            "localip": dev["localip"],
        }
    json.dump(cfg, open(CFG, "w"), ensure_ascii=False, indent=1)
    for i, did in enumerate(sel):
        dev = next(d for d in DEVICES if d["id"] == did)
        print(f"cameras[{i}] ← {dev['name']} ({dev['model']} @ {dev['localip']})")


def restart_multicam():
    st = rest("GET", "/api/plugins")
    items = st if isinstance(st, list) else st.get("plugins", [])
    enabled = any(isinstance(p, dict) and p.get("id") == "multicam"
                  and p.get("enabled") for p in items)
    if enabled:
        rest("POST", "/api/plugins/disable", {"id": "multicam"})
        time.sleep(5)
    rest("POST", "/api/plugins/enable", {"id": "multicam"})
    print("multicam 已重启（监督模式）")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true", help="列出账号设备")
    ap.add_argument("--set", dest="set_dids", help="选择两路（逗号分隔 did）")
    args = ap.parse_args()

    global DEVICES, ACCT
    DEVICES, ACCT = fetch_devices()
    if not DEVICES:
        sys.exit("设备目录为空")
    ACCT = ""
    print(f"账号设备 {len(DEVICES)} 台:")
    for d in DEVICES:
        ip = d["localip"] or "(不在本网段)"
        print(f"  {d['id']}  {d['name']}  [{d['model']}]  {ip}")

    if args.list:
        kill_disc()
        sys.exit(0)
    if not args.set_dids:
        print("用 --set <did0>,<did1> 选择两路")
        kill_disc()
        sys.exit(0)

    sel = [x.strip() for x in args.set_dids.split(",") if x.strip()]
    if len(sel) > 2:
        kill_disc()
        sys.exit("最多选择两路")
    for did in sel:
        if not any(d["id"] == did for d in DEVICES):
            kill_disc()
            sys.exit(f"did {did} 不在设备列表中")
    update_config(sel)
    kill_disc()
    restart_multicam()
    time.sleep(8)
    rt = rest("GET", "/api/media/runtime")
    print("管线:", json.dumps(rt.get("preview"), ensure_ascii=False),
          "state:", rt.get("state"))
