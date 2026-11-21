#!/usr/bin/env python3
import re
import sys

# The order this image actually registers them in. The previous list demanded
# window, entropy, mount, installd and statusbar, which this build never adds —
# so a run that registered every one of the seventeen services still scored 5.
ORDER = [
    'media.audio_', 'media.player', 'media.camera',
    'SurfaceFling',
    'power', 'batteryinfo', 'telephony.re', 'package',
    'activity', 'meminfo', 'cpuinfo',
    'activity.bro', 'activity.ser', 'activity.sen', 'activity.pro',
    'permission', 'netstat',
]

def parse(path):
    pat = re.compile(rb'w14\.\.21=([0-9a-f ]+)')
    out = []
    for line in open(path, 'rb').read().split(b'\n'):
        if b'PARCEL code=3' not in line:
            continue
        m = pat.search(line)
        if not m:
            continue
        words = m.group(1).split()
        if len(words) < 3:
            continue
        try:
            raw = b''.join(bytes.fromhex(w.decode())[::-1] for w in words)
        except Exception:
            continue
        s = raw.decode('utf-16-le', 'ignore')
        t = ''.join(c for c in s[4:] if 32 <= ord(c) < 127).rstrip('\x00')
        if t:
            out.append(t)
    return out

def main():
    path = sys.argv[1]
    names = parse(path)
    uniq = []
    for n in names:
        if n not in uniq:
            uniq.append(n)
    print('addService records :', len(names))
    print('unique names       :', len(uniq))
    for n in uniq:
        print('   ', n)

    # Count how many of the expected services are present, not how long the
    # leading run of them is. media.player and media.camera are added by
    # mediaserver on its own schedule and are legitimately absent in some runs;
    # a strict prefix stopped at the first one missing and reported 1 out of 17
    # while fifteen services were in fact registered.
    hit_names = []
    miss_names = []
    for step in ORDER:
        if any(n.startswith(step) for n in uniq):
            hit_names.append(step)
        else:
            miss_names.append(step)

    depth = len(hit_names) - 1
    reached = hit_names[-1] if hit_names else None

    if miss_names:
        print()
        print('missing            :', ', '.join(miss_names))

    print('boot depth         :', (depth + 1), '/', len(ORDER), '->', reached if reached else 'nothing')
    if depth + 1 < len(ORDER):
        print('next expected      :', ORDER[depth + 1])

if __name__ == '__main__':
    main()