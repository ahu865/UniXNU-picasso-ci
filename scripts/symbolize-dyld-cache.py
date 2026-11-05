#!/usr/bin/env python3
"""Resolve unslid virtual addresses inside a (split) dyld shared cache.

Usage: symbolize-dyld-cache.py <cache-file> <pc> [<pc> ...]

<cache-file> is the main file (dyld_shared_cache_arm64); the numbered
sub-cache files beside it are opened automatically. The on-device crash
reporter prints PCs and the cache slide; the unslid address (PC - slide)
belongs to one of the images listed in the main header. This script maps the
address to its image, then walks that image's export trie (wherever its
LINKEDIT lives, main file or a sub-cache) for the nearest symbol at or below
the address. Read-only.
"""
import glob
import mmap
import os
import struct
import sys

LC_DYLD_INFO = 0x22
LC_DYLD_INFO_ONLY = 0x80000022
EXPORT_FLAGS_REEXPORT = 0x08


def uleb128(data, pos):
    result = 0
    shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7f) << shift
        if not byte & 0x80:
            return result, pos
        shift += 7


class Cache:
    def __init__(self, path):
        self.main = None
        self.files = {}   # path -> mmap
        self.maps = []    # (addr, size, fileoff, path)
        self._open(path)
        self.main = path
        pattern = os.path.basename(path) + '.*'
        for sub in sorted(glob.glob(os.path.join(os.path.dirname(path), pattern))):
            self._open(sub)
        self.images = self._parse_main_images()

    def _open(self, path):
        try:
            f = open(path, 'rb')
        except OSError:
            return
        mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        f.close()
        if mm[:8] != b'dyld_v1 ':
            # the magic is 16 bytes ("dyld_v1   arm64\0"); the first 8 identify the format
            print(f'skip {path}: first bytes {bytes(mm[:16])!r}', file=sys.stderr)
            mm.close()
            return
        self.files[path] = mm
        mapping_offset, mapping_count, _, _ = struct.unpack_from('<IIII', mm, 16)
        for i in range(mapping_count):
            addr, size, off = struct.unpack_from('<QQQ', mm, mapping_offset + 32 * i)
            self.maps.append((addr, size, off, path))

    def _parse_main_images(self):
        mm = self.files[self.main]
        _, _, images_offset, images_count = struct.unpack_from('<IIII', mm, 16)
        images = []
        for i in range(images_count):
            base = images_offset + 32 * i
            path_off, _mtime, _inode, _pad, addr, size = struct.unpack_from('<IiiiQQ', mm, base)
            end = mm.index(b'\0', path_off)
            name = mm[path_off:end].decode('utf-8', 'replace')
            images.append((addr, size, name))
        return images

    def read(self, vmaddr, length):
        for addr, size, off, path in self.maps:
            if addr <= vmaddr < addr + size:
                mm = self.files[path]
                start = off + (vmaddr - addr)
                if start + length <= len(mm):
                    return mm[start:start + length]
        return None

    def close(self):
        for mm in self.files.values():
            mm.close()


def walk_trie(blob, pc, image_base):
    best = None

    def visit(node, prefix):
        nonlocal best
        terminal, p = uleb128(blob, node)
        if terminal:
            flags, q = uleb128(blob, p)
            if not flags & EXPORT_FLAGS_REEXPORT:
                sym_off, _q = uleb128(blob, q)
                va = image_base + sym_off
                if va <= pc and (best is None or va > best[0]):
                    best = (va, prefix)
        p = node + terminal
        if p < len(blob) and blob[p] == 0:
            p += 1
        while p < len(blob) and blob[p] != 0:
            edge_len = blob[p]
            edge = blob[p + 1:p + 1 + edge_len]
            child_off = blob[p + 1 + edge_len]
            visit(node + child_off, prefix + edge)
            p += 1 + edge_len + 1

    visit(0, b'')
    return best


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cache = Cache(sys.argv[1])
    targets = [int(x, 0) for x in sys.argv[2:]]
    print(f'cache: {len(cache.files)} file(s), {len(cache.maps)} mappings, {len(cache.images)} images')
    for pc in targets:
        hit = next((im for im in cache.images if im[1] and im[0] <= pc < im[0] + im[1]), None)
        if not hit:
            lower = [im for im in cache.images if im[0] <= pc]
            if lower:
                near = max(lower, key=lambda im: im[0])
                print(f'pc {pc:#x}: not inside an image; nearest image below is '
                      f'{near[2]} @ {near[0]:#x}')
            else:
                print(f'pc {pc:#x}: not found in this cache')
            continue
        addr, size, name = hit
        print(f'pc {pc:#x}: {name} (image {addr:#x}..{addr + size:#x}, +{pc - addr:#x} into it)')
        header = cache.read(addr, 4096)
        if not header:
            print('       (image header bytes not mapped by any cache file)')
            continue
        ncmds, = struct.unpack_from('<I', header, 16)
        pos = 32
        export_off = export_size = 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', header, pos)
            if cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                export_off, export_size = struct.unpack_from('<II', header, pos + 8)
                break
            pos += cmdsize
        if not (export_off and export_size):
            print('       (image exports no trie)')
            continue
        blob = cache.read(addr + export_off, export_size)
        if not blob:
            print('       (export trie bytes not mapped by any cache file)')
            continue
        best = walk_trie(blob, pc, addr)
        if best:
            va, sym = best
            print(f'       nearest export: {sym.decode("utf-8", "replace")} @ {va:#x} '
                  f'(+{pc - va:#x} bytes past its start)')
        else:
            print('       no export at or below the address in this trie')
    cache.close()


if __name__ == '__main__':
    main()
