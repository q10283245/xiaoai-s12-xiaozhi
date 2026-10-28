#!/usr/bin/env python3
"""Native ASan/UBSan panel IPC tests using private fake sysfs and evdev FIFO."""
import os
from pathlib import Path
import signal
import time
import unittest
from test_panel_io import PanelTest


def nonce(number):
    return f"{number:032x}"


def request(number, volume, deadline=None):
    if deadline is None:
        deadline = int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000) + 1500
    return f"v1 {nonce(number)} {volume} {deadline}\n"


class VolumeTest(unittest.TestCase):
    setUpClass = classmethod(PanelTest.setUpClass.__func__)
    tearDownClass = classmethod(PanelTest.tearDownClass.__func__)
    fixture = PanelTest.fixture

    def submit(self, fixture, number, volume, deadline=None):
        temporary = fixture.runtime / f".request-{nonce(number)}.next"
        temporary.write_text(request(number, volume, deadline))
        temporary.chmod(0o600)
        temporary.replace(fixture.runtime / "panel-volume.request")

    def ack(self, fixture, number):
        path = fixture.runtime / "panel-volume.ack"
        def matches():
            try:
                return path.read_text().split()[1] == nonce(number)
            except (FileNotFoundError, IndexError):
                return False
        fixture.wait(matches)
        self.assertEqual(path.stat().st_mode & 0o7777, 0o600)
        self.assertEqual(path.stat().st_nlink, 1)
        fixture.wait(lambda: not (fixture.runtime / ".panel-volume.claim").exists()
                     and not (fixture.runtime / "panel-volume.request").exists())
        return path.read_text().split()

    def test_exact_mapping_and_privacy_counters_preserved(self):
        f = self.fixture(control="v1 7 50 1 4 3\n").ready()
        for ident, volume, gain, seq in ((1, 50, 100, 8), (2, 0, 0, 9), (3, 100, 200, 10)):
            self.submit(f, ident, volume)
            self.assertEqual(self.ack(f, ident), ["v1", nonce(ident), "ok", str(volume), str(gain), str(seq)])
            self.assertEqual(f.fields(), (seq, gain, 1, 4, 3))
        self.assertEqual(f.control.stat().st_mode & 0o7777, 0o600)

    def test_half_volume_six_bars_and_zero_overlay_does_not_mute_microphone(self):
        f = self.fixture(rgb=True, state="capturing").ready()
        self.submit(f, 1, 50)
        self.ack(f, 1)
        f.wait(lambda: [0x202020] * 6 + [0] * 6 in f.frames())
        self.submit(f, 2, 0)
        self.ack(f, 2)
        f.wait(lambda: f.frames()[-1] == [0] * 12)
        self.assertEqual(f.fields()[1:], (0, 0, 0, 0))
        f.wait(lambda: f.frames()[-1] == [0x200000] * 12)

    def test_zero_control_load_and_touch_share_zero_to_two_hundred_range(self):
        f = self.fixture(control="v1 4 0 0 2 3\n").ready()
        f.click(108)
        time.sleep(.1)
        self.assertEqual(f.fields(), (4, 0, 0, 2, 3))
        f.click(103)
        f.wait(lambda: f.fields() == (5, 10, 0, 2, 3))
        self.submit(f, 1, 1)
        self.ack(f, 1)
        self.assertEqual(f.fields(), (6, 2, 0, 2, 3))
        f.click(108)
        f.wait(lambda: f.fields() == (7, 0, 0, 2, 3))

    def test_same_nonce_never_reapplies_after_manual_change_or_ack_failure(self):
        f = self.fixture().ready()
        blocked = f.runtime / f".panel-volume.ack.{f.process.pid}.next"
        blocked.write_text("occupied")
        blocked.chmod(0o600)
        self.submit(f, 1, 50)
        f.wait(lambda: f.fields() == (1, 100, 0, 0, 0))
        self.assertFalse((f.runtime / "panel-volume.ack").exists())
        f.click(103)
        f.wait(lambda: f.fields() == (2, 110, 0, 0, 0))
        blocked.unlink()
        self.assertEqual(self.ack(f, 1), ["v1", nonce(1), "ok", "50", "100", "1"])
        self.assertEqual(f.fields(), (2, 110, 0, 0, 0))
        self.submit(f, 1, 50)
        self.ack(f, 1)
        self.assertEqual(f.fields(), (2, 110, 0, 0, 0))

    def test_nonce_conflict_rejects_new_value(self):
        f = self.fixture().ready()
        self.submit(f, 1, 20)
        self.ack(f, 1)
        self.submit(f, 1, 80)
        f.wait(lambda: (f.runtime / "panel-volume.ack").read_text().endswith("failed nonce_conflict\n"))
        self.assertEqual(self.ack(f, 1), ["v1", nonce(1), "failed", "nonce_conflict"])
        self.assertEqual(f.fields(), (1, 40, 0, 0, 0))

    def test_control_publish_failure_never_reports_success_or_shows_applied_gain(self):
        f = self.fixture(rgb=True).ready()
        blocked = f.runtime / f".panel.control.{f.process.pid}.next"
        blocked.write_text("occupied")
        self.submit(f, 1, 80)
        self.assertEqual(self.ack(f, 1), ["v1", nonce(1), "failed", "publish_failed"])
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        self.assertFalse(any(0x202020 in frame for frame in f.frames()))
        blocked.unlink()
        self.submit(f, 2, 80)
        self.assertEqual(self.ack(f, 2)[2:], ["ok", "80", "160", "1"])

    def test_expired_and_unreasonably_future_deadline_are_rejected(self):
        f = self.fixture().ready()
        for ident, delta, reason in ((1, -1, "expired"), (2, 4000, "invalid_deadline")):
            self.submit(f, ident, 80, int(time.clock_gettime(time.CLOCK_MONOTONIC) * 1000) + delta)
            self.assertEqual(self.ack(f, ident), ["v1", nonce(ident), "failed", reason])
            self.assertEqual(f.fields(), (0, 50, 0, 0, 0))

    def test_stopped_daemon_cannot_apply_caller_timed_out_request_later(self):
        f = self.fixture().ready()
        f.process.send_signal(signal.SIGSTOP)
        try:
            self.submit(f, 1, 90)
            time.sleep(1.65)
        finally:
            f.process.send_signal(signal.SIGCONT)
        self.assertEqual(self.ack(f, 1), ["v1", nonce(1), "failed", "expired"])
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))

    def test_startup_request_and_claim_are_rejected_without_replaying_gain(self):
        for name in ("volume_request", "volume_claim"):
            with self.subTest(name=name):
                f = self.fixture(control="v1 9 80 1 3 2\n", **{name: request(1, 50)}).ready()
                self.assertEqual(self.ack(f, 1), ["v1", nonce(1), "failed", "daemon_restart"])
                self.assertEqual(f.fields(), (9, 80, 1, 3, 2))
                self.assertFalse((f.runtime / "panel-volume.request").exists())
                self.submit(f, 2, 25)
                self.assertEqual(self.ack(f, 2)[2:], ["ok", "25", "50", "10"])

    def test_ack_consumer_can_immediately_publish_next_request_without_loss(self):
        f = self.fixture().ready()
        for ident in range(1, 25):
            self.submit(f, ident, ident)
            # Deliberately observe ACK only: publish next request without
            # waiting for the daemon's subsequent claim cleanup.
            path = f.runtime / "panel-volume.ack"
            f.wait(lambda: path.exists() and path.read_text().split()[1] == nonce(ident))
        self.assertEqual(self.ack(f, 24)[2:], ["ok", "24", "48", "24"])
        self.assertEqual(f.fields(), (24, 48, 0, 0, 0))

    def test_remote_absolute_value_clears_old_ring_tail_then_new_motion_works(self):
        f = self.fixture().ready()
        f.ring_start(0)
        f.ring_move(20)
        f.wait(lambda: f.fields()[1] == 60)
        f.ring_move(40)
        self.submit(f, 1, 50)
        self.ack(f, 1)
        current = f.fields()
        self.assertEqual(current[1], 100)
        time.sleep(.2)
        self.assertEqual(f.fields(), current)
        f.ring_release()
        f.ring_start(50)
        f.ring_move(70)
        f.wait(lambda: f.fields()[1] == 110)

    def test_bad_metadata_or_payload_has_no_ack_or_state_change(self):
        cases = ("mode", "symlink", "hardlink", "fifo", "bad-nonce", "extra", "overflow")
        for case in cases:
            with self.subTest(case=case):
                f = self.fixture().ready()
                path = f.runtime / "panel-volume.request"
                if case == "symlink":
                    target = f.runtime / "outside"
                    target.write_text(request(1, 80)); target.chmod(0o600); path.symlink_to(target)
                elif case == "fifo":
                    os.mkfifo(path, 0o600)
                else:
                    content = request(1, 80)
                    if case == "bad-nonce": content = content.replace(nonce(1), "G" * 32)
                    if case == "extra": content += "extra\n"
                    if case == "overflow": content = f"v1 {nonce(1)} 80 18446744073709551616\n"
                    path.write_text(content); path.chmod(0o644 if case == "mode" else 0o600)
                    if case == "hardlink": os.link(path, f.runtime / "other-link")
                time.sleep(.15)
                self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
                self.assertFalse((f.runtime / "panel-volume.ack").exists())
                self.assertIsNone(f.process.poll())


if __name__ == "__main__":
    unittest.main()
