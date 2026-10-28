#!/usr/bin/env python3
"""Pure offline S12A Android-v0/newc helpers; use scripts/build_image.py.

No device, deployment, default private input or standalone probe entry point.
"""
from __future__ import annotations

from dataclasses import dataclass
import gzip
import hashlib
import io
import os
from pathlib import Path
import stat
import struct
import zlib

SOURCE_SHA256 = 'ddab3656e7a97ea2e2ba2028b92e47bcfcbb8816ce761b0c71f137520dda6445'
SLOT_SIZE = 8 * 1024 * 1024
CPIO_LIMIT = 32 * 1024 * 1024
ID_OFFSET = 576


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) // boundary * boundary


def android_id(kernel: bytes, ramdisk: bytes, second: bytes) -> bytes:
    digest = hashlib.sha1()
    for part in (kernel, ramdisk, second):
        digest.update(part)
        digest.update(struct.pack('<I', len(part)))
    return digest.digest()


@dataclass(frozen=True)
class Android:
    blob: bytes
    header: bytes
    kernel: bytes
    ramdisk: bytes
    second: bytes
    page: int
    starts: tuple[int, int, int]
    paddings: tuple[bytes, bytes, bytes]


def parse_android(blob: bytes, *, require_exact_source: bool = False) -> Android:
    if require_exact_source and sha256(blob) != SOURCE_SHA256:
        raise ValueError('Source is not the exact audited, page-aligned S12A image.')
    if len(blob) < 2048 or len(blob) > SLOT_SIZE or blob[:8] != b'ANDROID!':
        raise ValueError('Expected a bounded Android boot image, not an OTA container.')
    fields = struct.unpack_from('<10I', blob, 8)
    ks, _, rs, _, ss, _, _, page, dt, unused = fields
    if page != 2048 or dt or unused or not ks or not rs or not ss:
        raise ValueError('Only this S12A v0 layout with page 2048 and second is supported.')
    starts = (page, page + align(ks, page), page + align(ks, page) + align(rs, page))
    sizes = (ks, rs, ss)
    end = starts[2] + align(ss, page)
    if end != len(blob):
        raise ValueError('Image must end at its Android page boundary; no outer OTA tail.')
    components = tuple(blob[start:start + size] for start, size in zip(starts, sizes))
    pads = tuple(blob[start + size:start + align(size, page)] for start, size in zip(starts, sizes))
    if any(any(p) for p in pads):
        raise ValueError('Unexpected nonzero component padding; refusing to reinterpret it.')
    if blob[ID_OFFSET:ID_OFFSET + 20] != android_id(*components):
        raise ValueError('Android SHA1 ID does not match component data and LE32 sizes.')
    return Android(blob, blob[:page], *components, page, starts, pads)


def repack(source: Android, ramdisk: bytes) -> bytes:
    if not ramdisk or len(ramdisk) > SLOT_SIZE:
        raise ValueError('Invalid replacement ramdisk size.')
    header = bytearray(source.header)
    struct.pack_into('<I', header, 16, len(ramdisk))
    header[ID_OFFSET:ID_OFFSET + 20] = android_id(source.kernel, ramdisk, source.second)
    result = (bytes(header) + source.kernel + source.paddings[0] + ramdisk +
              b'\0' * (align(len(ramdisk), source.page) - len(ramdisk)) +
              source.second + source.paddings[2])
    parsed = parse_android(result)
    if parsed.kernel != source.kernel or parsed.second != source.second:
        raise AssertionError('Preserved components changed.')
    allowed = set(range(16, 20)) | set(range(ID_OFFSET, ID_OFFSET + 20))
    if any(a != b for i, (a, b) in enumerate(zip(source.header, parsed.header)) if i not in allowed):
        raise AssertionError('Unexpected header-field modification.')
    return result


def gunzip_bounded(data: bytes) -> bytes:
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    result = decoder.decompress(data, CPIO_LIMIT + 1)
    if len(result) > CPIO_LIMIT or decoder.unconsumed_tail or not decoder.eof or decoder.unused_data:
        raise ValueError('Expected one complete bounded gzip stream with no external tail.')
    return result


@dataclass(frozen=True)
class CpioEntry:
    name: bytes
    header: bytes
    name_and_padding: bytes
    data: bytes
    raw: bytes
    mode: int
    uid: int
    gid: int
    nlink: int


