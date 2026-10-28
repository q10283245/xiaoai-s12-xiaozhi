#!/usr/bin/env python3
"""Read-only prepublication check. Reports paths, lines and rule IDs, never data.

The default scans the source tree and all Git-tracked files. Local output and
credential directories are skipped only when untracked. This is a reproducible
release guard, not a claim that every possible secret or license issue is found.
"""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
LOCAL_DIRS = {".git", "build", "local", "__pycache__", ".pytest_cache", ".venv"}
TOP_FILES = {"README.md", "README_EN.md", "LICENSE", "THIRD_PARTY_NOTICES.md", ".gitignore",
             ".gitattributes", ".editorconfig", "Makefile", "requirements.txt"}
SOURCE_SUFFIXES = {".c", ".h", ".go", ".py", ".sh", ".js", ".mjs"}
TEXT_SUFFIXES = SOURCE_SUFFIXES | {".md", ".txt", ".json", ".toml", ".yml", ".yaml", ".html", ".conf", ".in", ".mod", ".sum"}
DEPENDENCY_NOTICES = {"LICENSE", "AUTHORS", "PATENTS", "COPYING", "NOTICE"}
PROMPT_FILES = {"followup_ready_pcm.h", "ui_feedback_pcm.h", "farewell_prompt_pcm.h"}
FORBIDDEN_PARTS = {".private", "sysroot", "firmware-analysis", "dist", "node_modules", "vendor-binaries", "recordings"}
FORBIDDEN_SUFFIXES = {".arm", ".bin", ".img", ".elf", ".o", ".a", ".so", ".wav", ".opus", ".pcm", ".mp3", ".ogg", ".mp4", ".heic", ".jpg", ".jpeg", ".png", ".zip", ".gz", ".xz", ".7z", ".pem", ".key", ".env", ".s12app"}
MAGICS = (b"\x7fELF", b"MZ", b"ANDROID!", b"hsqs", b"sqsh", b"\x1f\x8b", b"PK\x03\x04",
          b"7z\xbc\xaf\x27\x1c", b"\xfd7zXZ\x00", b"RIFF", b"OggS", b"fLaC", b"\x89PNG", b"\xff\xd8\xff")
RULES = {
    "phone_number_shape": re.compile(r"(?<!\d)1[3-9]\d{9}(?!\d)"),
    "personal_absolute_path": re.compile(r"/(?:Users|home|root)/[^\s\"']+|[A-Z]:\\Users\\", re.I),
    "private_workspace_reference": re.compile(r'''[\\/]\.[p]rivate(?:[\\/]|\b)|["']\.[p]rivate[\\/]'''),
    "api_key_shape": re.compile(r"\b(?:sk-[A-Za-z0-9_-]{16,}|rh_[0-9a-f]{30,})\b"),
    "private_key_material": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
}
SECRET_ASSIGNMENT = re.compile(
    r'''(?i)["']?\b(?:api[_-]?key|access[_-]?token|password|secret|signing[_-]?seed|token)\b["']?\s*[:=]\s*["']([^"'\r\n]{12,})["']''')
PLACEHOLDER = re.compile(r"^(?:YOUR_|EXAMPLE_|REPLACE_|<[^>]+>|example\b|placeholder\b|test[_-]|dummy[_-])", re.I)
MAC_LITERAL = re.compile(r"(?<![0-9a-f])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f])", re.I)
EXAMPLE_MACS = {"02:00:00:00:00:01", "00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff"}


