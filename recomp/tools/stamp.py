#!/usr/bin/env python3
"""Prefix each stdin line with the seconds since this started: stamp.py < in > out"""
import sys
import time

STAMP_FMT = "[{:8.3f}] "


def main():
    t0 = time.monotonic()
    for line in sys.stdin:
        sys.stdout.write(STAMP_FMT.format(time.monotonic() - t0) + line)
        sys.stdout.flush()


if __name__ == "__main__":
    main()