def parse_cpio(blob: bytes) -> tuple[list[CpioEntry], bytes]:
    if not 0 < len(blob) <= CPIO_LIMIT:
        raise ValueError('Invalid CPIO size.')
    entries, seen = [], set()
    pos = 0
    while pos + 110 <= len(blob):
        header = blob[pos:pos + 110]
        if header[:6] != b'070701':
            raise ValueError('Only unchecksummed newc CPIO is supported.')
        try:
            numbers = [int(header[6 + i * 8:14 + i * 8], 16) for i in range(13)]
        except ValueError as exc:
            raise ValueError('Malformed CPIO header.') from exc
        _, mode, uid, gid, nlink, _, size, _, _, _, _, namesize, checksum = numbers
        if not namesize or namesize > 4096 or checksum:
            raise ValueError('Invalid CPIO name/checksum.')
        data_start = align(pos + 110 + namesize, 4)
        end = align(data_start + size, 4)
        if end > len(blob):
            raise ValueError('Truncated CPIO entry.')
        name_field = blob[pos + 110:pos + 110 + namesize]
        if not name_field.endswith(b'\0') or b'\0' in name_field[:-1]:
            raise ValueError('Invalid CPIO name termination.')
        name = name_field[:-1]
        if name in seen:
            raise ValueError('Duplicate CPIO path.')
        seen.add(name)
        entry = CpioEntry(name, header, blob[pos + 110:data_start],
                          blob[data_start:data_start + size], blob[pos:end], mode, uid, gid, nlink)
        entries.append(entry)
        pos = end
        if name == b'TRAILER!!!':
            if size or any(blob[pos:]):
                raise ValueError('Unexpected data after CPIO trailer.')
            return entries, blob[pos:]
    raise ValueError('Missing CPIO trailer.')


def replace_init(blob: bytes, script: bytes) -> tuple[bytes, dict]:
    entries, tail = parse_cpio(blob)
    if b''.join(e.raw for e in entries) + tail != blob:
        raise AssertionError('CPIO parse/serialize roundtrip differs.')
    matches = [e for e in entries if e.name in (b'init', b'./init')]
    if len(matches) != 1:
        raise ValueError('Expected exactly one root init entry.')
    old = matches[0]
    if stat.S_IFMT(old.mode) != stat.S_IFREG or not old.mode & 0o111 or old.nlink != 1:
        raise ValueError('Init must be an executable regular file without hardlinks.')
    if not script.startswith(b'#!/bin/sh\n') or b'\0' in script or len(script) > 65536:
        raise ValueError('Expected a bounded /bin/sh init script.')
    changed = []
    for e in entries:
        if e is old:
            header = e.header[:54] + f'{len(script):08x}'.encode() + e.header[62:]
            changed.append(header + e.name_and_padding + script + b'\0' * (align(len(script), 4) - len(script)))
        else:
            changed.append(e.raw)
    rebuilt = b''.join(changed)
    rebuilt += b'\0' * (align(len(rebuilt), 512) - len(rebuilt))
    after, _ = parse_cpio(rebuilt)
    if len(after) != len(entries):
        raise AssertionError('CPIO entry count changed.')
    for before, new in zip(entries, after):
        if before is not old and before.raw != new.raw:
            raise AssertionError('A non-init CPIO entry changed.')
    return rebuilt, {'changed_paths': ['init'], 'entries_including_trailer': len(entries),
                     'old_init_sha256': sha256(old.data), 'new_init_sha256': sha256(script),
                     'init_mode_octal': oct(old.mode), 'uid': old.uid, 'gid': old.gid,
                     'nlink': old.nlink, 'other_entries_byte_identical': True,
                     'original_cpio_roundtrip_byte_identical': True,
                     'trailing_archive_padding': 'zero padding to 512-byte boundary'}


def gzip_deterministic(blob: bytes) -> bytes:
    output = io.BytesIO()
    with gzip.GzipFile(fileobj=output, mode='wb', filename='', mtime=0, compresslevel=9) as stream:
        stream.write(blob)
    result = output.getvalue()
    if gunzip_bounded(result) != blob:
        raise AssertionError('gzip roundtrip differs.')
    return result


def write_private(path: Path, data: bytes) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)

