#!/usr/bin/env python3
"""unix socket 中继（自守护）：拉起目标 go2rtc 实例并录制 mhcamera → go2rtc 的原始请求。
日志: /tmp/relay.log（MH->G2R 段即 mhcamera 发出的请求原文）
"""
import os
import socket
import subprocess
import sys
import threading
import time

L = '/data/plugins/mhcamera/tmp/go2rtc-api.sock'
R = '/tmp/mh-real.sock'

def daemonize():
    if os.fork() > 0:
        sys.exit(0)
    os.setsid()
    if os.fork() > 0:
        sys.exit(0)
    sys.stdout.flush(); sys.stderr.flush()
    fd = os.open('/tmp/relay-run.log', os.O_WRONLY | os.O_CREAT | os.O_APPEND)
    os.dup2(fd, 1); os.dup2(fd, 2)

daemonize()
log = open('/tmp/relay.log', 'ab', 0)

def tag(data, who):
    ts = time.strftime('%H:%M:%S')
    if isinstance(data, str):
        data = data.encode()
    log.write(f'\n==== {who} @{ts} ({len(data)}B) ====\n'.encode())
    log.write(data)
    log.write(b'\n')

def pipe(a, b, who):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            tag(d, who)
            b.sendall(d)
    except Exception as e:
        tag(repr(e), who + '-err')
    try:
        b.shutdown(socket.SHUT_WR)
    except Exception:
        pass

# 1) 目标实例：mh yaml 改 unix_listen 后启动
with open('/data/plugins/mhcamera/go2rtc.yaml') as f:
    yaml = f.read().replace(
        'unix_listen: /data/plugins/mhcamera/tmp/go2rtc-api.sock',
        'unix_listen: ' + R)
with open('/tmp/mh-real.yaml', 'w') as f:
    f.write(yaml)
try:
    os.unlink(R)
except FileNotFoundError:
    pass
p = subprocess.Popen(['/plugins/multicam/bin/go2rtc', '-config', '/tmp/mh-real.yaml'],
                     stdout=open('/tmp/mh-real.log', 'w'), stderr=subprocess.STDOUT,
                     stdin=subprocess.DEVNULL, start_new_session=True)
for _ in range(40):
    time.sleep(0.25)
    if os.path.exists(R):
        break
tag('target pid=%d sock=%s' % (p.pid, os.path.exists(R)), 'SYS')

# 2) 中继监听 mhcamera 的固定 socket 路径
try:
    os.unlink(L)
except FileNotFoundError:
    pass
ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
ls.bind(L)
ls.listen(4)
tag('relay up', 'SYS')
while True:
    c, _ = ls.accept()
    tag('accepted', 'SYS')
    u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        u.connect(R)
    except Exception as e:
        tag(repr(e), 'SYS-connect-fail')
        c.close()
        continue
    threading.Thread(target=pipe, args=(c, u, 'MH->G2R'), daemon=True).start()
    threading.Thread(target=pipe, args=(u, c, 'G2R->MH'), daemon=True).start()