def allowed(path: Path) -> bool:
    parts = path.parts
    if len(parts) == 1:
        return path.name in TOP_FILES
    if parts[0] == ".github":
        return len(parts) == 3 and parts[1] == "workflows" and path.suffix in {".yml", ".yaml"}
    if parts[0] == "device":
        return path.suffix in SOURCE_SUFFIXES or path.name in DEPENDENCY_NOTICES or path.name in {"upstream.json", "minimal_alsa.conf"}
    if parts[0] == "bridge":
        return path.suffix in TEXT_SUFFIXES or path.name in {"go.mod", "go.sum", "LICENSE"}
    if parts[0] == "scripts":
        return path.suffix in SOURCE_SUFFIXES
    if parts[0] == "runtime":
        return path.suffix in {".sh", ".conf"} or path.name == "init"
    if parts[0] == "tests":
        return path.suffix in TEXT_SUFFIXES
    if parts[0] == "docs":
        return path.suffix in {".md", ".txt"}
    if parts[0] == "config":
        return (".example." in path.name and path.suffix in {".json", ".conf", ".toml", ".yml", ".yaml"}) or path.name in {"wpa.conf.example", "README.md"}
    return False


def tracked_files(root: Path) -> tuple[set[str], list[dict]]:
    if not (root / ".git").exists():
        return set(), []
    try:
        result = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
        names = result.stdout.decode("utf-8").split("\0")
    except (OSError, subprocess.CalledProcessError, UnicodeError):
        return set(), [{"path": ".git", "line": 0, "rule": "git_index_unreadable"}]
    return {name for name in names if name}, []


