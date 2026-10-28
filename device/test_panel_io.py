#!/usr/bin/env python3
"""Native-only fake sysfs/FIFO tests; never opens actual input or LED devices."""
import os
from datetime import datetime, timedelta, timezone
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent


def beijing_time(hour, minute=0, second=0, day=24):
    return int(datetime(2026, 10, day, hour, minute, second,
                        tzinfo=timezone(timedelta(hours=8))).timestamp())


class Fixture:
    def __init__(self, executable, control=None, keymap=None, name="keys-cyttsp", rgb=False,
                 absinfo=(0, 0, 240, 0, 0, 0), direction=1, clock=0,
                 state="listening", environment_timezone=None, volume_request=None, volume_claim=None):
        self.temp = tempfile.TemporaryDirectory(prefix="s12-panel-test-")
        self.root = Path(self.temp.name)
        self.runtime = self.root / "run/s12"
        self.runtime.mkdir(parents=True, mode=0o700)
        self.control = self.runtime / "panel.control"
        if control is not None:
            self.control.write_text(control)
            self.control.chmod(0o600)
        for volume_name, text in (("panel-volume.request", volume_request), (".panel-volume.claim", volume_claim)):
            if text is not None:
                path = self.runtime / volume_name
                path.write_text(text)
                path.chmod(0o600)
        (self.runtime / "ui.state").write_text(state + "\n")
        self.clock = self.runtime / "test.clock"
        self.set_clock(clock)
        sys = self.root / "sys/class/input/event7"
        (sys / "device").mkdir(parents=True)
        (sys / "device/name").write_text(name + "\n")
        if absinfo is not None:
            (sys / "device/abs_x").write_text(" ".join(map(str, absinfo)) + "\n")
        (sys / "dev").write_text("13:71\n")
        dev = self.root / "dev/input/event7"
        dev.parent.mkdir(parents=True)
        os.mkfifo(dev, 0o600)
        self.writer = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
        self.led = self.root / "sys/devices/i2c-1/1-003c/led_rgb"
        self.led.parent.mkdir(parents=True)
        self.led.write_text("")
        args = [str(executable), "--test-root", str(self.root), "--run-for-ms", "10000"]
        if keymap:
            args += ["--keymap", keymap]
        if rgb:
            args += ["--rgb-confirmed"]
        args += ["--ring-direction", str(direction)]
        env = os.environ.copy()
        if environment_timezone is not None:
            env["TZ"] = environment_timezone
        self.process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.result = None

    def set_clock(self, seconds):
        temporary = self.runtime / ".test.clock.next"
        temporary.write_text(str(seconds) + "\n")
        temporary.chmod(0o600)
        temporary.replace(self.clock)

    def event(self, code, value, type_=1):
        self.bytes(struct.pack("<iiHHi", 0, 0, type_, code, value))

    def bytes(self, value):
        os.write(self.writer, value)

    def click(self, code):
        self.event(code, 1)
        self.event(code, 0)

    def ring_start(self, position):
        self.event(330, 1)
        self.event(0, position, type_=3)

    def ring_move(self, position):
        self.event(0, position, type_=3)

    def ring_release(self):
        self.event(330, 0)

    def wait(self, predicate, timeout=2):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            if self.process.poll() is not None:
                raise AssertionError("panel exited: " + self.process.communicate()[1].decode())
            time.sleep(.01)
        raise AssertionError("fake panel condition timeout")

    def fields(self):
        try:
            words = self.control.read_text().split()
            return tuple(map(int, words[1:])) if words[0] == "v1" and len(words) == 6 else None
        except (FileNotFoundError, ValueError, IndexError):
            return None

    def frames(self):
        rows = [line.split() for line in self.led.read_text().splitlines()]
        result = []
        for start in range(0, len(rows) - 11, 12):
            frame = rows[start:start + 12]
            if [int(row[0]) for row in frame] == list(range(12)):
                result.append([int(row[1], 16) for row in frame])
        return result

    def ready(self):
        self.wait(lambda: self.fields() is not None)
        # Initialization publishes before discovery; wait for the event FIFO to
        # be selected without depending on scheduler timing in the tests.
        time.sleep(.08)
        return self

    def stop(self):
        if self.result is None:
            if self.process.poll() is None:
                self.process.send_signal(signal.SIGTERM)
            self.result = self.process.communicate(timeout=2)
        return self.process.returncode

    def close(self):
        self.stop()
        os.close(self.writer)
        self.temp.cleanup()


class PanelTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="s12-panel-build-")
        cls.executable = Path(cls.build.name) / "panel"
        subprocess.run([shutil.which("cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                        "-fsanitize=address,undefined", "-DS12_PANEL_TEST", str(HERE / "panel_io.c"),
                        "-o", str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def fixture(self, **kwargs):
        f = Fixture(self.executable, **kwargs)
        self.addCleanup(f.close)
        return f

    def test_selftest_and_keymap_rejection(self):
        run = subprocess.run([str(self.executable), "--selftest"], capture_output=True, check=True)
        self.assertEqual(run.stdout, b"S12_PANEL_SELFTEST_OK\n")
        for keymap in ("103,103,28,105,106", "103,108,28,105,-1", "103,108,28,105,768", "103,108,28,105,106x", "103,108,330,105,106"):
            run = subprocess.run([str(self.executable), "--keymap", keymap], capture_output=True)
            self.assertEqual(run.returncode, 2)
        for direction in ("0", "2", "+1", "reverse"):
            run = subprocess.run([str(self.executable), "--ring-direction", direction], capture_output=True)
            self.assertEqual(run.returncode, 2)

    def test_all_five_keys_and_atomic_private_control(self):
        f = self.fixture().ready()
        for key in (103, 108, 28, 105, 106):
            f.click(key)
        f.wait(lambda: f.fields() == (5, 50, 1, 3, 1))
        self.assertEqual(f.control.stat().st_mode & 0o777, 0o600)
        self.assertFalse(list(f.runtime.glob(".*.next")))

    def test_arm_event_partial_and_unknown_repeat_ignored(self):
        f = self.fixture().ready()
        event = struct.pack("<iiHHi", 0, 0, 1, 103, 1)
        f.bytes(event[:7])
        time.sleep(.06)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        f.bytes(event[7:])
        f.event(103, 2)
        f.event(103, 1)
        f.event(999, 1)
        f.event(103, -1)
        f.event(103, 0)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))

    def test_keymap_remaps_linux_codes(self):
        f = self.fixture(keymap="12,13,14,15,16").ready()
        f.click(103)
        f.click(12)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))

    def test_gain_upper_and_lower_clamp(self):
        f = self.fixture(control="v1 10 195 0 2 3\n").ready()
        f.click(103)
        f.wait(lambda: f.fields() == (11, 200, 0, 2, 3))
        f2 = self.fixture(control="v1 10 15 0 2 3\n").ready()
        f2.click(108)
        f2.wait(lambda: f2.fields() == (11, 5, 0, 2, 3))

    def test_debounce_and_dropped_sync(self):
        f = self.fixture().ready()
        f.click(103)
        f.click(103)
        f.event(28, 1)
        f.event(3, 0, type_=0)  # SYN_DROPPED: do not invent a manual press.
        f.event(28, 0)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))

    def test_long_press_does_not_invent_manual_or_mute(self):
        f = self.fixture().ready()
        f.event(28, 1)
        f.event(105, 1)
        time.sleep(1.6)
        f.event(28, 0)
        f.event(105, 0)
        time.sleep(.12)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))

    def test_existing_control_preserved_with_counters_saturating(self):
        f = self.fixture(control="v1 4294967295 50 1 4294967295 4294967295\n").ready()
        f.click(28)
        time.sleep(.1)
        self.assertEqual(f.fields(), (4294967295, 50, 1, 4294967295, 4294967295))

    def test_malformed_control_is_fail_closed(self):
        invalid = ("v1 0 50 2 0 0\n", "v1 0 201 0 0 0\n", "v1 +0 50 0 0 0\n",
                   "v1 4294967296 50 0 0 0\n", "v1 0 50 0 0 0\nextra", "v1 0 50 0 0 0\x00\n")
        for text in invalid:
            f = self.fixture(control=text)
            self.assertEqual(f.process.wait(timeout=2), 1)
            self.assertEqual(f.control.read_text(), text)

    def test_other_input_name_is_not_consumed(self):
        f = self.fixture(name="other-keys").ready()
        f.click(103)
        time.sleep(.15)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))

    def test_default_led_bounds_and_bounded_rate_and_stop_off(self):
        f = self.fixture().ready()
        time.sleep(2.15)
        self.assertEqual(f.stop(), 0)
        rows = [line.split() for line in f.led.read_text().splitlines()]
        self.assertLessEqual(len(rows), 48)  # 3 frames plus final all-off frame.
        self.assertGreaterEqual(len(rows), 24)
        for index, color in rows:
            self.assertIn(int(index), range(12))
            self.assertLessEqual(int(color, 16), 32)
        self.assertEqual(rows[-12:], [[str(i), "0x000000"] for i in range(12)])

    def test_rgb_is_explicit_visible_and_capped_at_32(self):
        f = self.fixture(rgb=True).ready()
        f.wait(lambda: len(f.led.read_text().splitlines()) >= 12)
        first = f.led.read_text().splitlines()[0].split()[1]
        self.assertEqual(int(first, 16), 0x080800)  # BBGGRR: cyan is blue+green.
        time.sleep(1.1)
        self.assertTrue(any(value == 0x0c0c00 for frame in f.frames() for value in frame))
        for line in f.led.read_text().splitlines():
            value = int(line.split()[1], 16)
            self.assertTrue(all((value >> offset) & 255 <= 32 for offset in (0, 8, 16)))

    def test_boot_blue_breathing_uses_existing_day_night_and_fallback_scale(self):
        for clock, first, second in [(beijing_time(12), 24, 36),
                                     (beijing_time(22), 4, 6), (0, 8, 12)]:
            with self.subTest(clock=clock):
                f = self.fixture(rgb=True, state="boot", clock=clock).ready()
                f.wait(lambda: len(f.frames()) >= 2)
                self.assertEqual(f.frames()[0], [first << 16] * 12)
                self.assertEqual(f.frames()[1], [second << 16] * 12)
                self.assertEqual(f.stop(), 0)
                self.assertEqual(f.frames()[-1], [0] * 12)

    def test_ready_is_static_cyan_until_client_changes_state(self):
        f = self.fixture(rgb=True, state="ready", clock=beijing_time(12)).ready()
        f.wait(lambda: bool(f.frames()))
        self.assertEqual(f.frames()[0], [0x606000] * 12)
        time.sleep(2.1)  # The client owns the2s hold; panel has no expiry timer.
        self.assertEqual(len(f.frames()), 1)
        self.assertEqual(f.frames()[-1], [0x606000] * 12)
        (f.runtime / "ui.state").write_text("idle\n")
        f.wait(lambda: f.frames()[-1] == [0] * 12)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))

    def test_ready_does_not_override_mute_or_new_error(self):
        f = self.fixture(rgb=True, state="ready").ready()
        f.wait(lambda: f.frames()[-1:] == [[0x202000] * 12])
        f.click(105)
        f.wait(lambda: f.frames()[-1] == [0x000820] * 12)
        (f.runtime / "ui.state").write_text("ready\n")
        time.sleep(1.1)
        self.assertEqual(f.frames()[-1], [0x000820] * 12)
        f.click(105)
        f.wait(lambda: f.frames()[-1] == [0x202000] * 12)
        (f.runtime / "ui.state").write_text("error\n")
        f.wait(lambda: f.frames()[-1] == [0x000010] * 12)
        self.assertEqual(f.fields(), (2, 50, 0, 2, 0))

    def test_ready_touch_volume_expires_to_latest_error_not_stale_ready(self):
        f = self.fixture(rgb=True, state="ready").ready()
        f.click(103)
        f.wait(lambda: f.frames()[-1:] == [[0x202020] * 4 + [0] * 8])
        (f.runtime / "ui.state").write_text("error\n")
        f.wait(lambda: f.frames()[-1] == [0x000010] * 12)
        self.assertEqual(f.fields(), (1, 60, 0, 0, 0))

    def test_startup_state_changes_still_obey_write_budget_and_term_off(self):
        f = self.fixture(rgb=True, state="boot", clock=beijing_time(12)).ready()
        until = time.monotonic() + 2.15
        states = ["boot\n", "ready\n", "idle\n", "error\n"]
        n = 0
        while time.monotonic() < until:
            (f.runtime / "ui.state").write_text(states[n % len(states)])
            n += 1
            time.sleep(.02)
        self.assertEqual(f.stop(), 0)
        self.assertLessEqual(len(f.frames()), 4)
        self.assertEqual(f.frames()[-1], [0] * 12)
        self.assertTrue(all(all((v >> bit) & 255 <= 96 for bit in (0, 8, 16))
                            for frame in f.frames() for v in frame))

    def test_beijing_schedule_boundaries_ignore_environment_timezone(self):
        cases = [(6, 59, 59, 16), (7, 0, 0, 96), (21, 59, 59, 96),
                 (22, 0, 0, 16), (23, 59, 59, 16), (0, 0, 0, 16)]
        for hour, minute, second, limit in cases:
            with self.subTest(hour=hour, minute=minute, second=second):
                f = self.fixture(rgb=True, state="capturing", clock=beijing_time(hour, minute, second),
                                 environment_timezone="America/New_York").ready()
                f.wait(lambda: bool(f.frames()))
                self.assertEqual(f.frames()[-1], [limit << 16] * 12)  # BBGGRR blue.
                self.assertEqual(f.stop(), 0)
                self.assertEqual(f.frames()[-1], [0] * 12)

    def test_invalid_clock_recovers_and_static_mute_tracks_clock_jumps(self):
        f = self.fixture(rgb=True, control="v1 7 50 1 2 0\n", clock=0).ready()
        f.wait(lambda: f.frames()[-1:] == [[0x000820] * 12])
        for seconds, rgb in [(beijing_time(12), 0x001860), (beijing_time(22), 0x000410),
                             (2147483647, 0x001860), (2147483648, 0x000820),
                             (-1, 0x000820), (beijing_time(7), 0x001860)]:
            with self.subTest(seconds=seconds):
                f.set_clock(seconds)
                f.wait(lambda: f.frames()[-1] == [rgb] * 12, timeout=1.5)
        self.assertEqual(f.fields(), (7, 50, 1, 2, 0))
        self.assertTrue(all(all((v >> bit) & 255 <= 96 for bit in (0, 8, 16))
                            for frame in f.frames() for v in frame))

    def test_midnight_keeps_night_and_morning_repaints_static_capture(self):
        f = self.fixture(rgb=True, state="capturing", clock=beijing_time(23, 59, 59)).ready()
        f.wait(lambda: f.frames()[-1:] == [[0x100000] * 12])
        before = len(f.frames())
        f.set_clock(beijing_time(0, day=25))
        time.sleep(1.1)
        self.assertEqual(len(f.frames()), before)
        f.set_clock(beijing_time(7, day=25))
        f.wait(lambda: f.frames()[-1] == [0x600000] * 12, timeout=1.5)

    def test_volume_overlay_scales_across_night_and_expires_to_real_state(self):
        f = self.fixture(rgb=True, control="v1 3 200 0 0 0\n", state="capturing",
                         clock=beijing_time(21, 59, 59)).ready()
        f.click(103)
        f.wait(lambda: f.frames()[-1:] == [[0x606060] * 12])
        f.set_clock(beijing_time(22))
        f.click(103)  # Clamp refresh: identical bars still redraw at the new cap.
        f.wait(lambda: f.frames()[-1] == [0x101010] * 12, timeout=1.5)
        f.wait(lambda: f.frames()[-1] == [0x100000] * 12, timeout=1.5)
        self.assertEqual(f.fields(), (3, 200, 0, 0, 0))

    def test_non_rgb_mute_is_red_and_pulse_follows_night_scale(self):
        f = self.fixture(control="v1 0 50 1 0 0\n", clock=beijing_time(12)).ready()
        f.wait(lambda: f.frames()[-1:] == [[96] * 12])
        f.set_clock(beijing_time(22))
        f.wait(lambda: f.frames()[-1] == [16] * 12, timeout=1.5)
        g = self.fixture(rgb=True, clock=beijing_time(22)).ready()
        g.wait(lambda: bool(g.frames()))
        self.assertEqual(g.frames()[0], [0x040400] * 12)
        g.wait(lambda: len(g.frames()) >= 2)
        self.assertEqual(g.frames()[1], [0x060600] * 12)

    def test_wall_clock_flapping_does_not_bypass_write_rate_or_term_off(self):
        f = self.fixture(rgb=True, state="capturing", clock=beijing_time(12)).ready()
        deadline = time.monotonic() + 2.15
        while time.monotonic() < deadline:
            f.set_clock(beijing_time(22))
            time.sleep(.02)
            f.set_clock(beijing_time(12))
            time.sleep(.02)
        self.assertEqual(f.stop(), 0)
        self.assertLessEqual(len(f.frames()), 4)
        self.assertEqual(f.frames()[-1], [0] * 12)
        self.assertTrue(all(all((v >> bit) & 255 <= 96 for bit in (0, 8, 16))
                            for frame in f.frames() for v in frame))

    def test_volume_bar_is_not_missed_and_expires_to_latest_ui(self):
        f = self.fixture(rgb=True).ready()
        f.wait(lambda: bool(f.frames()))
        f.click(103)
        bar = [0x202020] * 4 + [0] * 8  # gain 60 maps to ceil(60*12/200).
        f.wait(lambda: bar in f.frames())
        shown = time.monotonic()
        before = len(f.frames())
        (f.runtime / "ui.state").write_text("thinking\n")
        f.wait(lambda: len(f.frames()) > before and f.frames()[-1] == [0x000808] * 12, timeout=1.6)
        self.assertGreater(time.monotonic() - shown, .7)
        self.assertEqual(f.fields(), (1, 60, 0, 0, 0))

    def test_clamped_press_shows_and_refreshes_bar_without_counter_change(self):
        f = self.fixture(rgb=True, control="v1 3 200 0 0 0\n").ready()
        f.click(103)
        f.wait(lambda: [0x202020] * 12 in f.frames())
        before = len(f.frames())
        time.sleep(.65)
        f.click(103)
        time.sleep(.65)
        self.assertEqual(f.fields(), (3, 200, 0, 0, 0))
        self.assertEqual(f.frames()[-1], [0x202020] * 12)
        # Same rendered segments refresh the hold without another I2C frame.
        self.assertEqual(len(f.frames()), before)
        f.wait(lambda: f.frames()[-1] != [0x202020] * 12, timeout=1.4)

    def test_mute_wins_over_volume_and_volume_does_not_unmute(self):
        f = self.fixture(rgb=True).ready()
        f.click(103)
        f.wait(lambda: [0x202020] * 4 + [0] * 8 in f.frames())
        f.click(105)
        f.wait(lambda: f.frames()[-1] == [0x000820] * 12)
        before = len(f.frames())
        f.click(108)
        f.wait(lambda: f.fields() == (3, 50, 1, 1, 0))
        time.sleep(1.15)
        self.assertEqual(f.frames()[-1], [0x000820] * 12)
        self.assertEqual(len(f.frames()), before)

    def test_ui_muted_also_suppresses_pending_volume_bar(self):
        f = self.fixture(rgb=True).ready()
        (f.runtime / "ui.state").write_text("muted\n")
        f.click(103)
        f.wait(lambda: f.frames()[-1] == [0x000820] * 12)
        self.assertFalse(any(0x202020 in frame for frame in f.frames()))
        self.assertEqual(f.fields(), (1, 60, 0, 0, 0))

    def test_cancel_clears_bar_and_term_turns_all_leds_off(self):
        f = self.fixture(rgb=True).ready()
        f.click(103)
        f.wait(lambda: [0x202020] * 4 + [0] * 8 in f.frames())
        (f.runtime / "ui.state").write_text("idle\n")
        f.click(106)
        f.wait(lambda: f.fields() == (2, 60, 0, 1, 0))
        f.wait(lambda: f.frames()[-1] == [0] * 12)
        f.click(108)
        f.wait(lambda: [0x202020] * 3 + [0] * 9 in f.frames())
        self.assertEqual(f.stop(), 0)
        self.assertEqual(f.frames()[-1], [0] * 12)

    def test_key_flood_keeps_original_write_rate_and_control_semantics(self):
        f = self.fixture(rgb=True).ready()
        deadline = time.monotonic() + 2.15
        while time.monotonic() < deadline:
            f.click(103)
            f.click(108)
            time.sleep(.015)
        self.assertEqual(f.stop(), 0)
        self.assertLessEqual(len(f.frames()), 4)  # <=3 scheduled + final off.
        self.assertEqual(f.frames()[-1], [0] * 12)
        self.assertEqual(f.fields()[2:], (0, 0, 0))
        for frame in f.frames():
            self.assertTrue(all(all((value >> bit) & 255 <= 32 for bit in (0, 8, 16)) for value in frame))

    def test_failed_control_publish_does_not_show_unapplied_gain(self):
        f = self.fixture(rgb=True).ready()
        (f.runtime / (".panel.control.%d.next" % f.process.pid)).write_text("occupied")
        f.click(103)
        time.sleep(1.15)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        self.assertFalse(any(0x202020 in frame for frame in f.frames()))
        self.assertIsNone(f.process.poll())

    def test_ring_first_point_release_and_retouch_never_jump(self):
        f = self.fixture().ready()
        f.ring_start(200)
        f.event(330, 1)  # Duplicate touch-down does not discard the baseline.
        time.sleep(.08)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        f.ring_move(220)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))
        f.ring_release()
        f.ring_move(70)  # No active contact, ignored.
        f.ring_start(20)
        time.sleep(.08)
        self.assertEqual(f.fields(), (1, 60, 0, 0, 0))
        f.ring_move(0)
        f.wait(lambda: f.fields() == (2, 50, 0, 0, 0))

    def test_ring_both_wrap_directions_and_configurable_sign(self):
        f = self.fixture().ready()
        f.ring_start(230)
        f.ring_move(5)  # 241-coordinate period: shortest delta is +16.
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))
        f.ring_release()
        f.ring_start(5)
        f.ring_move(230)
        f.wait(lambda: f.fields() == (2, 50, 0, 0, 0))
        g = self.fixture(direction=-1).ready()
        g.ring_start(230)
        g.ring_move(5)
        g.wait(lambda: g.fields() == (1, 40, 0, 0, 0))

    def test_ring_uses_actual_min_max_and_accumulates_small_distances(self):
        f = self.fixture(absinfo=(10, 10, 109, 0, 0, 0)).ready()  # period100, step5.
        f.ring_start(107)
        f.ring_move(109)  # +2; first point alone and sub-step movement are quiet.
        f.ring_move(10)   # wrap +1, total3.
        time.sleep(.08)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        f.ring_move(12)   # +2, total5 => one step.
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))

    def test_ring_jitter_invalid_positions_and_large_teleports_do_not_jump(self):
        f = self.fixture(absinfo=(0, 0, 240, 2, 0, 0)).ready()
        f.ring_start(40)
        for position in (41, 42, 39, 40, 41, 40):
            f.ring_move(position)
        f.ring_move(160)  # Ambiguous half-turn: new baseline, no gain.
        f.ring_move(255)  # Outside reported range: baseline discarded.
        f.ring_move(20)   # Fresh valid baseline only.
        time.sleep(.08)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        f.ring_move(40)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))

    def test_ring_coalesces_fast_steps_with_80ms_commit_limit(self):
        f = self.fixture().ready()
        f.ring_start(0)
        f.ring_move(20)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))
        f.ring_move(40)
        time.sleep(.025)
        self.assertEqual(f.fields(), (1, 60, 0, 0, 0))
        time.sleep(.07)
        f.ring_move(41)  # Accumulated28 / step13 => two steps in one publish.
        f.wait(lambda: f.fields() == (3, 80, 0, 0, 0))

    def test_ring_fast_tail_flushes_without_another_position_and_after_release(self):
        for release in (False, True):
            f = self.fixture().ready()
            f.ring_start(0)
            f.ring_move(20)
            f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))
            f.ring_move(40)
            if release:
                f.ring_release()
            f.wait(lambda: f.fields() == (3, 80, 0, 0, 0), timeout=.4)

    def test_ring_sync_loss_discards_queued_whole_steps(self):
        f = self.fixture().ready()
        f.ring_start(0)
        f.ring_move(20)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))
        f.ring_move(40)
        f.event(3, 0, type_=0)
        f.event(0, 0, type_=0)
        time.sleep(.15)
        self.assertEqual(f.fields(), (1, 60, 0, 0, 0))

    def test_ring_syn_dropped_requires_new_contact_and_missing_abs_is_safe(self):
        f = self.fixture().ready()
        f.ring_start(0)
        f.event(3, 0, type_=0)
        f.ring_move(30)
        f.event(0, 0, type_=0)
        f.ring_move(60)
        time.sleep(.08)
        self.assertEqual(f.fields(), (0, 50, 0, 0, 0))
        f.ring_release()
        f.ring_start(70)
        f.ring_move(90)
        f.wait(lambda: f.fields() == (1, 60, 0, 0, 0))
        g = self.fixture(absinfo=None).ready()
        g.ring_start(10)
        g.ring_move(40)
        time.sleep(.1)
        self.assertEqual(g.fields(), (0, 50, 0, 0, 0))
        g.click(103)
        g.wait(lambda: g.fields() == (1, 60, 0, 0, 0))

    def test_ring_volume_overlay_mute_priority_and_clamp(self):
        f = self.fixture(rgb=True, control="v1 0 195 0 0 0\n").ready()
        f.ring_start(30)
        f.ring_move(50)
        f.wait(lambda: f.fields() == (1, 200, 0, 0, 0))
        f.wait(lambda: [0x202020] * 12 in f.frames())
        f.click(105)
        f.wait(lambda: f.frames()[-1] == [0x000820] * 12)
        f.ring_release()
        f.ring_start(30)
        f.ring_move(50)
        time.sleep(1.1)
        self.assertEqual(f.fields(), (2, 200, 1, 1, 0))
        self.assertEqual(f.frames()[-1], [0x000820] * 12)

    def test_bad_ui_state_is_safe_off_and_led_failure_does_not_exit(self):
        f = self.fixture().ready()
        (f.runtime / "ui.state").write_text("unsafe\nstate\n")
        time.sleep(1.05)
        self.assertTrue(all(int(row.split()[1], 16) == 0 for row in f.led.read_text().splitlines()[-12:]))
        f.led.unlink()
        f.click(28)
        f.wait(lambda: f.fields() == (1, 50, 0, 1, 1))
        self.assertIsNone(f.process.poll())

    def test_start_script_has_only_pinned_module_entrypoints(self):
        script = HERE.parent / "runtime/panel_start.sh"
        text = script.read_text()
        self.assertIn("load_panel_module cyttsp_button", text)
        self.assertIn("load_panel_module is31fl3236a", text)
        self.assertNotIn("rmmod", text)
        self.assertNotIn("/data", text)
        self.assertNotIn("ledserver", text)
        subprocess.run(["/bin/sh", "-n", str(script)], check=True)


if __name__ == "__main__":
    unittest.main()
