"""Seeds for fuzz_catalog (F1, f1-lang): small compiled catalogs in tests/fuzz/seeds/catalog.

Each seed is a key table and one language's inflated texts, as fuzz_catalog.cpp reads them: the key
table's size (32-bit little-endian), its bytes, then the texts' bytes. The format is
src/core/LangCatalog.h's; this script writes it the way tools/strings/fwstrings.cpp does.

  python tests/fuzz/make_catalog_seeds.py
"""
import os
import struct

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'seeds', 'catalog')


def step(h, byte):
    return ((h ^ (byte & 0xFF)) * 16777619) & 0xFFFFFFFF


def units(s):
    b = s.encode('utf-16-le')
    return [b[i] | b[i + 1] << 8 for i in range(0, len(b), 2)]


def hash_key(seed, context, en):
    h = 2166136261 ^ seed
    if context is not None:
        for u in units(context):
            h = step(step(h, u), u >> 8)
        h = step(step(h, 4), 0)
    for u in units(en):
        h = step(step(h, u), u >> 8)
    return h


def keys_check(seed, hashes, places):
    h = 2166136261 ^ seed
    for x, p in zip(hashes, places):
        for b in range(4):
            h = step(h, x >> (8 * b))
        h = step(step(h, p), p >> 8)
    return h


def seed_file(seed, keys, texts):
    """keys: (context, english) in the template's order; texts: UTF-8 str per key ('' untranslated)."""
    hs = [hash_key(seed, c, e) for c, e in keys]
    order = sorted(range(len(hs)), key=lambda i: hs[i])
    hashes = [hs[i] for i in order]
    gaps = [h - p for h, p in zip(hashes, [0] + hashes[:-1])]
    k = min(range(32), key=lambda t: (sum(t + 1 + (g >> t) for g in gaps), t))
    w = max(1, (len(hs) - 1).bit_length())
    bits = []  # Rice-coded differences, then the places in w bits; low bits first
    for g in gaps:
        bits += [1] * (g >> k) + [0] + [(g >> b) & 1 for b in range(k)]
    for i in order:
        bits += [(i >> b) & 1 for b in range(w)]
    packed = sum(b << n for n, b in enumerate(bits))
    table = b'FWLK' + struct.pack('<III', 2, seed, len(hs)) + bytes([k, w])
    table += packed.to_bytes((len(bits) + 7) // 8, 'little')
    body = b'FWLT' + struct.pack('<III', 1, len(hs), keys_check(seed, hashes, order))
    body += b''.join(t.encode('utf-8') + b'\0' for t in texts)
    return struct.pack('<I', len(table)) + table + body


def main():
    os.makedirs(OUT, exist_ok=True)
    seeds = {
        'three.bin': seed_file(0, [(None, 'Bold'), ('verb', 'Open'), (None, 'Open')], ['Vet', '', 'Openen …']),
        'placeholders.bin': seed_file(7, [(None, 'Page {0} of {1}'), (None, '&Save\tCtrl+S'), (None, 'Width: %d px')],
                                      ['Seite {0} von {1}', '&Speichern\tStrg+S', 'Breite: %d px']),
        'wide.bin': seed_file(3, [(None, 'Font'), (None, 'Size'), (None, 'Color'), (None, 'Clear')],
                              ['Police', 'Größe', '色 😀', '']),
    }
    for name, data in seeds.items():
        with open(os.path.join(OUT, name), 'wb') as f:
            f.write(data)
        print('wrote', name, len(data), 'bytes')


if __name__ == '__main__':
    main()
