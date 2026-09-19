#!/usr/bin/env python3
"""传输层 A/B：同一已知有效 body，curl vs C 代码同款 HTTP/1.0 裸请求。"""
import http.client, json, socket, subprocess

SOCK = "/data/plugins/multicam/tmp/g2r-api.sock"
BODY = json.dumps({"enabled": True, "username": "viewer", "password": "SKvAgSHf"})

def via_curl():
    r = subprocess.run(["curl", "-s", "--max-time", "5", "--unix-socket", SOCK,
                        "-X", "POST", "-H", "Content-Type: application/json",
                        "-d", BODY, "http://localhost/api/camera/rtsp"],
                       capture_output=True, text=True)
    return r.stdout.strip()[:250]

def via_raw10():
    """与 multicam src/g2r.c http_unix 完全相同的请求形态"""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    req = ("POST /api/camera/rtsp HTTP/1.0\r\n"
           "Host: localhost\r\n"
           "Content-Type: application/json\r\n"
           f"Content-Length: {len(BODY)}\r\n"
           "Connection: close\r\n\r\n" + BODY)
    s.sendall(req.encode())
    data = b""
    while True:
        chunk = s.recv(4096)
        if not chunk:
            break
        data += chunk
    s.close()
    text = data.decode(errors="replace")
    return text[text.find("\r\n\r\n") + 4:][:250] if "\r\n\r\n" in text else text[:250]

print("curl  :", via_curl())
print("raw10 :", via_raw10())
