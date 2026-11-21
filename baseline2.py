import os
import sys
import subprocess
import time
import re

RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 2
TAG = sys.argv[2] if len(sys.argv) > 2 else time.strftime('%H%M%S')
BASE = '/tmp/b' + TAG

os.system('pkill -f sbd_ >/dev/null 2>&1')
os.system('pkill -f qemu-arm >/dev/null 2>&1')
time.sleep(2)

results = []

for i in range(RUNS):
    run = '%s_%d' % (BASE, i)
    subprocess.run(['rm', '-rf', run])
    env = dict(os.environ)
    env['ROOT'] = '/root/adp1_raw'
    env['RUN'] = run
    env['PROP'] = '/tmp/prop_new.bin'
    env['QEMU'] = '/root/qemu-8.2.4/build-musl/qemu-arm'
    subprocess.run(['timeout', '1600', 'python3', '/root/run_loop.py'],
                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(2)

    bl = os.path.join(run, 'binder.log')
    depth = -1
    reached = '?'
    if os.path.exists(bl):
        out = subprocess.run(['python3', '/root/bootdepth.py', bl],
                             capture_output=True, text=True).stdout
        m = re.search(r'boot depth\s*:\s*(\d+) / \d+ -> (\S+)', out)
        if m:
            depth = int(m.group(1))
            reached = m.group(2)
    results.append((run, depth, reached))
    print('%-18s depth=%d reached=%s' % (run, depth, reached), flush=True)

depths = [r[1] for r in results]
print()
print('depths :', depths)
print('median :', sorted(depths)[len(depths) // 2])
print('reached:', [r[2] for r in results])