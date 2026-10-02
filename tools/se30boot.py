#!/usr/bin/env python3
"""se30boot.py - put se30boot's stage 1 in a Mac volume's boot blocks.

  se30boot.py install TARGET STAGE1 [HFS-PATH]   (default Linux:se30boot)
  se30boot.py remove TARGET
  se30boot.py info TARGET

TARGET is a disk image or device with an Apple partition map; the first
Apple_HFS partition is the volume. Stage 2 must already be on it as
HFS-PATH, in one piece (install-se30boot.sh copies it with hfsutils).

System 7.1's boot blocks are 262 bytes of code (0x8a-0x18f) and filler to
0x3ff. install puts stage 1 in the filler, points the entry branch at it,
and records where stage 2 is. The original code is never touched, and
remove puts the branch and the filler back as they were.
"""
import struct
import sys

ORIG_BRANCH = bytes.fromhex('60000086')   # bra.w 0x8a
OUR_BRANCH = bytes.fromhex('6000018c')    # bra.w 0x190
HERE, END = 0x190, 0x400
MARK = b'se30boot'


def find_hfs(f):
    f.seek(0)
    if f.read(2) != b'ER':
        sys.exit('no Apple partition map (no driver descriptor)')
    for i in range(1, 64):
        f.seek(512 * i)
        e = f.read(512)
        if e[:2] != b'PM':
            break
        start, size = struct.unpack('>II', e[8:16])
        if e[48:80].split(b'\0')[0] == b'Apple_HFS':
            return start * 512, size * 512
    sys.exit('no Apple_HFS partition')


class Volume:
    def __init__(self, f, base):
        self.f, self.base = f, base
        mdb = self.read(1024, 512)
        if mdb[:2] != b'BD':
            sys.exit('the Apple_HFS partition is not HFS')
        self.name = mdb[0x25:0x25 + mdb[0x24]].decode('mac_roman')
        self.blksz = struct.unpack('>I', mdb[0x14:0x18])[0]
        self.blkst = struct.unpack('>H', mdb[0x1c:0x1e])[0]
        size = struct.unpack('>I', mdb[0x92:0x96])[0]
        exts = self.extents(mdb[0x96:0xa2])
        if sum(c for _, c in exts) * self.blksz < size:
            sys.exit('the catalog has overflow extents; not supported here')
        self.catalog = b''.join(self.read_ext(s, c) for s, c in exts)[:size]

    def read(self, off, n):
        self.f.seek(self.base + off)
        return self.f.read(n)

    def read_ext(self, start, count):
        return self.read(self.blkst * 512 + start * self.blksz, count * self.blksz)

    @staticmethod
    def extents(rec):
        out = []
        for i in range(3):
            s, c = struct.unpack('>HH', rec[i * 4:i * 4 + 4])
            if c:
                out.append((s, c))
        return out

    def leaves(self):
        cat = self.catalog
        node = struct.unpack('>I', cat[14 + 10:14 + 14])[0]
        nodesize = struct.unpack('>H', cat[14 + 18:14 + 20])[0]
        while node:
            nd = cat[node * nodesize:(node + 1) * nodesize]
            nrecs = struct.unpack('>H', nd[10:12])[0]
            for i in range(nrecs):
                off = struct.unpack('>H', nd[nodesize - 2 * (i + 1):nodesize - 2 * i])[0]
                klen = nd[off]
                key = nd[off:off + 1 + klen]
                data = nd[off + ((1 + klen + 1) & ~1):]
                yield key, data
            node = struct.unpack('>I', nd[0:4])[0]

    def lookup(self, path):
        parent, rec = 2, None
        parts = [p for p in path.replace('/', ':').split(':') if p]
        for i, name in enumerate(parts):
            want = name.encode('mac_roman').upper()
            rec = None
            for key, data in self.leaves():
                if (struct.unpack('>I', key[2:6])[0] == parent
                        and key[7:7 + key[6]].upper() == want and data[0] in (1, 2)):
                    rec = data
                    break
            if rec is None:
                sys.exit('%s: not found on the volume' % path)
            if i < len(parts) - 1:
                parent = struct.unpack('>I', rec[6:10])[0]
        if rec[0] != 2:
            sys.exit('%s is a folder' % path)
        return rec


def boot_blocks(f, base):
    f.seek(base)
    bb = bytearray(f.read(1024))
    if bb[:2] != b'LK':
        sys.exit('no boot blocks on the volume (not a startup disk)')
    ours = bb[2:6] == OUR_BRANCH and bb[0x3f8:0x400] == MARK
    if not ours:
        if bb[2:6] != ORIG_BRANCH:
            sys.exit('unexpected boot block entry %s; not System 7.1\'s' % bb[2:6].hex())
        if len(set(bb[HERE:END])) != 1:
            sys.exit('boot blocks 0x190-0x3ff are not filler; not System 7.1\'s')
    return bb, ours


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('install', 'remove', 'info'):
        sys.exit(__doc__)
    cmd, target = sys.argv[1], sys.argv[2]
    with open(target, 'r+b' if cmd != 'info' else 'rb') as f:
        base, _ = find_hfs(f)
        bb, ours = boot_blocks(f, base)
        if cmd == 'info':
            vol = Volume(f, base)
            print('volume "%s" at %#x; boot blocks: %s' % (
                vol.name, base, 'se30boot' if ours else 'System 7.1'))
            if ours:
                off, length = struct.unpack('>II', bb[0x3f0:0x3f8])
                print('stage 2: %d bytes at volume offset %#x' % (length, off))
            return
        if cmd == 'remove':
            if not ours:
                print('se30boot is not installed')
                return
            bb[2:6] = ORIG_BRANCH
            bb[HERE:END] = b'\xda' * (END - HERE)
        else:
            stage1 = open(sys.argv[3], 'rb').read()
            path = sys.argv[4] if len(sys.argv) > 4 else 'Linux:se30boot'
            if len(stage1) != END - HERE or stage1[-8:] != MARK:
                sys.exit('%s is not stage 1' % sys.argv[3])
            vol = Volume(f, base)
            rec = vol.lookup(path)
            size = struct.unpack('>I', rec[0x1a:0x1e])[0]
            exts = vol.extents(rec[0x4a:0x56])
            if not exts or exts[0][1] * vol.blksz < size:
                sys.exit('%s is not in one piece; copy it again' % path)
            if vol.read_ext(*exts[0])[2:6] != b'SE3B':
                sys.exit('%s is not se30boot\'s stage 2' % path)
            offset = vol.blkst * 512 + exts[0][0] * vol.blksz
            length = (size + 511) & ~511
            bb[2:6] = OUR_BRANCH
            bb[HERE:END] = stage1
            bb[0x3f0:0x3f8] = struct.pack('>II', offset, length)
            print('stage 2: %d bytes at volume offset %#x' % (length, offset))
        f.seek(base)
        f.write(bb)
        print('boot blocks: %s' % ('se30boot' if cmd == 'install' else 'System 7.1'))


if __name__ == '__main__':
    main()
