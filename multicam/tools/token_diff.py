#!/usr/bin/env python3
"""对比实例 yaml 与 mhcamera yaml 的 xiaomi token 是否已分叉（自续期证据）。"""
import hashlib
import re


def tok(path):
    s = open(path).read()
    m = re.search(r"xiaomi:\n\s+(.*)", s)
    return m.group(1).strip() if m else ""


a = tok("/data/plugins/mhcamera/go2rtc.yaml")
b = tok("/data/plugins/multicam/go2rtc-0.yaml")
print("mhcamera token len:", len(a), "md5:", hashlib.md5(a.encode()).hexdigest()[:8])
print("instance token len:", len(b), "md5:", hashlib.md5(b.encode()).hexdigest()[:8])
print("token 已分叉(实例自续期):", a != b)
