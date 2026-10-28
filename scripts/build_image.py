#!/usr/bin/env python3
"""Build a private S12A boot1 image offline; never selects or flashes a slot."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'device'))
import repack_boot as boot
from elf_abi import validate_bridge
from prepare_config import valid_server

RUNTIME_FILES = ('init.sh', 'client_session.sh', 'client_wake.sh', 'network_session.sh',
                 'network_start.sh', 'dhcp_ram.sh', 'panel_start.sh')
REQUIRED_CONFIG = ('client.json', 'server.json', 'wpa.conf', 'wifi-mac', 'token')
OPTIONAL_CONFIG = ('upstream-token', 'ca.pem')


def private_read(path, maximum=65536):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as f:
        s = os.fstat(f.fileno())
        if not stat.S_ISREG(s.st_mode) or s.st_uid != os.geteuid() or s.st_mode & 0o077 or not 0 < s.st_size <= maximum:
            raise ValueError('private owned regular configuration file required')
        raw = f.read(maximum + 1)
        if len(raw) != s.st_size:
            raise ValueError('configuration changed during read')
        return raw


def newc(name, data, mode, ino):
    encoded = name.encode('ascii')
    if name.startswith('/') or any(x in ('', '.', '..') for x in name.split('/')):
        raise ValueError('noncanonical CPIO path')
    fields = (ino, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, 0, len(data), 0, 0, 0, 0, len(encoded) + 1, 0)
    h = b'070701' + b''.join(f'{x:08x}'.encode() for x in fields)
    n = encoded + b'\0'
    return h + n + b'\0' * (boot.align(len(h) + len(n), 4) - len(h) - len(n)) + data + b'\0' * (boot.align(len(data), 4) - len(data))


def extend(original, init, files):
    changed, _ = boot.replace_init(original, init)
    entries, _ = boot.parse_cpio(changed)
    normalize = lambda n: n.removeprefix(b'./')
    mapping = {normalize(e.name): e for e in entries}
    if len(mapping) != len(entries) or any(n == b's12' or n.startswith(b's12/') for n in mapping):
        raise ValueError('CPIO canonical collision or custom runtime already exists')
    if b'run' in mapping and not stat.S_ISDIR(mapping[b'run'].mode):
        raise ValueError('CPIO /run must be a directory')
    additions = {'s12': (b'', stat.S_IFDIR | 0o700), 's12/private': (b'', stat.S_IFDIR | 0o700)}
    if b'run' not in mapping:
        additions['run'] = (b'', stat.S_IFDIR | 0o755)
    additions.update({n: (data, stat.S_IFREG | mode) for n, (data, mode) in files.items()})
    ino = max(int(e.header[6:14], 16) for e in entries) + 1
    parts = [e.raw for e in entries if e.name != b'TRAILER!!!']
    for name in sorted(additions, key=lambda n: (n.count('/'), n)):
        if name.encode() in mapping:
            raise ValueError('CPIO added name already exists')
        data, mode = additions[name]
        parts.append(newc(name, data, mode, ino))
        ino += 1
    parts.append(mapping[b'TRAILER!!!'].raw)
    cpio = b''.join(parts)
    cpio += b'\0' * (boot.align(len(cpio), 512) - len(cpio))
    after, tail = boot.parse_cpio(cpio)
    assert b''.join(e.raw for e in after) + tail == cpio
    after_map = {normalize(e.name): e for e in after}
    for e in boot.parse_cpio(original)[0]:
        if normalize(e.name) != b'init' and e.raw != after_map[normalize(e.name)].raw:
            raise ValueError('non-init original CPIO member changed')
    return cpio, sorted(additions)


def tone(notes):
    samples = []
    for hz, duration_ms in notes:
        n = duration_ms * 16
        for i in range(n):
            envelope = min(1, i / 80, (n - 1 - i) / 80)
            samples.append(round(2000 * max(0, envelope) * math.sin(2 * math.pi * hz * i / 16000)) if hz else 0)
    return struct.pack('<' + 'h' * len(samples), *samples)


def config_payloads(directory):
    directory = Path(directory)
    st = directory.lstat()
    if not stat.S_ISDIR(st.st_mode) or st.st_uid != os.geteuid() or st.st_mode & 0o077:
        raise ValueError('configuration directory must be private, owned, and not a symlink')
    blobs = {n: private_read(directory / n, 512 * 1024 if n == 'ca.pem' else 65536) for n in REQUIRED_CONFIG}
    for n in OPTIONAL_CONFIG:
        if (directory / n).exists():
            blobs[n] = private_read(directory / n, 512 * 1024 if n == 'ca.pem' else 4096)
    client = json.loads(blobs['client.json'])
    server = json.loads(blobs['server.json'])
    mac = blobs['wifi-mac'].decode('ascii').strip()
    if not re.fullmatch(r'[0-9a-f]{2}(?::[0-9a-f]{2}){5}', mac):
        raise ValueError('actual Wi-Fi MAC required')
    if set(client) != {'url', 'deviceid', 'clientid', 'token-file'} or client['url'] != 'ws://127.0.0.1:18080/xiaozhi/v1/' or client['token-file'] != '/s12/private/token' or client['deviceid'] != mac:
        raise ValueError('client must route through the local authenticated Xiaozhi bridge')
    if not isinstance(client['clientid'], str) or not 1 <= len(client['clientid']) < 128 or any(ord(c) < 33 or ord(c) > 126 for c in client['clientid']):
        raise ValueError('client UUID/header value invalid')
    if set(server) != {'listen', 'upstream_url', 'device_id', 'local_token_file', 'upstream_token_file', 'ca_file'}:
        raise ValueError('bridge configuration fields differ from documented schema')
    parsed = valid_server(server['upstream_url'])
    if server.get('listen') != '127.0.0.1:18080' or server.get('device_id') != mac or server.get('local_token_file') != '/s12/private/token':
        raise ValueError('bridge identity/loopback authentication mismatch')
    if server.get('upstream_token_file') not in ('', '/s12/private/upstream-token') or server.get('ca_file') not in ('', '/s12/private/ca.pem'):
        raise ValueError('bridge token and CA paths must remain inside private RAM image')
    if bool(server['upstream_token_file']) != ('upstream-token' in blobs) or bool(server['ca_file']) != ('ca.pem' in blobs):
        raise ValueError('bridge references missing or unused private files')
    if parsed.scheme == 'wss' and 'ca.pem' not in blobs:
        raise ValueError('TLS firmware image requires its public CA bundle')
    if not re.fullmatch(rb'[0-9a-f]{64}\n?', blobs['token']):
        raise ValueError('local bridge token must be 64 lowercase random hex characters')
    if b'include' in blobs['wpa.conf'] or b'\0' in blobs['wpa.conf'] or b'update_config=0' not in blobs['wpa.conf'] or b'ctrl_interface=/run/s12/wpa-ctrl' not in blobs['wpa.conf']:
        raise ValueError('WPA config must disable persistence and use RAM control sockets')
    return {'s12/client.json': (blobs.pop('client.json'), 0o600), **{'s12/private/' + n: (v, 0o600) for n, v in blobs.items()}}


def build(source_path, build_dir, config_dir, output, build_unix):
    source = boot.parse_android(Path(source_path).read_bytes(), require_exact_source=True)
    if not 1704067200 <= build_unix <= 2147483647:
        raise ValueError('build timestamp must fit original signed ARM time_t')
    build_dir = Path(build_dir)
    artifacts = json.loads((build_dir / 'build.json').read_text())['artifacts']
    files = config_payloads(config_dir)
    for name in ('client', 'panel', 'bridge'):
        path = build_dir / (name + '.arm')
        raw = path.read_bytes()
        if len(raw) != artifacts[path.name]['bytes'] or boot.sha256(raw) != artifacts[path.name]['sha256']:
            raise ValueError('compiled artifact differs from offline build manifest')
        if name == 'bridge':
            validate_bridge(path)
        files['s12/' + path.name] = (raw, 0o500)
        files['s12/' + name + '.sha256'] = ((boot.sha256(raw) + '\n').encode(), 0o600)
    for name in RUNTIME_FILES:
        if name != 'init.sh':
            files['s12/' + name] = ((ROOT / 'runtime' / name).read_bytes(), 0o500)
    files['s12/alsa.conf'] = ((ROOT / 'device/minimal_alsa.conf').read_bytes(), 0o600)
    files['s12/panel-rgb-confirmed'] = (b'yes\n', 0o600)
    files['s12/build-unix'] = ((str(build_unix) + '\n').encode(), 0o600)
    files['s12/startup-feedback-enabled'] = (b'yes\n', 0o600)
    for name, notes in {'wakeup.pcm': [(880, 100), (0, 30), (1320, 100)],
                        'bootup.pcm': [(660, 120), (0, 35), (880, 120), (0, 35), (1320, 140)],
                        'wifi-unavailable.pcm': [(440, 140), (0, 60), (330, 140)],
                        'cloud-unavailable.pcm': [(550, 120), (0, 50), (440, 120)]}.items():
        files['s12/' + name] = (tone(notes), 0o600)
    cpio, additions = extend(boot.gunzip_bounded(source.ramdisk), (ROOT / 'runtime/init.sh').read_bytes(), files)
    ramdisk = boot.gzip_deterministic(cpio)
    image = boot.repack(source, ramdisk)
    parsed = boot.parse_android(image)
    assert parsed.kernel == source.kernel and parsed.second == source.second
    report = {'schema': 1, 'image': {'file': 'boot.img', 'bytes': len(image), 'sha256': boot.sha256(image)},
              'stock_boot_sha256': boot.sha256(source.blob), 'kernel_preserved': True, 'dtb_preserved': True,
              'other_header_fields_preserved': True, 'original_non_init_cpio_members_preserved': True,
              'added_paths': additions, 'build_unix': build_unix, 'experimental': True,
              'device_access': False, 'flash_written': False, 'boot0_selected_or_modified': False,
              'env_writer_included': False, 'hardware_tested': False,
              'contains_user_secrets_and_vendor_boot_components': True,
              'redistributable_firmware_artifact': False, 'sources_are_separate_from_private_outputs': True,
              'prompt_source': 'original mathematical sine tones; no vendor sound samples',
              'artifacts': artifacts}
    output = Path(output)
    output.mkdir(mode=0o700)
    for name, raw in {'boot.img': image, 'ramdisk.cpio.gz': ramdisk,
                      'manifest.json': (json.dumps(report, indent=2) + '\n').encode()}.items():
        boot.write_private(output / name, raw)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stock-boot', type=Path, required=True, help='user-supplied original S12A 1.76.54 page-aligned Android boot image')
    p.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    p.add_argument('--config-dir', type=Path, required=True)
    p.add_argument('--output-dir', type=Path, required=True, help='new private output directory; never publish the generated image')
    p.add_argument('--build-unix', type=int, default=int(os.environ.get('SOURCE_DATE_EPOCH', int(time.time()))))
    a = p.parse_args()
    try:
        result = build(a.stock_boot, a.build_dir, a.config_dir, a.output_dir, a.build_unix)
    except (ValueError, OSError, KeyError) as exc:
        p.exit(1, f'image refused: {exc}\n')
    print(json.dumps({k: result[k] for k in ('image', 'kernel_preserved', 'dtb_preserved', 'device_access', 'flash_written', 'hardware_tested', 'redistributable_firmware_artifact')}, indent=2))


if __name__ == '__main__':
    main()
