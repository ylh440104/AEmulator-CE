import os
import socket
import sys
import time

PATH = '/tmp/longrun.sock'
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 600
GAP = float(sys.argv[2]) if len(sys.argv) > 2 else 0.02

if os.path.exists(PATH):
    os.unlink(PATH)

srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(PATH)
srv.listen(1024)
srv.settimeout(0)

held = []
ok = 0
refused = 0
other = 0
first_fail = None
t0 = time.time()

while time.time() - t0 < DUR:
    c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        c.connect(PATH)
        ok += 1
        held.append(c)
    except ConnectionRefusedError:
        refused += 1
        c.close()
        if first_fail is None:
            first_fail = time.time() - t0
    except OSError:
        other += 1
        c.close()

    # accept everything queued, like the daemon does
    while True:
        try:
            a, _ = srv.accept()
            a.close()
        except BlockingIOError:
            break
        except OSError:
            break

    time.sleep(GAP)

print('duration  :', round(time.time() - t0, 1))
print('connected :', ok)
print('refused   :', refused)
print('other     :', other)
print('held open :', len(held))
print('first fail:', first_fail)

srv.close()
if os.path.exists(PATH):
    os.unlink(PATH)