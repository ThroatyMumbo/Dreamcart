#!/usr/bin/env python3
"""Summarize a play or live log's per-second and per-frame lines: playlog.py LOG [--brief]"""
import argparse
import re

STAMP = re.compile(r"^\[ *\d+\.\d+\] ")

PLAY = re.compile(r"^play +(\d+): fields +(\d+) (?:\(vi intrs +\d+\) )?frames +(\d+) audio +(\d+) \| ms gfx +(\d+) \(pvr wait +(\d+)\) audio +(\d+) "
                  r"pi +(\d+) \((\d+) KB(?:, cart (\d+) KB)?\) idle +(\d+) game +(\d+) \| tris/frame (\d+)")
FRAME = re.compile(r"^fr((?: \d+){14})$")
FRAME_COLS = ["n", "fields", "wall", "gfx", "audio", "tasks", "pi", "kb", "cart", "idle", "rep", "game", "tris", "uploads"]
MS_COLS = {"wall", "gfx", "audio", "pi", "idle", "rep", "game"}
NOTE = re.compile(r"^(pad\S* .*|os: .*|os_boot: .*|rcp: .*|pif: .*|hostmmu: .*|exit: .*|play: .*|bench: .*|gfx: .*)$")


def frame_table(frames):
    """Per-frame lines grouped by how many VI fields each frame took, then the slowest frames one by one."""
    if not frames:
        return
    frames = [{k: (v / 10 if k in MS_COLS else v) for k, v in zip(FRAME_COLS, f)} for f in frames]
    drawn = [f for f in frames if f["tris"]]
    show = ["wall", "gfx", "audio", "tasks", "pi", "kb", "cart", "idle", "game", "tris", "uploads"]
    print(f"\n{len(drawn)} drawn frames; ms per frame by fields taken (2 fields = 30 fps, 3 = 20, 4 = 15)")
    print("fields frames  " + " ".join(f"{c:>7}" for c in show))
    for n in sorted({f["fields"] for f in drawn}):
        g = [f for f in drawn if f["fields"] == n]
        print(f"{n:>6} {len(g):>6}  " + " ".join(f"{sum(f[c] for f in g) / len(g):7.1f}" for c in show))
    print("\nslowest 15 drawn frames")
    print("     n fields  " + " ".join(f"{c:>7}" for c in show))
    for f in sorted(drawn, key=lambda f: -f["wall"])[:15]:
        print(f"{f['n']:>6} {f['fields']:>6}  " + " ".join(f"{f[c]:7.1f}" for c in show))


def summarize_session(lines, brief):
    rows, frames = [], []
    for line in lines:
        m = FRAME.match(line)
        if m:
            frames.append([int(x) for x in m.group(1).split()])
            continue
        m = PLAY.match(line)
        if m:
            rows.append([int(x or 0) for x in m.groups()])
            if not brief:
                print(line)
        elif NOTE.match(line) and not brief:
            print(f"   {line}")
    if rows:
        live = [r for r in rows if r[2]]
        if live:
            fps = sum(r[2] for r in live) / len(live)
            print(f"{len(rows)} s reported, {len(live)} with frames: {fps:.1f} frames/s average over those")
    frame_table(frames)
    return rows


def boot_times(text):
    """Seconds from launch, off the host stamps: KOS up, cartboot start, first drawn frame, the SD upload's end."""
    marks, start = {}, None
    for raw in text.splitlines():
        m = re.match(r"^\[ *(\d+\.\d+)\] (.*)$", raw.strip())
        if not m:
            continue
        t, line = float(m.group(1)), m.group(2)
        if line.startswith("KallistiOS v"):
            marks, start = {"kos": t}, t
        elif line.startswith('cartboot: "'):
            marks.setdefault("cartboot", t)
        elif line.startswith("xcache:") and " sent to the reader" in line:
            marks.setdefault("sd sent", t)
        else:
            f = FRAME.match(line)
            if f and int(f.group(1).split()[12]):
                marks.setdefault("first frame", t)
    if start is None:
        return
    print("\nfrom launch: " + ", ".join(f"{k} {v:.1f} s" for k, v in marks.items()))
    if "cartboot" in marks and "first frame" in marks:
        print(f"cartboot start to first drawn frame: {marks['first frame'] - marks['cartboot']:.1f} s")


def summarize(text, brief=False):
    """One summary per KOS boot: a Dreamcart 64 log holds a game session for each restart through its menu."""
    sessions = [[]]
    for line in text.splitlines():
        line = STAMP.sub("", line.strip())
        if line.startswith("KallistiOS v") and sessions[-1]:
            sessions.append([])
        sessions[-1].append(line)
    games = [s for s in sessions if any(PLAY.match(line) for line in s)] or sessions[-1:]
    rows = []
    for i, s in enumerate(games):
        if len(games) > 1:
            title = next((line.split('"')[1].strip() for line in s if line.startswith("cartboot: \"")), "?")
            print(f"\n=== game session {i + 1} of {len(games)}: {title}")
        rows += summarize_session(s, brief)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--brief", action="store_true", help="only the per-session summaries, not the per-second lines")
    a = ap.parse_args()
    text = open(a.log).read()
    summarize(text, a.brief)
    boot_times(text)


if __name__ == "__main__":
    main()
