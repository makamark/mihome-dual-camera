#!/usr/bin/env python3
"""反汇编 mhcamera.app，解析 adrp+add 字符串引用，定位 xiaomi-phone 请求构造现场。"""
import re, struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else '/work/reference/mhcamera/mhcamera.app'
dis_path = sys.argv[2] if len(sys.argv) > 2 else '/tmp/mh.dis'
data = open(path, 'rb').read()

e_shoff = struct.unpack_from('<Q', data, 0x28)[0]
e_shentsize = struct.unpack_from('<H', data, 0x3a)[0]
e_shnum = struct.unpack_from('<H', data, 0x3c)[0]
e_shstrndx = struct.unpack_from('<H', data, 0x3e)[0]

def sh(i):
    o = e_shoff + i * e_shentsize
    return struct.unpack_from('<IIQQQQ', data, o)

stro = sh(e_shstrndx)[4]
secs = []
for i in range(e_shnum):
    name, typ, flags, addr, off, size = sh(i)
    nm = data[stro + name:data.find(b'\0', stro + name)].decode()
    secs.append((nm, addr, off, size))

def va2str(va):
    for nm, addr, off, size in secs:
        if nm == '.rodata' and addr <= va < addr + size:
            fo = off + (va - addr)
            end = data.find(b'\0', fo)
            s = data[fo:min(end, fo + 150)]
            if len(s) >= 3 and all(32 <= c < 127 for c in s):
                return s.decode()
    return None

dis = open(dis_path).read().splitlines()
ins_re = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{8})\s*(.*)$')
adrp_re = re.compile(r'adrp\s+(x\d+),\s*(?:0x)?([0-9a-f]+)')
add_re = re.compile(r'add\s+(x\d+),\s*x\d+,\s*#(?:0x)?([0-9a-f]+|\d+)')

# 线性扫描：adrp 记 (reg->page)，随后 8 行内 add 同寄存器则解析字符串
hits = []  # (addr, va, string)
for i, line in enumerate(dis):
    m = adrp_re.search(line)
    if not m:
        continue
    reg, page = m.group(1), int(m.group(2), 16)
    for j in range(i + 1, min(i + 10, len(dis))):
        m2 = add_re.search(dis[j])
        if m2 and m2.group(1) == reg:
            imm = int(m2.group(2), 16)
            va = page + imm
            s = va2str(va)
            if s:
                a = ins_re.match(dis[j])
                hits.append((a.group(1) if a else '?', va, s))
            break

key = sys.argv[3] if len(sys.argv) > 3 else 'xiaomi'
print(f'== 含 "{key}" 的 rodata 引用 ==')
for addr, va, s in hits:
    if key.lower() in s.lower():
        print(f'  0x{addr}: 0x{va:x} {s[:110]}')

if len(sys.argv) > 4:
    lo, hi = int(sys.argv[4], 16), int(sys.argv[5], 16)
    print(f'== 函数区间 0x{lo:x}-0x{hi:x} 内全部字符串引用 ==')
    for addr, va, s in hits:
        a = int(addr, 16)
        if lo <= a <= hi:
            print(f'  0x{addr}: 0x{va:x} {s[:110]}')
