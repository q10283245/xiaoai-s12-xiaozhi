#!/usr/bin/env python3
"""Run offline suites once per test ID, including shared imported fixtures."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]


def cases(suite):
    for entry in suite:
        if isinstance(entry, unittest.TestSuite):
            yield from cases(entry)
        else:
            yield entry


def main():
    selected, seen = unittest.TestSuite(), set()
    for directory in ('device', 'scripts', 'tests'):
        source = ROOT / directory
        sys.path.insert(0, str(source))
        suite = unittest.TestLoader().discover(str(source), pattern='test_*.py')
        for case in cases(suite):
            identity = case.id()
            if identity not in seen:
                selected.addTest(case)
                seen.add(identity)
    result = unittest.TextTestRunner(verbosity=2).run(selected)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    raise SystemExit(main())