def scan(root: Path) -> dict:
    root = root.resolve()
    tracked, issues = tracked_files(root)
    entries: set[str] = set(tracked)
    skipped = 0
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        parent = Path(directory)
        for name in list(dirnames):
            path = parent / name
            relative = path.relative_to(root)
            if path.is_symlink():
                entries.add(relative.as_posix())
                dirnames.remove(name)
            elif name in LOCAL_DIRS:
                skipped += 1
                dirnames.remove(name)
        for name in filenames:
            entries.add((parent / name).relative_to(root).as_posix())
    scanned = 0
    for name in sorted(entries):
        relative = Path(name)
        if relative.is_absolute() or ".." in relative.parts:
            issues.append({"path": "<invalid-index-path>", "line": 0, "rule": "path_escape"})
            continue
        path = root / relative
        if any(part in LOCAL_DIRS for part in relative.parts):
            if name in tracked:
                issues.append({"path": name, "line": 0, "rule": "tracked_local_output_or_credentials"})
            continue
        try:
            metadata = path.lstat()
        except OSError:
            issues.append({"path": name, "line": 0, "rule": "indexed_file_missing_or_unreadable"})
            continue
        if stat.S_ISLNK(metadata.st_mode):
            issues.append({"path": name, "line": 0, "rule": "symlink_not_publishable"})
            continue
        if not stat.S_ISREG(metadata.st_mode):
            issues.append({"path": name, "line": 0, "rule": "nonregular_file"})
            continue
        if metadata.st_size > 2 * 1024 * 1024:
            issues.append({"path": name, "line": 0, "rule": "oversize_source_or_blob"})
            continue
        if any(part in FORBIDDEN_PARTS for part in relative.parts) or any(part.startswith("build-") for part in relative.parts):
            issues.append({"path": name, "line": 0, "rule": "forbidden_directory"})
        if path.suffix.lower() in FORBIDDEN_SUFFIXES or re.search(r"\.so(?:\.[0-9]+)*$", path.name):
            issues.append({"path": name, "line": 0, "rule": "forbidden_binary_or_private_extension"})
        if not allowed(relative):
            issues.append({"path": name, "line": 0, "rule": "outside_source_allowlist"})
        try:
            data = path.read_bytes()
        except OSError:
            issues.append({"path": name, "line": 0, "rule": "source_unreadable"})
            continue
        scanned += 1
        if any(data.startswith(magic) for magic in MAGICS) or b"\0" in data:
            issues.append({"path": name, "line": 0, "rule": "binary_magic_or_nul"})
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeError:
            issues.append({"path": name, "line": 0, "rule": "non_utf8_source"})
            continue
        for number, line in enumerate(text.splitlines(), 1):
            for rule, pattern in RULES.items():
                if pattern.search(line):
                    if rule == "private_workspace_reference" and name == ".gitignore" and line.strip().removeprefix("**/").removeprefix("/") == "." + "private/":
                        continue
                    issues.append({"path": name, "line": number, "rule": rule})
            for match in SECRET_ASSIGNMENT.finditer(line):
                if not PLACEHOLDER.match(match.group(1)):
                    issues.append({"path": name, "line": number, "rule": "credential_assignment_shape"})
            if any(match.group().lower() not in EXAMPLE_MACS and not match.group().lower().startswith("02:00:00:00:00:") for match in MAC_LITERAL.finditer(line)):
                issues.append({"path": name, "line": number, "rule": "nonexample_device_mac"})
        if path.suffix == ".h" and path.name not in PROMPT_FILES and len(re.findall(r"0x[0-9a-fA-F]{2}\s*,", text)) > 100:
            issues.append({"path": name, "line": 0, "rule": "unreviewed_embedded_byte_array"})
    # The source recipe must recreate every published cue byte exactly; a
    # provenance comment alone cannot legitimize copied factory recordings.
    try:
        generator = root / "scripts/generate_prompts.py"
        if generator.is_symlink():
            raise ValueError("symlink")
        # A candidate --root is data to inspect, not Python code to execute.
        # Use this checkout's trusted recipe even when scanning another tree.
        specification = importlib.util.spec_from_file_location("public_prompt_recipe", ROOT / "scripts/generate_prompts.py")
        if specification is None or specification.loader is None:
            raise ValueError("loader")
        module = importlib.util.module_from_spec(specification)
        previous_bytecode = sys.dont_write_bytecode
        try:
            sys.dont_write_bytecode = True
            specification.loader.exec_module(module)
        finally:
            sys.dont_write_bytecode = previous_bytecode
        expected = module.outputs()
        if set(expected) != PROMPT_FILES:
            raise ValueError("prompt set")
        for name, data in expected.items():
            target = root / "device" / name
            if target.is_symlink() or not target.is_file() or target.read_bytes() != data:
                issues.append({"path": "device/" + name, "line": 0, "rule": "synthetic_prompt_provenance_mismatch"})
    except Exception:
        issues.append({"path": "scripts/generate_prompts.py", "line": 0, "rule": "synthetic_prompt_recipe_unverifiable"})
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "device/third_party/libfvad/LICENSE",
                 "device/third_party/libfvad/AUTHORS", "device/third_party/libfvad/PATENTS", "device/third_party/libfvad/upstream.json"):
        if not (root / name).is_file():
            issues.append({"path": name, "line": 0, "rule": "license_or_provenance_missing"})
    try:
        directory = root / "device/third_party/libfvad"
        provenance = json.loads((directory / "upstream.json").read_text())
        if provenance["commit"] != "532ab666c20d3cfda38bca63abbb0f152706c369" or not all(name in provenance["files"] for name in ("LICENSE", "AUTHORS", "PATENTS")):
            raise ValueError("revision or notices")
        for name, expected in provenance["files"].items():
            member = Path(name)
            if member.is_absolute() or ".." in member.parts or any((directory / Path(*member.parts[:index])).is_symlink() for index in range(1, len(member.parts) + 1)):
                raise ValueError("dependency member path")
            data = (directory / member).read_bytes()
            if len(data) != expected["bytes"] or hashlib.sha256(data).hexdigest() != expected["sha256"]:
                issues.append({"path": "device/third_party/libfvad/" + name, "line": 0, "rule": "dependency_source_or_license_hash_mismatch"})
    except (OSError, ValueError, KeyError, TypeError):
        issues.append({"path": "device/third_party/libfvad/upstream.json", "line": 0, "rule": "dependency_provenance_unverifiable"})
    issues = sorted(issues, key=lambda item: (item["path"], item["line"], item["rule"]))
    return {"ok": not issues, "scanned_files": scanned, "tracked_files": len(tracked),
            "skipped_untracked_local_directories": skipped, "issues": issues,
            "report_contains_secret_values": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    result = scan(args.root)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
