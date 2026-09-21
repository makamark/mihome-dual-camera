#!/usr/bin/env python3
"""go2rtc 包装器：顶替 /plugins/mhcamera/bin/go2rtc。
真二进制在 go2rtc.real。把 -config 的 yaml 复制并把 unix_listen 重定向到
/tmp/cap-sidecar.sock 启动真 sidecar；本进程在原 socket 路径做中继并录制全部流量。
"""
import os
import socket
import subprocess
import sys
import threading
import time

REAL = '/plugins/mhcamera/bin/go2rtc.real'
CFG = '/tmp/cap-config.yaml'
SIDE_SOCK = '/tmp/cap-sidecar.sock'
LOG = '/tmp/capture.log'

def log_write(data, who):
    with open(LOG, 'ab', 0) as f:
        ts = time.strftime('%H:%M:%S')
        if isinstance(data, str):
            data = data.encode()
        f.write(f'\n==== {who} @{ts} ({len(data)}B) ====\n'.encode())
        f.write(data)
        f.write(b'\n')

def main():
    args = sys.argv[1:]
    cfg_path = None
    for i, a in enumerate(args):
        if a == '-config' and i + 1 < len(args):
            cfg_path = args[i + 1]
    if not cfg_path:
        os.execv(REAL, [REAL] + args)
        return

    yaml = open(cfg_path).read()
    # 提取原 unix_listen
    orig = None
    for line in yaml.splitlines():
        if 'unix_listen:' in line:
            orig = line.split(':', 1)[1].strip()
            break
    if not orig:
        os.execv(REAL, [REAL] + args)
        return
    log_write(f'wrap cfg={cfg_path} orig_sock={orig}', 'SYS')
    yaml2 = yaml.replace(orig, SIDE_SOCK)
    open(CFG, 'w').write(yaml2)

    try:
        os.unlink(SIDE_SOCK)
    except FileNotFoundError:
        pass
    p = subprocess.Popen([REAL, '-config', CFG],
                         stdout=open('/tmp/cap-sidecar.log', 'w'), stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    for _ in range(40):
        time.sleep(0.25)
        if os.path.exists(SIDE_SOCK):
            break
    log_write(f'sidecar pid={p.pid} ready={os.path.exists(SIDE_SOCK)}', 'SYS')

    try:
        os.unlink(orig)
    except FileNotFoundError:
        pass
    ls = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    ls.bind(orig)
    ls.listen(4)
    log_write('relay up @ ' + orig, 'SYS')

    def pipe(a, b, who):
        try:
            while True:
                d = a.recv(65536)
                if not d:
                    break
                log_write(d, who)
                b.sendall(d)
        except Exception as e:
            log_write(repr(e), who + '-err')

    while True:
        c, _ = ls.accept()
        u = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            u.connect(SIDE_SOCK)
        except Exception as e:
            log_write(repr(e), 'SYS-connect')
            c.close()
            continue
        threading.Thread(target=pipe, args=(c, u, 'MH->G2R'), daemon=True).start()
        threading.Thread(target=pipe, args=(u, c, 'G2R->MH'), daemon=True).start()

if __name__ == '__main__':
    main()
