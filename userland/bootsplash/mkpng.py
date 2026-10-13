# Render the LispBSD lambda exactly as bootsplash.c does, full frame, to PNG.
import re, sys, zlib, struct
W, H = int(sys.argv[1]), int(sys.argv[2])
src = open(sys.argv[3]).read()
LW = int(re.search(r'#define\s+LOGO_W\s+(\d+)', src).group(1))
LH = int(re.search(r'#define\s+LOGO_H\s+(\d+)', src).group(1))
body = src[src.index('logo_bits'):]
rows = re.findall(r'\{([01,\s]+)\}', body)
bits = [[int(v) for v in r.replace(' ', '').strip(',').split(',')] for r in rows][:LH]
shades = [(175,255,175),(135,255,175),(95,255,175),(95,255,135),(95,215,135),(95,175,135)]
scale = max(1, H // 3 // LH)
w, h = LW*scale, LH*scale
x0, y0 = (W - w)//2, (H - h)//2
img = bytearray(W*H*3)
for y in range(LH):
    band = y*6//LH
    for x in range(LW):
        if not bits[y][x]: continue
        r,g,b = shades[band]
        for sy in range(scale):
            base = ((y0 + y*scale + sy)*W + x0 + x*scale)*3
            img[base:base+scale*3] = bytes((r,g,b))*scale
raw = b''.join(b'\x00' + bytes(img[y*W*3:(y+1)*W*3]) for y in range(H))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t+d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
open(sys.argv[4], 'wb').write(png)
print(f"{W}x{H} lambda {w}x{h} at {x0},{y0} scale {scale}, {len(png)} bytes")
