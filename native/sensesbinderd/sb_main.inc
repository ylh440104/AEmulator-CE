int main(int argc, char **argv) {
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] != '-') path = argv[i];
        else return 2;
    }
    if (!path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 2;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, 0, _IOLBF, 0);
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (ls < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    memcpy(sa.sun_path, path, strlen(path) + 1);
    unlink(path);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 128) < 0) {
        perror("bind/listen"); close(ls); return 1;
    }
    g_listen_fd = ls;
    ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = 0 };
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev) < 0) {
        perror("epoll"); close(ls); return 1;
    }
    logf_("sensesbinderd: listening on %s", path);
    for (;;) {
        struct epoll_event events[64];
        int n = epoll_wait(ep, events, 64, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait"); return 1;
        }
        for (int i = 0; i < n; i++) {
            struct thread *th = events[i].data.ptr;
            if (!th) {
                for (int accepted = 0; accepted < 64; accepted++) {
                    int fd = accept4(ls, 0, 0, SOCK_CLOEXEC | SOCK_NONBLOCK);
                    if (fd < 0) {
                        if (errno == EINTR) { accepted--; continue; }
                        if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept4");
                        break;
                    }
                    th = zalloc(sizeof *th);
                    th->fd = fd;
                    ev.events = EPOLLIN | EPOLLRDHUP;
                    ev.data.ptr = th;
                    if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) < 0) {
                        close(fd); free(th); continue;
                    }
                    V("binder: accepted fd %d", fd);
                }
                continue;
            }
            if (th->fd < 0) continue;
            if (events[i].events & EPOLLOUT) {
                if (io_flush(th) < 0) { disconnect_thread(th); continue; }
            }
            if (th->fd >= 0 && events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP))
                on_message(th);
        }
    }
}