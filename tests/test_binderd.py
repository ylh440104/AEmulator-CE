import array
import contextlib
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

H = struct.Struct('<IHHII')
T = struct.Struct('<6I4Q')
P = struct.Struct('<IIQQ')
MAGIC = 0x53420000
BINARY = os.environ.get('BINDERD', '/tmp/sensesbinderd-test')


def frame(kind, tid, body=b'', nfds=0):
    return H.pack(H.size + len(body), kind, nfds, tid, MAGIC) + body


def transaction(code, data=b'', offsets=(), handle=0, flags=0):
    return T.pack(handle, code, flags, 0, 1000, 0, 0, 0, len(data), len(offsets) * 8) + data + b''.join(struct.pack('<Q', x) for x in offsets)


def exact(s, length):
    out = b''
    while len(out) < length:
        part = s.recv(length - len(out))
        if not part:
            raise EOFError('closed')
        out += part
    return out


def receive(s):
    header = exact(s, H.size)
    length, kind, nfds, seq, magic = H.unpack(header)
    if length < H.size or length > 8 * 1024 * 1024 or magic & 0xffff0000 != MAGIC:
        raise AssertionError(('invalid frame', header.hex()))
    return kind, exact(s, length - H.size)


class BinderTests(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp(prefix='sbtest_'))
        self.path = self.folder / 'binder.sock'
        self.log = (self.folder / 'daemon.log').open('wb')
        self.process = subprocess.Popen([BINARY, '-s', str(self.path), '-v'], stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        self.sockets = []
        until = time.monotonic() + 5
        while not self.path.exists():
            if self.process.poll() is not None or time.monotonic() > until:
                self.fail('daemon failed to start: ' + str(self.folder))
            time.sleep(0.01)

    def tearDown(self):
        for s in self.sockets:
            s.close()
        if self.process.poll() is None:
            os.killpg(self.process.pid, signal.SIGTERM)
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=2)
        self.log.close()

    def client(self, pid, tid=None, first=1):
        tid = tid or pid
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        self.sockets.append(s)
        s.connect(str(self.path))
        s.sendall(frame(1, tid, struct.pack('<4I', pid, 1000, 1000, first)))
        self.assertEqual(receive(s)[0], 64)
        return s

    def manager(self):
        s = self.client(1001)
        s.sendall(frame(2, 1001))
        self.assertEqual(receive(s)[0], 64)
        return s

    def wait(self, s, tid):
        s.sendall(frame(11, tid))

    def ack(self, s):
        changes = []
        for _ in range(16):
            kind, body = receive(s)
            if kind == 75:
                return changes
            self.assertEqual(kind, 71)
            self.assertEqual(len(body) % P.size, 0)
            changes.extend(P.iter_unpack(body))
        self.fail('ACK missing')

    def fragment(self, split):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        self.sockets.append(s)
        s.connect(str(self.path))
        data = frame(1, 1001, struct.pack('<4I', 1001, 1000, 1000, 1))
        s.sendall(data[:split])
        time.sleep(0.15)
        s.sendall(data[split:])
        self.assertEqual(receive(s)[0], 64)

    def test_complete_hello(self):
        self.fragment(32)

    def test_split_header(self):
        self.fragment(8)

    def test_split_body(self):
        self.fragment(16)

    def route(self, late=False):
        sm = self.manager()
        caller = self.client(2001)
        workers = [self.client(2001, 2002 + i, 0) for i in range(3)]
        for i, w in enumerate(workers):
            self.wait(w, 2002 + i)
        self.wait(sm, 1001)
        caller.sendall(frame(4, 2001, transaction(77, b'call')))
        self.ack(caller)
        self.assertEqual(receive(sm)[0], 66)
        if not late:
            self.wait(caller, 2001)
        sm.sendall(frame(5, 1001, transaction(0, b'done')))
        self.ack(sm)
        if late:
            self.assertFalse(select.select(workers + [caller], [], [], 0.05)[0])
            self.wait(caller, 2001)
        kind, body = receive(caller)
        self.assertEqual(kind, 67)
        self.assertEqual(body[T.size:], b'done')
        self.assertFalse(select.select(workers, [], [], 0.05)[0])

    def test_reply_returns_to_caller(self):
        self.route()

    def test_reply_waits_for_caller(self):
        self.route(True)

    def publish(self, sm, owner, code=3):
        obj = struct.pack('<4I', 0x73622a85, 0x17f, 0x20000, 0x10000)
        self.wait(sm, 1001)
        owner.sendall(frame(4, 2001, transaction(code, obj, (0,))))
        refs = self.ack(owner)
        self.assertEqual([(r[0], r[2], r[3]) for r in refs], [(0, 0x20000, 0x10000), (1, 0x20000, 0x10000)])
        for op, pad, ptr, cookie in refs:
            owner.sendall(frame(7, 2001, P.pack(op, pad, ptr, cookie)))
        kind, body = receive(sm)
        self.assertEqual(kind, 66)
        fbo = struct.unpack_from('<4I', body, T.size)
        self.assertEqual(fbo[0], 0x73682a85)
        self.assertGreater(fbo[2], 0)
        sm.sendall(frame(6, 1001, struct.pack('<II', 1, fbo[2])))
        sm.sendall(frame(14, 1001, struct.pack('<Q', T.unpack_from(body)[5])))
        self.wait(owner, 2001)
        sm.sendall(frame(5, 1001, transaction(0, struct.pack('<i', 0))))
        self.ack(sm)
        self.assertEqual(receive(owner)[0], 67)
        return fbo[2]

    def test_reference_identity_and_target(self):
        sm = self.manager()
        owner = self.client(2001)
        handle = self.publish(sm, owner)
        self.wait(owner, 2001)
        sm.sendall(frame(4, 1001, transaction(99, b'ping', handle=handle)))
        self.ack(sm)
        kind, body = receive(owner)
        self.assertEqual(kind, 66)
        tx = T.unpack_from(body)
        self.assertEqual(tx[6:8], (0x20000, 0x10000))
        self.assertEqual(tx[1], 99)

    def test_oneway_waits_for_correct_buffer(self):
        sm = self.manager()
        owner = self.client(2001)
        handle = self.publish(sm, owner)
        worker = self.client(2001, 2002, 0)
        self.wait(owner, 2001)
        sm.sendall(frame(4, 1001, transaction(101, b'a', handle=handle, flags=1)))
        self.ack(sm)
        kind, first = receive(owner)
        self.assertEqual(kind, 66)
        first_id = T.unpack_from(first)[5]
        self.wait(worker, 2002)
        sm.sendall(frame(4, 1001, transaction(102, b'b', handle=handle, flags=1)))
        self.ack(sm)
        worker.sendall(frame(14, 2002, struct.pack('<Q', first_id + 99)))
        self.assertFalse(select.select([worker], [], [], 0.05)[0])
        owner.sendall(frame(14, 2001, struct.pack('<Q', first_id)))
        kind, second = receive(worker)
        self.assertEqual(kind, 66)
        self.assertEqual(T.unpack_from(second)[1], 102)

    def test_nested_call_returns_to_waiting_sender(self):
        sm = self.manager()
        owner = self.client(2001)
        handle = self.publish(sm, owner)
        self.wait(owner, 2001)
        sm.sendall(frame(4, 1001, transaction(51, b'outer', handle=handle)))
        self.ack(sm)
        self.assertEqual(receive(owner)[0], 66)
        self.wait(sm, 1001)
        owner.sendall(frame(4, 2001, transaction(52, b'inner')))
        self.ack(owner)
        kind, body = receive(sm)
        self.assertEqual(kind, 66)
        self.assertEqual(T.unpack_from(body)[1], 52)
        self.wait(owner, 2001)
        sm.sendall(frame(5, 1001, transaction(0, b'inner reply')))
        self.ack(sm)
        self.assertEqual(receive(owner)[0], 67)
        self.wait(sm, 1001)
        owner.sendall(frame(5, 2001, transaction(0, b'outer reply')))
        self.ack(owner)
        kind, body = receive(sm)
        self.assertEqual(kind, 67)
        self.assertEqual(body[T.size:], b'outer reply')

    def test_handle_returns_home_as_original_object(self):
        sm = self.manager()
        owner = self.client(2001)
        handle = self.publish(sm, owner)
        self.wait(sm, 1001)
        owner.sendall(frame(4, 2001, transaction(2)))
        self.ack(owner)
        self.assertEqual(receive(sm)[0], 66)
        obj = struct.pack('<4I', 0x73682a85, 0x17f, handle, 0)
        self.wait(owner, 2001)
        sm.sendall(frame(5, 1001, transaction(0, obj, (0,))))
        self.ack(sm)
        kind, body = receive(owner)
        self.assertEqual(kind, 67)
        self.assertEqual(struct.unpack_from('<4I', body, T.size), (0x73622a85, 0x17f, 0x20000, 0x10000))

    def test_rights_survive_fragmented_body(self):
        sm = self.manager()
        caller = self.client(2001)
        self.wait(sm, 1001)
        with tempfile.TemporaryFile() as f:
            f.write(b'fd payload')
            f.seek(0)
            obj = struct.pack('<4I', 0x66642a85, 0, f.fileno(), 0)
            data = frame(4, 2001, transaction(88, obj, (0,)), 1)
            caller.sendmsg([data[:H.size]], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [f.fileno()]))])
            time.sleep(0.12)
            caller.sendall(data[H.size:])
            self.ack(caller)
            header, ancillary, flags, address = sm.recvmsg(H.size, socket.CMSG_SPACE(4))
            header += exact(sm, H.size - len(header))
            length, kind, nfds, seq, magic = H.unpack(header)
            self.assertEqual((kind, nfds), (66, 1))
            body = exact(sm, length - H.size)
            received = []
            for level, ty, value in ancillary:
                if level == socket.SOL_SOCKET and ty == socket.SCM_RIGHTS:
                    ids = array.array('i')
                    ids.frombytes(value[:len(value) - len(value) % ids.itemsize])
                    received.extend(ids)
            try:
                self.assertEqual(len(received), 1)
                self.assertEqual(os.read(received[0], 10), b'fd payload')
                self.assertEqual(struct.unpack_from('<4I', body, T.size)[2], 0)
            finally:
                for fd in received:
                    os.close(fd)

    def test_invalid_frame_isolated(self):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2)
        self.sockets.append(s)
        s.connect(str(self.path))
        s.sendall(H.pack(8, 1, 0, 1001, MAGIC))
        self.assertEqual(s.recv(1), b'')
        self.client(2002)

    def test_prehello_close_not_spinning(self):
        for _ in range(12):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
                s.connect(str(self.path))
        self.client(1001)
        time.sleep(0.05)
        self.assertLess((self.folder / 'daemon.log').stat().st_size, 30000)

    def test_backpressure_keeps_all_replies(self):
        s = self.client(1001)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        packet = frame(3, 1001, struct.pack('<I', 1))
        s.sendall(packet * 6000)
        time.sleep(0.05)
        for _ in range(6000):
            self.assertEqual(receive(s)[0], 64)
        self.client(2001)


if __name__ == '__main__':
    unittest.main(verbosity=2)
