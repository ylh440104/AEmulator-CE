#!/usr/bin/env python3
import os
import socket
import subprocess
import sys
import time

ROOT = os.environ.get("ROOT", "/root/adp1_raw")
RUN = os.environ.get("RUN", "/tmp/adrun")
QEMU = os.environ.get("QEMU", "/tmp/qemu_senses_orig")
PROP = os.environ.get("PROP", "/tmp/proparea.bin")
BSOCK = ROOT + "/dev/socket/sensesbinder"
ZPATH = ROOT + "/dev/socket/zygote"
BOOT = ("/system/framework/core.jar:/system/framework/ext.jar:"
        "/system/framework/framework.jar:/system/framework/android.policy.jar:"
        "/system/framework/services.jar")

os.makedirs(RUN + "/socket", exist_ok=True)
os.makedirs(ROOT + "/data/dalvik-cache", exist_ok=True)
os.makedirs(ROOT + "/data/misc", exist_ok=True)
os.makedirs(ROOT + "/data/anr", exist_ok=True)
os.makedirs(ROOT + "/cache", exist_ok=True)


def prop_fd():
    fd = os.open(PROP, os.O_RDWR)
    size = os.fstat(fd).st_size
    os.set_inheritable(fd, True)
    import os as _o
    _o.dup2(fd, 30)
    _o.set_inheritable(30, True)
    return 30, size


def main():
    if os.path.exists(ZPATH):
        os.unlink(ZPATH)
    os.system("pkill -f sbd_ >/dev/null 2>&1")
    os.system("pkill -f qemu-arm >/dev/null 2>&1")
    time.sleep(1)

    pfd, plen = prop_fd()
    ws = "%d,%d" % (pfd, plen)

    genv = {
        "GUEST_BINDER": BSOCK,
        "GUEST_BINDER_LOG": RUN + "/binder.log",
        "GUEST_BINDER_VERBOSE": "1",
        "GUEST_ANDROID_LOG": "1",
        "ANDROID_PROPERTY_WORKSPACE": ws,

    }

    ge = [
        "-E", "PATH=/system/bin:/system/xbin:/sbin",
        "-E", "LD_LIBRARY_PATH=/system/lib",
        "-E", "ANDROID_BOOTLOGO=1",
        "-E", "ANDROID_ROOT=/system",
        "-E", "ANDROID_ASSETS=/system/app",
        "-E", "ANDROID_DATA=/data",
        "-E", "EXTERNAL_STORAGE=/sdcard",
        "-E", "BOOTCLASSPATH=" + BOOT,
        

    ]

    def spawn(tag, prog, args, extra=(), fds=None):
        cmd = [QEMU, "-cpu", "arm926", "-L", ROOT] + ge + list(extra) + ["--", ROOT + prog] + args
        env = dict(os.environ)
        env.update(genv)
        out = open(RUN + "/" + tag + ".log", "wb")
        return subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT,
                                env=env, pass_fds=(pfd,) + tuple(fds or ()))

    env = dict(os.environ)
    env.update(genv)
    bd = subprocess.Popen(["/tmp/sbd_probe4", "-s", BSOCK, "-v"],
                          stdout=open(RUN + "/binderd.log", "wb"),
                          stderr=subprocess.STDOUT, env=env)
    time.sleep(2)

    sm = spawn("servicemanager", "/system/bin/servicemanager", [])
    time.sleep(3)

    sf = None
    time.sleep(2)
    ms = spawn("mediaserver", "/system/bin/mediaserver", [])
    time.sleep(2)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(ZPATH)
    os.chmod(ZPATH, 0o666)
    s.listen(4)
    sfd = s.fileno()
    os.set_inheritable(sfd, True)

    zy = spawn("zygote", "/system/bin/app_process",
               ["-Xzygote", "/system/bin", "--zygote", "--start-system-server"],
               ["-E", "ANDROID_SOCKET_zygote=%d" % sfd], fds=(sfd,))

    print("binderd=%d sm=%d sf=%d ms=%d zygote=%d prop=%s" %
          (bd.pid, sm.pid, (0), ms.pid, zy.pid, ws))
    # Keep the daemon alive for the whole guest run.
    #
    # 200 seconds of wall clock was not enough: the daemon was stopped at host
    # t=208 while the guest was still at guest t=747 and asking for binder. Every
    # later connect got ECONNREFUSED because nothing was listening any more, and
    # the guest burned hundreds of attempts in that window. That is what all the
    # ECONNREFUSED traffic was: the daemon had already been stopped.
    #
    # The guest clock runs much faster than wall clock under qemu-user, so this
    # sleep has to be generously longer than the guest timestamps suggest.
    # Wait for the guest to finish, not for a fixed number of wall seconds.
    #
    # The guest clock runs far faster than wall clock under qemu-user: a run that
    # reached guest t=999 was still going when the daemon's 600 second sleep
    # expired. Killing the daemon under a live guest produces exactly the burst of
    # ECONNREFUSED that earlier rounds chased as a socket bug.
    #
    # Poll the guest process instead: keep the daemon up until every guest process
    # has exited, with a long ceiling so a hung guest cannot block forever.
    deadline = time.time() + 1500
    while time.time() < deadline:
        alive = []
        for p_ in (zy, ms, sm, sf):
            if p_ is not None and p_.poll() is None:
                alive.append(p_)
        if not alive:
            break
        time.sleep(2)
    for p in (zy, ms, sm, bd):
        p.terminate()
    return 0


sys.exit(main())