import struct, sys
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'$VBT', d[:20]
hdr_size, vbt_size = struct.unpack_from('<HH', d, 0x16)
bdb_off, = struct.unpack_from('<I', d, 0x1c)
bdb = d[bdb_off:]
assert bdb[:16] == b'BIOS_DATA_BLOCK ', bdb[:16]
bver, bhdr, bsize = struct.unpack_from('<HHH', bdb, 16)
print('VBT size', vbt_size, 'BDB version', bver)
blocks = {}
p = bhdr
while p + 3 <= bsize:
    bid = bdb[p]; size, = struct.unpack_from('<H', bdb, p + 1)
    if bid == 0: break
    data = p + 3
    if bid == 53 and bdb[data] >= 3:   # MIPI sequence v3+ carries its own u32 size
        size, = struct.unpack_from('<I', bdb, data + 1)
    blocks[bid] = bdb[data:data+size]; p = data + size
print('blocks:', sorted(blocks))
if 12 in blocks:
    b = blocks[12]
    feat, = struct.unpack_from('<H', b, 17)
    print('driver features: drrs_enabled=%d psr_enabled=%d dmrrs=%d (raw 0x%04x)' % ((feat >> 5) & 1, (feat >> 9) & 1, (feat >> 12) & 1, feat))
if 40 in blocks:
    b = blocks[40]
    panel_type = b[0]
    dps, = struct.unpack_from('<I', b, 16)
    print('lfp options: panel_type=%d dps_panel_type_bits=0x%08x' % (panel_type, dps))
    if panel_type <= 15:
        mode = (dps >> (panel_type * 2)) & 3
        print('  DRRS mode for this panel: %d (%s)' % (mode, {0: 'static', 2: 'seamless'}.get(mode, 'not supported')))
    else:
        print('  panel_type > 15: this driver version would skip DRRS parsing')
if 9 in blocks:
    b = blocks[9]
    print('psr block present, %d bytes' % len(b))
