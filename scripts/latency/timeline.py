#!/usr/bin/env python3
"""Print the blocking waits of a guest log recorded with NM_RT_TRACE=1.

  timeline.py <guest log> [min waits per process]

One line per wait: process (last 3 pid digits), ms since the first wait,
duration in ms, and the Venus command it waited for. Processes with fewer than
the given number of waits (default 30) are skipped, which leaves the app and
hides Wine's helpers. Needs MESA_SRC (default ~/src-latency/mesa) for the
command names.
"""
import collections, os, re, sys

src = os.environ.get('MESA_SRC', os.path.expanduser('~/src-latency/mesa'))
names = {}
for l in open(f'{src}/src/virtio/venus-protocol/vn_protocol_driver_defines.h'):
    m = re.match(r'\s*VK_COMMAND_TYPE_(\w+?)_EXT = (\d+)', l)
    if m:
        names[int(m.group(2))] = m.group(1)
names.update({8000: 'roundtrip', 8001: 'wait_all', 8002: 'RENDERER', 8003: 'other'})
rows = [l.split() for l in open(sys.argv[1]) if l.startswith('NMT ')]
pids = collections.Counter(r[1] for r in rows)
print(dict(pids))
t0 = float(rows[0][2])
minw = int(sys.argv[2]) if len(sys.argv) > 2 else 30
for r in rows:
    if pids[r[1]] >= minw:
        print(r[1][-3:], '%7.0f' % (float(r[2]) - t0), '%5s' % r[4], names.get(int(r[3]), r[3]))
