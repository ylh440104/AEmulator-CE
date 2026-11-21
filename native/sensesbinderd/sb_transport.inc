static int io_events(struct thread *th) {
    struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.ptr = th };
    if (th->io.head) ev.events |= EPOLLOUT;
    return epoll_ctl(ep, EPOLL_CTL_MOD, th->fd, &ev);
}

static void io_output_free(struct sb_output *out) {
    for (int i = 0; i < out->nfds; i++) close(out->fds[i]);
    free(out->data);
    free(out);
}

static void io_dispose(struct thread *th) {
    struct sb_io *io = &th->io;
    for (int i = 0; i < io->nfds; i++) close(io->fds[i]);
    free(io->body);
    while (io->head) {
        struct sb_output *out = io->head;
        io->head = out->next;
        io_output_free(out);
    }
    memset(io, 0, sizeof *io);
}

static int io_flush(struct thread *th) {
    struct sb_io *io = &th->io;
    if (th->fd < 0 || io->error) { errno = EPIPE; return -1; }
    size_t budget = 1024u * 1024u;
    while (io->head && budget) {
        struct sb_output *out = io->head;
        size_t left = out->size - out->sent;
        if (left > budget) left = budget;
        struct iovec vec = { out->data + out->sent, left };
        union {
            struct cmsghdr alignment;
            char bytes[CMSG_SPACE(sizeof(int) * SB_MAXFDS)];
        } control;
        struct msghdr msg = { .msg_iov = &vec, .msg_iovlen = 1 };
        if (out->nfds) {
            memset(&control, 0, sizeof control);
            msg.msg_control = control.bytes;
            msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)out->nfds);
            struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
            cm->cmsg_level = SOL_SOCKET;
            cm->cmsg_type = SCM_RIGHTS;
            cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)out->nfds);
            memcpy(CMSG_DATA(cm), out->fds, sizeof(int) * (size_t)out->nfds);
        }
        ssize_t n = sendmsg(th->fd, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0) {
            io->error = n == 0 ? EPIPE : errno;
            shutdown(th->fd, SHUT_RDWR);
            errno = io->error;
            return -1;
        }
        for (int i = 0; i < out->nfds; i++) close(out->fds[i]);
        out->nfds = 0;
        out->sent += (size_t)n;
        io->queued -= (size_t)n;
        budget -= (size_t)n;
        if (out->sent == out->size) {
            io->head = out->next;
            if (!io->head) io->tail = 0;
            io_output_free(out);
        }
    }
    if (io_events(th) < 0) {
        io->error = errno;
        shutdown(th->fd, SHUT_RDWR);
        return -1;
    }
    return 0;
}

static int io_send(struct thread *th, u32 type, const void *body, u32 blen,
                   const int *fds, int nfds, u32 flags) {
    if (!th || th->fd < 0 || th->io.error) { errno = ENOTCONN; return -1; }
    if (blen > SB_MAXMSG - sizeof(struct sb_hdr) || nfds < 0 || nfds > SB_MAXFDS) {
        errno = EMSGSIZE;
        return -1;
    }
    size_t total = sizeof(struct sb_hdr) + blen;
    if (th->io.queued > 4u * SB_MAXMSG - total) {
        th->io.error = ENOBUFS;
        shutdown(th->fd, SHUT_RDWR);
        errno = ENOBUFS;
        return -1;
    }
    struct sb_output *out = calloc(1, sizeof *out);
    if (!out) return -1;
    out->data = malloc(total);
    if (!out->data) { free(out); return -1; }
    out->size = total;
    for (int i = 0; i < nfds; i++) {
        int fd = fcntl(fds[i], F_DUPFD_CLOEXEC, 3);
        if (fd < 0) { int e = errno; io_output_free(out); errno = e; return -1; }
        out->fds[out->nfds++] = fd;
    }
    struct sb_hdr h = {
        .len = (u32)total, .type = (u16)type, .nfds = (u16)nfds,
        .tid = ++th->iseq, .flags = SB_F_MAGIC | flags
    };
    memcpy(out->data, &h, sizeof h);
    if (blen) memcpy(out->data + sizeof h, body, blen);
    if (th->io.tail) th->io.tail->next = out;
    else th->io.head = out;
    th->io.tail = out;
    th->io.queued += total;
    return io_flush(th);
}

static int io_receive(struct thread *th) {
    struct sb_io *io = &th->io;
    if (io->error) { errno = io->error; return -1; }
    for (;;) {
        int header = io->header_used < sizeof io->header;
        if (!header) {
            if (io->header.len < sizeof io->header || io->header.len > SB_MAXMSG ||
                (io->header.flags & SB_F_MAGIC_MASK) != SB_F_MAGIC ||
                io->header.nfds > SB_MAXFDS) {
                errno = EPROTO;
                return -1;
            }
            size_t size = io->header.len - sizeof io->header;
            if (size > io->body_cap) {
                unsigned char *body = realloc(io->body, size);
                if (!body) return -1;
                io->body = body;
                io->body_cap = size;
            }
            if (io->body_used == size) {
                if (io->nfds != io->header.nfds) { errno = EPROTO; return -1; }
                return 1;
            }
        }
        struct iovec vec;
        if (header) {
            vec.iov_base = (char *)&io->header + io->header_used;
            vec.iov_len = sizeof io->header - io->header_used;
        } else {
            vec.iov_base = io->body + io->body_used;
            vec.iov_len = io->header.len - sizeof io->header - io->body_used;
        }
        union {
            struct cmsghdr alignment;
            char bytes[CMSG_SPACE(sizeof(int) * SB_MAXFDS)];
        } control;
        memset(&control, 0, sizeof control);
        struct msghdr msg = {
            .msg_iov = &vec, .msg_iovlen = 1,
            .msg_control = control.bytes, .msg_controllen = sizeof control.bytes
        };
        ssize_t n = recvmsg(th->fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) { if (!n) errno = ECONNRESET; return -1; }
        int invalid = (msg.msg_flags & MSG_CTRUNC) != 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) continue;
            if (cm->cmsg_len < CMSG_LEN(0)) { invalid = 1; continue; }
            size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                int fd;
                memcpy(&fd, (char *)CMSG_DATA(cm) + i * sizeof fd, sizeof fd);
                if (io->nfds < SB_MAXFDS) io->fds[io->nfds++] = fd;
                else { close(fd); invalid = 1; }
            }
        }
        if (invalid) { errno = EMSGSIZE; return -1; }
        if (header) io->header_used += (size_t)n;
        else io->body_used += (size_t)n;
    }
}
