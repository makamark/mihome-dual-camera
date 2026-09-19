#!/usr/bin/env python3
"""设备侧进程治理：清残留扫描进程，保留 multicam 长稳本体。"""
import os, signal

KILL_PAT = ("find_scan6", "find_new", "finalize_dual", "go2rtc_8556",
            "go2rtc-scan", "go2rtc-pt", "find_cam_b")
keep, killed = [], []
for p in filter(str.isdigit, os.listdir("/proc")):
    try:
        c = open(f"/proc/{p}/cmdline", "rb").read().decode(errors="ignore")
    except Exception:
        continue
    if "go2rtc" in c and "go2rtc-p" in c:
        keep.append((p, c[:60]))
    elif any(k in c for k in KILL_PAT):
        try:
            os.kill(int(p), signal.SIGKILL)
            killed.append(f"{p} {c[:60]}")
        except Exception as e:
            killed.append(f"{p} fail {e}")

print("KILLED:", len(killed))
for k in killed:
    print("  ", k)
print("KEEP:", len(keep))
for k in keep:
    print("  ", k)
