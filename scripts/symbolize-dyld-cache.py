#!/usr/bin/env python3
"""Resolve unslid virtual addresses inside a dyld shared cache (symbolize).

Usage: symbolize-dyld-cache.py <cache-file> <pc> [<pc> ...]

The on-device crash reporter prints PCs and the cache slide; the unslid
address (PC - slide) falls inside one of the cache's images. This script
parses the dyld_shared_cache header (v1 layout), finds the image whose
mapping covers each address, and walks that image's export trie for the
nearest symbol at or below it. Read-only.
"""
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


def walk_trie(blob, pc, image_base):
    """Return (va, symbol) of the nearest export at or below pc, or None."""
    best = None

    def visit(node, prefix):
        nonlocal best
        terminal, p = uleb128(blob, node)
        if terminal:
            flags, q = uleb128(blob, p)
            if not flags & EXPORT_FLAGS_REEXPORT:
                sym_off, q = uleb128(blob, q)
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
    path = sys.argv[1]
    targets = [int(x, 0) for x in sys.argv[2:]]
    data = open(path, 'rb').read(64 * 1024 * 1024)

    if data[:8] != b'dyldV1  ':
        raise SystemExit(f'not a v1 dyld cache: {data[:8]!r}')
    mapping_offset, mapping_count, images_offset, images_count = struct.unpack_from('<IIII', data, 20)
    maps = []
    for i in range(mapping_count):
        addr, size, off = struct.unpack_from('<QQQ', data, mapping_offset + 32 * i)[:3]
        maps.append((addr, size, off))

    def file_offset(vmaddr):
        for addr, size, off in maps:
            if addr <= vmaddr < addr + size:
                return off + (vmaddr - addr)
        return None

    images = []
    for i in range(images_count):
        base = images_offset + 32 * i
        path_off, _mtime, _inode, _pad, addr, size = struct.unpack_from('<IiiiQQ', data, base)
        end = data.index(b'\0', path_off)
        name = data[path_off:end].decode('utf-8', 'replace')
        images.append((addr, size, name))
    print(f'cache: {len(images)} images, {mapping_count} mappings')

    for pc in targets:
        hit = next((im for im in images if im[1] and im[0] <= pc < im[0] + im[1]), None)
        if not hit:
            lower = [im for im in images if im[0] <= pc]
            if lower:
                near = max(lower, key=lambda im: im[0])
                print(f'pc {pc:#x}: not inside a main-cache image; nearest image below is '
                      f'{near[2]} @ {near[0]:#x} (possibly in a sub-cache)')
            else:
                print(f'pc {pc:#x}: not found in this cache')
            continue
        addr, size, name = hit
        print(f'pc {pc:#x}: {name} (image {addr:#x}..{addr + size:#x}, +{pc - addr:#x} into it)')
        foff = file_offset(addr)
        if foff is None:
            continue
        ncmds, = struct.unpack_from('<I', data, foff + 16)
        pos = foff + 32
        export_off = export_size = 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack_from('<II', data, pos)
            if cmd in (LC_DYLD_INFO, LC_DYLD_INFO_ONLY):
                export_off, export_size = struct.unpack_from('<II', data, pos + 8)
                break
            pos += cmdsize
        if not (export_off and export_size):
            print('       (image exports no trie)')
            continue
        trie_foff = file_offset(addr + export_off)
        if trie_foff is None:
            continue
        blob = data[trie_foff:trie_foff + export_size]
        best = walk_trie(blob, pc, addr)
        if best:
            va, sym = best
            print(f'       nearest export: {sym.decode("utf-8", "replace")} @ {va:#x} '
                  f'(+{pc - va:#x} bytes past its start)')
        else:
            print('       no export at or below the address in this trie')


if __name__ == '__main__':
    main()
