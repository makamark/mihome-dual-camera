#!/usr/bin/env python3
"""父进程协议测试：本进程 spawn go2rtc（成为其父进程），
用 +86 格式直调 api/xiaomi-phone?action=start（假号，不会发真短信）。
逐个变体打印响应，定位 error:input 的确切条件。"""
import http.client
import json
import os
import socket
import subprocess
import sys
import time

DATA = '/tmp/authtest'
G2R = '/plugins/mhcamera/bin/go2rtc'
MH_YAML = '/tmp/mh-yaml-backup.yaml'
YAML = f'{DATA}/go2rtc-authtest.yaml'
SOCK = f'{DATA}/tmp/g2r-authtest.sock'
LOG = '/tmp/auth-test-g2r.log'

os.makedirs(f'{DATA}/tmp', exist_ok=True)
# 完整复制 mhcamera yaml（保留 allow_paths 等全部段），仅替换 unix_listen
os.makedirs('/tmp/authtest/tmp', exist_ok=True)
yaml_src = open(MH_YAML).read()
import re
yaml2 = re.sub(r'unix_listen: \S+', 'unix_listen: ' + SOCK, yaml_src)
with open(YAML, 'w') as f:
    f.write(yaml2)
os.chmod(YAML, 0o600)
print('yaml lines:', len(yaml2.splitlines()), '| allow_paths:', 'allow_paths' in yaml2)

p = subprocess.Popen([G2R, '-config', YAML],
                     stdout=open(LOG, 'w'), stderr=subprocess.STDOUT,
                     stdin=subprocess.DEVNULL, start_new_session=True)
for _ in range(60):
    time.sleep(0.25)
    if os.path.exists(SOCK):
        break
print(f'parent={os.getpid()} sidecar={p.pid} sock={os.path.exists(SOCK)}', flush=True)

def req(body_bytes, ctype, qs):
    c = http.client.HTTPConnection('localhost')
    c.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.sock.settimeout(30)
    c.sock.connect(SOCK)
    headers = {'Content-Type': ctype} if body_bytes else {}
    c.request('POST', f'/api/xiaomi-phone?{qs}', body_bytes, headers)
    r = c.getresponse()
    data = r.read().decode()
    c.close()
    return data

# 先设 source（餐厅）建立 camera owner 上下文，等 running
import http.client as hc2
def raw(method, path, body=None, ctype='application/json'):
    c = hc2.HTTPConnection('localhost')
    c.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    c.sock.settimeout(40)
    c.sock.connect(SOCK)
    c.request(method, path, body, {'Content-Type': ctype} if body else {})
    r = c.getresponse()
    d = r.read().decode()
    c.close()
    return d

src = 'xiaomi://1000000001:cn@192.0.2.154?did=2000000001&model=chuangmi.camera.81ac1&subtype=1&audio=0&transport=tcp'
print('set source:', raw('POST', '/api/camera/source', json.dumps({'source': src}))[:80], flush=True)
for _ in range(20):
    time.sleep(1)
    st = raw('GET', '/api/camera/source')
    if '"running"' in st:
        print('source running', flush=True)
        break
print('state:', st[:120], flush=True)

variants = [
    ('literal+86', 'calling_code=+86&national_number=11111111111',
     'application/x-www-form-urlencoded'),
    ('json+86', json.dumps({'calling_code': '+86', 'national_number': '11111111111'}).encode(),
     'application/json'),
    ('literal86', 'calling_code=86&national_number=11111111111',
     'application/x-www-form-urlencoded'),
]
for name, body, ctype in variants:
    try:
        out = req(body, ctype, 'action=start')
    except Exception as e:
        out = f'EXC {e!r}'
    print(f'{name}: {out[:200]}', flush=True)

p.kill()
