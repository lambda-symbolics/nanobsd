# ppmpix.py FILE X Y ...: print WIDTHxHEIGHT and (X,Y)=RRGGBB of a binary PPM
import sys

data = open(sys.argv[1], 'rb').read()
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    if data[pos:pos + 1] == b'#':
        while data[pos:pos + 1] != b'\n':
            pos += 1
        continue
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
pos += 1
width, height = int(fields[1]), int(fields[2])
out = '%dx%d' % (width, height)
args = sys.argv[2:]
for i in range(0, len(args), 2):
    x, y = int(args[i]), int(args[i + 1])
    o = pos + 3 * (y * width + x)
    out += ' (%d,%d)=%02x%02x%02x' % (x, y, data[o], data[o + 1], data[o + 2])
print(out)
