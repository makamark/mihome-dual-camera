#!/usr/bin/env python3
"""M0 实验：设备侧启动 multicam 专用 go2rtc 第二实例并启用 RTSP 输出。
用法: python3 g2r_setup.py [--keep-source]
- 从 mhcamera 的 go2rtc.yaml 合并 xiaomi token 段（唯一写入者仍是 mhcamera.app）
- 精确终止旧实例（按 cmdline 匹配 multicam/go2rtc.yaml）
- 写本实例 yaml、启动、POST source、启用 RTSP、输出最终状态
"""
import json, os, re, signal, subprocess, sys, time

DATA = "/data/plugins/multicam"
G2R = "/plugins/mhcamera/bin/go2rtc"
YAML = f"{DATA}/go2rtc.yaml"
SOCK = f"{DATA}/tmp/g2r-api.sock"
LOG = f"{DATA}/tmp/g2r.log"
SEL = "/data/plugins/mhcamera/selection.json"
MH_YAML = "/data/plugins/mhcamera/go2rtc.yaml"
CAM_IP = "192.0.2.154"  # TODO(M0-B): 第二台摄像头时改为对应 IP

def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)

def api(method, path, body=None):
    cmd = ["curl", "-s", "--max-time", "8", "--unix-socket", SOCK]
    if body is not None:
        cmd += ["-X", method, "-H", "Content-Type: application/json", "-d", json.dumps(body)]
    cmd += [f"http://localhost{path}"]
    r = sh(cmd)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {"_raw": r.stdout.strip(), "_err": r.stderr.strip(), "_argv": " ".join(cmd)}

def kill_old():
    me = os.getpid()
    for pid in filter(str.isdigit, os.listdir("/proc")):
        if int(pid) == me:
            continue
        try:
            cmd = open(f"/proc/{pid}/cmdline", "rb").read().decode(errors="ignore")
        except Exception:
            continue
        if "go2rtc" in cmd and "multicam/go2rtc.yaml" in cmd:
            try:
                os.kill(int(pid), signal.SIGTERM)
                print("terminated old instance pid", pid)
            except Exception as e:
                print("kill failed", pid, e)
    time.sleep(1)

def main():
    sel = json.load(open(SEL))
    mh = open(MH_YAML).read()
    m = re.search(r"^xiaomi:\n((?:  .*\n?)+)", mh, re.M)
    if not m:
        sys.exit("mhcamera yaml 没有 xiaomi 段")
    token_lines = [l.strip() for l in m.group(1).strip().splitlines()]
    url = (f"xiaomi://{sel['account_id']}:cn@{CAM_IP}"
           f"?did={sel['id']}&model={sel['model']}&subtype=1&audio=0&transport=tcp")
    yaml = (
        "api:\n"
        f"  unix_listen: {SOCK}\n"
        "rtsp:\n"
        '  listen: ":8554"\n'
        "  username: mcuser\n"
        "  password: mcPass9987\n"
        "xiaomi:\n" + "".join(f"  {l}\n" for l in token_lines)
    )
    open(YAML, "w").write(yaml)
    os.chmod(YAML, 0o600)
    print("yaml written")

    kill_old()
    if os.path.exists(SOCK):
        os.unlink(SOCK)
    log = open(LOG, "w")
    subprocess.Popen([G2R, "-config", YAML], stdout=log, stderr=log,
                     stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(3)

    r = api("POST", "/api/camera/source", {"source": url})
    print("source:", json.dumps(r)[:200])
    time.sleep(6)
    r = api("POST", "/api/camera/rtsp", {"enabled": True})
    print("rtsp enable:", json.dumps(r)[:200])
    r = api("GET", "/api/camera/rtsp")
    print("rtsp status:", json.dumps(r)[:300])
    r = api("GET", "/api/camera/source")
    print("source state:", r.get("state") if isinstance(r, dict) else r)

if __name__ == "__main__":
    main()
