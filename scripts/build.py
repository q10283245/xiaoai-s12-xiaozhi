#!/usr/bin/env python3
"""Cross-compile local sources only. No downloads, device access or flashing."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from elf_abi import validate, validate_bridge

ROOT = Path(__file__).resolve().parents[1]
DEVICE = ROOT / 'device'
LIBRARIES = ('usr/lib/libasound.so.2.0.0', 'usr/lib/libopus.so.0.6.1',
             'usr/lib/libjson-c.so.2.0.2', 'lib/libc-2.25.so',
             'lib/libpthread-2.25.so', 'lib/libm-2.25.so', 'lib/libdl-2.25.so')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(zig, sysroot, output, bridge=None, go='go'):
    zig = str(Path(zig).resolve()) if Path(zig).exists() else shutil.which(zig)
    if not zig:
        raise ValueError('Zig executable not found; set --zig or S12_ZIG')
    if subprocess.check_output([zig, 'version'], text=True).strip() != '0.13.0':
        raise ValueError('this build currently requires Zig 0.13.0')
    sysroot = Path(sysroot).resolve(strict=True)
    libraries = [sysroot / name for name in LIBRARIES]
    if any(not x.is_file() for x in libraries):
        raise ValueError('user sysroot is missing S12A 1.76.54 libraries listed in docs/BUILD.md')
    output = Path(output).absolute()
    if output.is_symlink():
        raise ValueError('output directory cannot be a symlink')
    output.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(output, 0o700)
    base = [zig, 'cc', '-target', 'arm-linux-gnueabihf.2.25', '-mcpu=cortex_a7', '-fPIC', '-pie',
            '-Os', '-s', '-std=c11', '-Wall', '-Wextra', '-Werror']
    report = {'schema': 1, 'toolchain': 'Zig 0.13.0', 'device_access': False,
              'flash_written': False, 'vendor_libraries_distributed': False,
              'vendor_input_sha256': {name: digest(sysroot / name) for name in LIBRARIES}, 'artifacts': {}}
    with tempfile.TemporaryDirectory(prefix='.s12-build-', dir=output) as td:
        tmp = Path(td)
        fvad = DEVICE / 'third_party/libfvad'
        provenance = json.loads((fvad / 'upstream.json').read_text())
        for name, meta in provenance['files'].items():
            if digest(fvad / name) != meta['sha256']:
                raise ValueError('libfvad source/license differs from pinned upstream: ' + name)
        client = tmp / 'client.arm'
        cmd = base + ['-pthread', str(DEVICE / 'xiaozhi_client.c'), str(DEVICE / 'vendor_wake.c'),
                      '-L' + str(sysroot / 'usr/lib'), '-l:libasound.so.2.0.0', '-l:libopus.so.0.6.1',
                      '-l:libjson-c.so.2.0.2', '-Wl,--allow-shlib-undefined', '-Wl,-z,nocopyreloc',
                      '-ldl', '-lm', '-lc', '-o', str(client)]
        subprocess.run(cmd, check=True, cwd=ROOT)
        client_abi = validate(client, libraries, ('libasound.so.2', 'libopus.so.0', 'libjson-c.so.2',
                                                'libpthread.so.0', 'libm.so.6', 'libdl.so.2', 'libc.so.6'))
        panel = tmp / 'panel.arm'
        subprocess.run(base + [str(DEVICE / 'panel_io.c'), '-Wl,-z,nocopyreloc', '-lc', '-o', str(panel)], check=True, cwd=ROOT)
        panel_abi = validate(panel, libraries, ('libpthread.so.0', 'libc.so.6'))
        bridge_output = tmp / 'bridge.arm'
        if bridge is not None:
            bridge_output.write_bytes(Path(bridge).read_bytes())
        else:
            env = os.environ.copy()
            env.update({'CGO_ENABLED': '0', 'GOOS': 'linux', 'GOARCH': 'arm', 'GOARM': '7',
                        'GOTOOLCHAIN': 'local', 'GOPROXY': 'off'})
            subprocess.run([go, 'build', '-trimpath', '-ldflags=-s -w -buildid=', '-o', str(bridge_output), '.'],
                           check=True, cwd=ROOT / 'bridge', env=env)
        bridge_abi = validate_bridge(bridge_output)
        for file, abi in ((client, client_abi), (panel, panel_abi), (bridge_output, bridge_abi)):
            report['artifacts'][file.name] = {'bytes': file.stat().st_size, 'sha256': digest(file), 'elf': abi}
            destination = output / file.name
            if destination.is_symlink():
                raise ValueError('existing artifact cannot be a symlink')
            file.chmod(0o600)
            os.replace(file, destination)
    report_path = output / 'build.json'
    if report_path.is_symlink():
        raise ValueError('build report cannot be a symlink')
    report_path.write_text(json.dumps(report, indent=2) + '\n')
    report_path.chmod(0o600)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--zig', default=os.environ.get('S12_ZIG', 'zig'))
    p.add_argument('--sysroot', type=Path, default=os.environ.get('S12_SYSROOT'), required=not bool(os.environ.get('S12_SYSROOT')))
    p.add_argument('--output-dir', type=Path, default=ROOT / 'build')
    p.add_argument('--bridge', type=Path, help='use an already-built static bridge instead of invoking Go')
    p.add_argument('--go', default=os.environ.get('GO', 'go'))
    a = p.parse_args()
    try:
        result = build(a.zig, a.sysroot, a.output_dir, a.bridge, a.go)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        p.exit(1, f'build refused: {exc}\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
