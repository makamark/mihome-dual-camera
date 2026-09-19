#!/usr/bin/env python3
"""在局域网候选 IP 中迭代验证客厅监控(did=2000000002)的实际地址。"""
import concurrent.futures
import json
import subprocess
import time


alive = []
for base in range(2, 255, 8):
    procs = {}
    for i in range(base, min(base + 8, 255)):
        ip = f'192.0.2.{i}'
        procs[ip] = subprocess.Popen(['ping', '-c', '1', '-W', '1', ip],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for ip, p in procs.items():
        if p.wait() == 0:
            alive.append(ip)

skip = {'192.0.2.142', '192.0.2.154', '192.0.2.221', '192.0.2.1'}
cands = [ip for ip in alive if ip not in skip]
print(f'存活 {len(alive)}，候选 {len(cands)}: {cands}', flush=True)


def api(method, path, body=None):
    cmd = ['curl', '-s', '--max-time', '6',
           '--unix-socket', '/data/plugins/multicam/tmp/g2r-pt.sock']
    if body is not None:
        cmd += ['-X', method, '-H', 'Content-Type: application/json',
                '-d', json.dumps(body)]
    r = subprocess.run(cmd + ['http://localhost' + path], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except Exception:
        return {}


found = None
for ip in cands:
    url = (f'xiaomi://1000000001:cn@{ip}?did=2000000002'
           f'&model=chuangmi.camera.086ac1&subtype=1&audio=0&transport=tcp')
    api('POST', '/api/camera/source', {'source': url})
    ok = False
    for _ in range(10):
        time.sleep(1)
        r = api('GET', '/api/camera/source')
        if r.get('state') == 'running':
            print(f'FOUND 客厅监控 = {ip}', flush=True)
            ok = True
            break
        if r.get('error'):
            break
    if ok:
        found = ip
        break
    print(f'  {ip} -> {r.get("state")}/{r.get("error")}', flush=True)

if not found:
    print('NOT_FOUND', flush=True)
