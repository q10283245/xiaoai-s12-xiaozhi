#!/usr/bin/env python3
"""Compile and exercise deterministic six-channel PCM with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE=Path(__file__).resolve().parent

class MicFrontendTest(unittest.TestCase):
    def test_synthetic_channel_selection_lock_health_and_antialias(self):
        with tempfile.TemporaryDirectory(prefix="s12-mic-synthetic-") as td:
            binary=Path(td)/"mic-frontend"
            subprocess.run(["clang","-std=c11","-O2","-Wall","-Wextra","-Werror",
                            "-fsanitize=address,undefined",str(HERE/"test_mic_frontend.c"),
                            "-o",str(binary)],check=True)
            result=subprocess.run([str(binary)],capture_output=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            self.assertIn(b'"synthetic_six_mic_frontend":"passed"',result.stdout)

if __name__=="__main__":unittest.main()
