import struct
import sys

SRC, DST = sys.argv[1], sys.argv[2]
d = bytearray(open(SRC, 'rb').read())

def w32(off, val): d[off:off+4] = struct.pack('<I', val)
def r32(off): return struct.unpack('<I', d[off:off+4])[0]

def enc_b(src, dst):
    return 0x14000000 | (((dst - src) // 4) & 0x3FFFFFF)

def enc_bl(src, dst):
    return 0x94000000 | (((dst - src) // 4) & 0x3FFFFFF)

CAVE = 0x288d04
ORR_W2 = 0x32080042
ORR_W3 = 0x32080043

assert r32(0x178694) == 0xa9bf7bfd
assert r32(0x178b60) == 0x97fffeb4
assert r32(0x178b9c) == 0x2a0303f7

replay = [r32(0x178694), r32(0x178698), r32(0x17869c)]
c1 = CAVE
code = [ORR_W2] + replay + [enc_b(c1 + 16, 0x1786a0)]
for i, wd in enumerate(code):
    w32(c1 + 4*i, wd)
w32(0x178694, enc_b(0x178694, c1))

c2 = CAVE + 4 * 5
code2 = [ORR_W3, r32(0x178b9c), enc_b(c2 + 8, 0x178ba0)]
for i, wd in enumerate(code2):
    w32(c2 + 4*i, wd)
w32(0x178b9c, enc_b(0x178b9c, c2))

open(DST, 'wb').write(d)
print('cave used: 0x%x..0x%x' % (c1, c2 + 12))
print('written', DST, len(d))