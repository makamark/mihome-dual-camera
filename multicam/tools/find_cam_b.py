#!/usr/bin/env python3
"""正式版：确保 go2rtc 实例运行后，对局域网候选 IP 验证宝宝/客厅监控的 did。"""
import concurrent.futures
import json
import os
import re
import signal
import subprocess
import time

DATA = "/data/plugins/multicam"
G2R = "/plugins/multicam/bin/go2rtc"
YAML = f"{DATA}/go2rtc.yaml"
SOCK = f"{DATA}/tmp/g2r-api.sock"
ACCOUNT = "1000000001"
DIDS = [("2000000003", "chuangmi.camera.021a04", "宝宝监控"),
        ("2000000002", "chuangmi.camera.086ac1", "客厅监控")]
SKIP = {"192.0.2.142", "192.0.2.154", "192.0.2.221", "192.0.2.1"}


def kill_all():
    for p in filter(str.isdigit, os.listdir("/proc")):
        try:
            c = open(f"/proc/{p}/cmdline", "rb").read().decode(errors="ignore")
            if "go2rtc" in c and "-config" in c:
                os.kill(int(p), signal.SIGKILL)
        except Exception:
            pass


def api(method, path, body=None):
    cmd = ["curl", "-s", "--max-time", "6", "--unix-socket", SOCK]
    if body is not None:
        cmd += ["-X", method, "-H", "Content-Type: application/json", "-d", json.dumps(body)]
    r = subprocess.run(cmd + [f"http://localhost{path}"], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {}


def main():
    kill_all()
    time.sleep(1)
    mh = open("/data/plugins/mhcamera/go2rtc.yaml").read()
    tok = re.search(r"^xiaomi:\n((?:  .*\n?)+)", mh, re.M).group(1)
    open(YAML, "w").write(
        f"api:\n  unix_listen: {SOCK}\nrtsp:\n  listen: \":8554\"\nxiaomi:\n  {tok}")
    os.chmod(YAML, 0o600)
    log = open(f"{DATA}/tmp/g2r.log", "w")
    subprocess.Popen([G2R, "-config", YAML], stdout=log, stderr=log,
                     stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(3)

    # ping 扫描（分批 8 个防线程/进程限制）
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
            url = (f"xiaomi://{ACCOUNT}:cn@{ip}"
                   f"?did={did}&model={model}&subtype=1&audio=0&transport=tcp")
            api("POST", "/api/camera/source", {"source": url})
            ok = False
            for _ in range(8):
                time.sleep(1)
                r = api("GET", "/api/camera/source")
                if r.get("state") == "running":
                    print(f"FOUND {name} ({did}) = {ip}", flush=True)
                    ok = True
                    break
                if r.get("error"):
                    break
            if ok:
                print("DONE — 该实例正在为第二台摄像头服务(8554)", flush=True)
                return
            print(f"  {ip}/{did} -> {r.get('state')}/{r.get('error')}", flush=True)
    print("NOT_FOUND — 两台摄像头均不在本局域网", flush=True)


if __name__ == "__main__":
    main()
