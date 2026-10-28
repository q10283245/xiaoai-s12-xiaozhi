"""Offline ARM32 executable validation against a user's S12A libraries."""
from pathlib import Path
import re
import struct


def elf(path):
    data = Path(path).read_bytes()
    if len(data) < 52:
        raise ValueError('ELF header missing')
    head = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    if head[0][:7] != b'\x7fELF\x01\x01\x01' or head[2] != 40:
        raise ValueError('ARM32 little-endian ELF required')
    if head[12] > 8192 or head[11] != 40 or head[6] + head[12] * head[11] > len(data):
        raise ValueError('ELF section table bounds')
    sections = [struct.unpack_from('<IIIIIIIIII', data, head[6] + i * head[11]) for i in range(head[12])]
    return data, head, sections


def symbols(path):
    data, _, sections = elf(path)
    public, missing = set(), set()
    for section in sections:
        if section[1] != 11:
            continue
        strings = sections[section[6]]
        tab = data[strings[4]:strings[4] + strings[5]]
        if section[9] != 16:
            raise ValueError('ELF dynamic symbol size')
        for at in range(section[4], section[4] + section[5], section[9]):
            name, _, _, info, _, index = struct.unpack_from('<IIIBBH', data, at)
            if name and info >> 4 in (1, 2):
                label = tab[name:tab.index(0, name)].decode()
                if index:
                    public.add(label)
                elif info >> 4 == 1:
                    missing.add(label)
    return public, missing


def validate(path, vendor_libraries, expected_needed):
    data, head, sections = elf(path)
    if head[1] != 3 or head[7] & 0x600 != 0x400:
        raise ValueError('ARM32 hard-float PIE required')
    if any(x in data for x in (b'S12_TEST_', b'S12_PANEL_TEST', b'--test-root')):
        raise ValueError('offline test hooks in production binary')
    interp = []
    for i in range(head[10]):
        ph = struct.unpack_from('<IIIIIIII', data, head[5] + i * head[9])
        if ph[0] == 3:
            interp.append(data[ph[1]:ph[1] + ph[4]].rstrip(b'\0').decode())
        if ph[0] == 1 and (ph[1] + ph[4] > len(data) or ph[4] > ph[5] or not ph[7] or (ph[2] - ph[1]) % ph[7]):
            raise ValueError('ELF load segment bounds/alignment')
    if interp != ['/lib/ld-linux-armhf.so.3']:
        raise ValueError('S12A vendor ELF loader required')
    needed = []
    for section in sections:
        if section[1] in (4, 9):
            for at in range(section[4], section[4] + section[5], section[9]):
                _, info = struct.unpack_from('<II', data, at)
                if info & 255 == 20:
                    raise ValueError('unsafe ARM COPY relocation')
        if section[1] == 6:
            strings = sections[section[6]]
            tab = data[strings[4]:strings[4] + strings[5]]
            for at in range(section[4], section[4] + section[5], section[9]):
                tag, value = struct.unpack_from('<II', data, at)
                if tag == 1:
                    needed.append(tab[value:tab.index(0, value)].decode())
    if set(needed) != set(expected_needed):
        raise ValueError('unexpected shared-library dependencies: ' + repr(needed))
    versions = sorted(set(x.decode() for x in re.findall(rb'GLIBC_(\d+\.\d+(?:\.\d+)?)\x00', data)))
    if any(tuple(map(int, x.split('.'))) > (2, 25) for x in versions):
        raise ValueError('glibc newer than the S12A 2.25 ABI')
    public, undefined = symbols(path)
    if {'stdin', 'stdout', 'stderr', 'environ', '__environ'} & public:
        raise ValueError('libc global incorrectly defined by the executable')
    if '__fxstat64' not in undefined or undefined & {'fstat', 'fstat64', '__fxstat'}:
        raise ValueError('explicit old ARM stat64 ABI required')
    vendor = set()
    for library in vendor_libraries:
        vendor.update(symbols(library)[0])
    if undefined - vendor:
        raise ValueError('symbols absent from user vendor libraries: ' + repr(sorted(undefined - vendor)))
    return {'architecture': 'ARM32 hard-float PIE', 'interpreter': interp[0], 'needed': needed,
            'glibc_versions': versions, 'unsafe_copy_relocations': False,
            'all_vendor_symbols_present': True, 'production_test_hooks': False}


def validate_bridge(path):
    data, head, _ = elf(path)
    if head[1] != 2:
        raise ValueError('static ARM bridge executable required')
    for i in range(head[10]):
        p = struct.unpack_from('<IIIIIIII', data, head[5] + i * head[9])
        if p[0] in (2, 3) or p[1] + p[4] > len(data):
            raise ValueError('Go bridge must be static with no dynamic loader')
    return {'architecture': 'linux/arm', 'static': True, 'cgo': False}
