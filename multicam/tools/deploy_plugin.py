#!/usr/bin/env python3
"""设备侧插件导入小工具：登录一次（cookie+csrf 同会话），POST 原始字节流导入插件包。
用法: python3 deploy_plugin.py /tmp/multicam-x.y.plugin"""
import json, sys, urllib.request, http.cookiejar

pkg = sys.argv[1]
jar = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(jar))
r = json.load(op.open(urllib.request.Request(
    "http://127.0.0.1/api/auth/login",
    data=json.dumps({"username": "admin", "password": "1234"}).encode(),
    headers={"Content-Type": "application/json"})))
data = open(pkg, "rb").read()
req = urllib.request.Request(
    "http://127.0.0.1/api/plugins/import", data=data,
    headers={"Content-Type": "application/vnd.ainice.plugin",
             "X-CSRF-Token": r["csrf_token"]})
print(json.dumps(json.load(op.open(req)))[:400])
