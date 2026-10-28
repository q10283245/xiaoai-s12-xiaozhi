#!/usr/bin/env python3
"""Create user-private configuration; passwords/tokens never enter argv/logs."""
import argparse
import getpass
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
from urllib.parse import urlsplit
import uuid


def write_private(path, data):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as f:
        f.write(data)


def valid_server(url):
    try:
        parsed = urlsplit(url)
        port = parsed.port
    except ValueError:
        raise ValueError('server URL host or port invalid') from None
    if parsed.netloc.endswith(':') or port is not None and not 1 <= port <= 65535:
        raise ValueError('server URL port must be 1..65535 when specified')
    if parsed.scheme not in ('ws', 'wss') or not parsed.hostname or parsed.username is not None or parsed.password is not None or parsed.fragment or len(url) > 4096:
        raise ValueError('server must be a ws:// or wss:// URL without userinfo/fragment')
    if any(ord(x) < 33 or ord(x) > 126 for x in url) or not parsed.path.startswith('/'):
        raise ValueError('server URL must have a canonical absolute path')
    if parsed.scheme == 'ws':
        try:
            ip = ipaddress.ip_address(parsed.hostname)
            private = ip.is_loopback or any(ip in ipaddress.ip_network(n) for n in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', 'fc00::/7') if ip.version == ipaddress.ip_network(n).version)
            if not private or getattr(ip, 'ipv4_mapped', None) is not None:
                raise ValueError
        except ValueError:
            raise ValueError('ws requires an explicit private numeric IP; use wss for public servers') from None
    return parsed


def make_wpa(ssid, password):
    raw = ssid.encode('utf-8')
    if not 1 <= len(raw) <= 32:
        raise ValueError('Wi-Fi SSID must contain 1..32 UTF-8 bytes')
    if not 8 <= len(password.encode('utf-8')) <= 63:
        raise ValueError('Wi-Fi WPA2 password must contain 8..63 UTF-8 bytes')
    psk = hashlib.pbkdf2_hmac('sha1', password.encode('utf-8'), raw, 4096, 32).hex()
    return ('ctrl_interface=/run/s12/wpa-ctrl\nupdate_config=0\nnetwork={\n    ssid=' + raw.hex() + '\n    psk=' + psk + '\n}\n').encode()


def prepare(output, server_url, device_id, ssid, wifi_password, upstream_token='', ca_file=None):
    parsed = valid_server(server_url)
    device_id = device_id.lower()
    if not re.fullmatch(r'[0-9a-f]{2}(?::[0-9a-f]{2}){5}', device_id) or int(device_id[:2], 16) & 1 or device_id == '00:00:00:00:00:00':
        raise ValueError('the speaker actual unicast Wi-Fi MAC is required')
    if upstream_token and (len(upstream_token) > 500 or any(ord(x) < 33 or ord(x) > 126 for x in upstream_token)):
        raise ValueError('upstream token must be a printable, single-line header value')
    ca = None
    if ca_file is not None:
        ca = Path(ca_file).read_bytes()
        if not 0 < len(ca) <= 512 * 1024 or b'-----BEGIN CERTIFICATE-----' not in ca or b'PRIVATE KEY' in ca:
            raise ValueError('CA file must contain public certificates only, at most 512 KiB')
    if parsed.scheme == 'wss' and ca is None:
        raise ValueError('wss requires --ca-file with a public CA bundle; certificate checks are never disabled')
    files = {'wpa.conf': make_wpa(ssid, wifi_password), 'wifi-mac': (device_id + '\n').encode(),
             'token': (secrets.token_hex(32) + '\n').encode()}
    server = {'listen': '127.0.0.1:18080', 'upstream_url': server_url, 'device_id': device_id,
              'local_token_file': '/s12/private/token',
              'upstream_token_file': '/s12/private/upstream-token' if upstream_token else '',
              'ca_file': '/s12/private/ca.pem' if ca is not None else ''}
    client = {'url': 'ws://127.0.0.1:18080/xiaozhi/v1/', 'deviceid': device_id,
              'clientid': str(uuid.uuid4()), 'token-file': '/s12/private/token'}
    files['server.json'] = (json.dumps(server, indent=2) + '\n').encode()
    files['client.json'] = (json.dumps(client, indent=2) + '\n').encode()
    if upstream_token:
        files['upstream-token'] = (upstream_token + '\n').encode()
    if ca is not None:
        files['ca.pem'] = ca
    output = Path(output)
    output.mkdir(mode=0o700, parents=False)
    for name, data in files.items():
        write_private(output / name, data)
    return {'private_files': len(files), 'configuration_created': True, 'device_access': False,
            'secrets_printed': False, 'firmware_image_is_private': True}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output-dir', type=Path, required=True, help='new private directory, preferably outside this repository')
    p.add_argument('--server-url', required=True, help='your chosen Xiaozhi WebSocket endpoint')
    p.add_argument('--device-id', required=True, help='actual speaker Wi-Fi MAC, used as Device-Id')
    p.add_argument('--ca-file', type=Path)
    a = p.parse_args()
    try:
        ssid = input('Wi-Fi SSID: ')
        password = getpass.getpass('Wi-Fi WPA2 password (hidden): ')
        token = getpass.getpass('Xiaozhi server token (hidden; Enter if not required): ')
        print(json.dumps(prepare(a.output_dir, a.server_url, a.device_id, ssid, password, token, a.ca_file)))
    except (ValueError, OSError) as exc:
        p.exit(1, f'configuration refused: {exc}\n')


if __name__ == '__main__':
    main()
