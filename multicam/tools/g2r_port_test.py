#!/usr/bin/env python3
"""实验：camera 模块的 RTSP 端口是否跟随 rtsp.listen 配置。"""
import json, os, re, signal, subprocess, time

DATA = "/data/plugins/multicam"
G2R = "/plugins/mhcamera/bin/go2rtc"
PORT = 8555
YAML = f"{DATA}/go2rtc-port-test.yaml"
SOCK = f"{DATA}/tmp/g2r-pt.sock"

for p in filter(str.isdigit, os.listdir("/proc")):
    try:
        c = open(f"/proc/{p}/cmdline", "rb").read().decode(errors="ignore")
        if "go2rtc-port-test.yaml" in c:
            os.kill(int(p), signal.SIGTERM)
    except Exception:
        pass
time.sleep(1)

mh = open("/data/plugins/mhcamera/go2rtc.yaml").read()
tok = re.search(r"^xiaomi:\n((?:  .*\n?)+)", mh, re.M).group(1)
sel = json.load(open("/data/plugins/mhcamera/selection.json"))
url = (f"xiaomi://{sel['account_id']}:cn@192.0.2.154"
       f"?did={sel['id']}&model={sel['model']}&subtype=1&audio=0&transport=tcp")
open(YAML, "w").write(
    f"api:\n  unix_listen: {SOCK}\nrtsp:\n  listen: \":{PORT}\"\nxiaomi:\n  {tok}")
os.chmod(YAML, 0o600)

log = open(f"{DATA}/tmp/g2r-pt.log", "w")
subprocess.Popen([G2R, "-config", YAML], stdout=log, stderr=log,
                 stdin=subprocess.DEVNULL, start_new_session=True)
time.sleep(3)

def api(method, path, body=None):
    cmd = ["curl", "-s", "--max-time", "8", "--unix-socket", SOCK]
    if body is not None:
        cmd += ["-X", method, "-H", "Content-Type: application/json", "-d", json.dumps(body)]
    r = subprocess.run(cmd + [f"http://localhost{path}"], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {"_raw": r.stdout.strip()}

api("POST", "/api/camera/source", {"source": url})
time.sleep(6)
r = api("POST", "/api/camera/rtsp", {"enabled": True, "username": "viewer", "password": "Qc7buvp9"})
print("enable:", json.dumps(r)[:180])
r = api("GET", "/api/camera/rtsp")
print("port:", r.get("port"), "enabled:", r.get("enabled"))
