"""Offline publication guard regression tests; no real credentials or network."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load(name: str, filename: str):
    specification = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


guard = load("release_guard", "check_public_tree.py")
prompts = load("prompt_recipe", "generate_prompts.py")


class ReleaseGuardTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copyfile(ROOT / name, self.root / name)
        (self.root / "device").mkdir()
        (self.root / "scripts").mkdir()
        shutil.copyfile(ROOT / "scripts/generate_prompts.py", self.root / "scripts/generate_prompts.py")
        shutil.copytree(ROOT / "device/third_party/libfvad", self.root / "device/third_party/libfvad")
        for name, data in prompts.outputs().items():
            (self.root / "device" / name).write_bytes(data)

    def put(self, name, data):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data if isinstance(data, bytes) else data.encode())
        return target

    def rules(self):
        return {issue["rule"] for issue in guard.scan(self.root)["issues"]}

    def test_clean_source_and_examples(self):
        self.put("config/client.example.json", json.dumps({"token": "YOUR_TOKEN_HERE", "device": "02:00:00:00:00:01", "url": "ws://127.0.0.1:18081/ws"}))
        self.assertTrue(guard.scan(self.root)["ok"])

    def test_key_shapes_are_redacted(self):
        value = "sk-" + "z" * 32
        self.put("config/client.example.json", json.dumps({"token": value}))
        result = guard.scan(self.root)
        self.assertIn("api_key_shape", {issue["rule"] for issue in result["issues"]})
        self.assertNotIn(value, json.dumps(result))

    def test_generic_phone_mac_and_home_paths(self):
        # Algorithmic shape fixtures, unrelated to any person's identifiers.
        phone = "139" + "0" * 8
        mac = ":".join(f"{value:02x}" for value in range(6))
        line = " ".join([phone, mac, "/" + "Users/fictional-person/project"])
        self.put("docs/fixture.md", line)
        self.assertTrue({"phone_number_shape", "personal_absolute_path", "nonexample_device_mac"} <= self.rules())

    def test_relative_private_fixture_reference_is_rejected(self):
        self.put("tests/fixture.py", "path = '" + "." + "private/old/input.pcm'\n")
        self.assertIn("private_workspace_reference", self.rules())

    def test_binary_disguised_as_source(self):
        self.put("device/hidden.c", b"\x7fELF" + b"binary")
        self.assertIn("binary_magic_or_nul", self.rules())

    def test_unapproved_extension_and_byte_array(self):
        self.put("device/recording.wav", "not really an audio file")
        self.put("device/unreviewed.h", "unsigned char data[]={" + "0x00," * 101 + "};")
        self.assertTrue({"forbidden_binary_or_private_extension", "outside_source_allowlist", "unreviewed_embedded_byte_array"} <= self.rules())

    def test_symlink_rejected(self):
        (self.root / "device/link.c").symlink_to(self.root / "LICENSE")
        self.assertIn("symlink_not_publishable", self.rules())

    def test_original_pcm_recipe_is_required(self):
        target = self.root / "device/followup_ready_pcm.h"
        target.write_bytes(target.read_bytes() + b"/* unexpected recording */\n")
        self.assertIn("synthetic_prompt_provenance_mismatch", self.rules())

    def test_changed_dependency_license_rejected(self):
        self.put("device/third_party/libfvad/LICENSE", "unexpected license replacement")
        self.assertIn("dependency_source_or_license_hash_mismatch", self.rules())

    def test_untracked_local_outputs_are_skipped(self):
        self.put("build/output.img", b"ANDROID!" + b"\0" * 64)
        self.put("local/token", "local-only-placeholder")
        self.assertTrue(guard.scan(self.root)["ok"])

    def test_tracked_local_outputs_are_rejected(self):
        self.put("local/token", "local-only-placeholder")
        subprocess.run(["git", "init", "-q", str(self.root)], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["git", "-C", str(self.root), "add", "local/token"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.assertIn("tracked_local_output_or_credentials", self.rules())


class PromptTests(unittest.TestCase):
    def test_deterministic_quiet_and_bounded(self):
        self.assertEqual(prompts.outputs(), prompts.outputs())
        for recipe in prompts.RECIPES.values():
            data = prompts.pcm(recipe)
            samples = struct.unpack("<" + "h" * (len(data) // 2), data)
            self.assertLessEqual(max(abs(value) for value in samples), 2400)
            self.assertEqual(samples[0], 0)
            self.assertEqual(samples[-1], 0)
            self.assertEqual(len(samples), sum(ms for _, ms in recipe) * 16)

    def test_published_pcm_matches_recipe(self):
        for name, data in prompts.outputs().items():
            self.assertEqual((ROOT / "device" / name).read_bytes(), data)
            self.assertIn(b"no vendor audio or TTS", data)
            self.assertNotIn(b"Source: stock", data)


if __name__ == "__main__":
    unittest.main()
