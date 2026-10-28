#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "sb_proto.h"

typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef unsigned char u8;

#define INLINE_MAX (256u * 1024u)
#define SB_MAXMSG (8u * 1024u * 1024u)

#define T_BINDER        0x73622a85u
#define T_WEAK_BINDER   0x77622a85u
#define T_HANDLE        0x73682a85u
#define T_WEAK_HANDLE   0x77682a85u
#define T_FD            0x66642a85u
#define T_FDA           0x66646185u
#define T_PTR           0x70742a85u

struct fbo { u32 type, flags; u64 binder, cookie; };

struct proc; struct thread; struct node;

enum { W_TXN = 1, W_COMPLETE, W_NODE, W_DEAD_BINDER, W_CLEAR_DONE, W_ERROR, W_ACK };

struct work { struct work *next; int type; struct txn *t; struct node *node; u64 cookie; u32 err; u32 ack_type; };
struct wq { struct work *head, *tail; };

struct ref {
    struct proc *proc;
    u32 desc;
    struct node *node;
    int strong, weak;
    int has_death;
    u64 death;
    struct ref *next_in_node;
};

struct node {
    struct proc *owner;
    u64 ptr, cookie;
    int internal_strong, local_strong, local_weak;
    int nrefs, has_strong, has_weak, queued, has_async;
    int pend_strong, pend_weak;
    struct ref *refs;
    struct wq async;
    struct node *hnext;
};

struct held { struct ref *ref; struct node *node; int strong; };

struct dbuf {
    struct dbuf *prev, *next;
    struct proc *proc;
    struct node *async_node;
    int nheld;
    struct held *held;
};

struct txn {
    struct thread *from, *to_thread;
    struct proc *to_proc;
    struct txn *from_parent, *to_parent;
    struct node *target;
    u32 code, flags;
    int reply, need_reply, sender_pid;
    u32 sender_euid;
    u64 data_size, offsets_size;
    char *data;
    int memfd;
    int nfds, *fds;
    int nheld;
    struct held *held;
};

struct thread {
    int fd, tid, is_main, waiting, nonblock, looper, woken;
    u32 read_size;
    struct proc *proc;
    struct wq todo;
    struct txn *stack;
    struct thread *next;
    u32 iseq;
};

#define NHASH 256

struct proc {
    int pid, max_threads, requested, started, kick;
    int internal_ctx;
    u32 key;
    struct thread *threads;
    struct wq todo;
    struct ref **refs;
    u32 nrefs;
    struct node *nodes[NHASH];
    struct dbuf *bufs;
    struct proc *next;
};

static struct proc *procs;
static struct node *ctx_mgr;
static struct thread *cur;
static int ep, verbose;

static void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

#define V(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

static void *zalloc(size_t n) {
    void *p = calloc(1, n);
    if (!p) { logf_("sensesbinderd: out of memory"); exit(1); }
    return p;
}

static void wq_push(struct wq *q, struct work *w) {
    w->next = 0;
    if (q->tail) q->tail->next = w; else q->head = w;
    q->tail = w;
}

static struct work *wq_pop(struct wq *q) {
    struct work *w = q->head;
    if (w) { q->head = w->next; if (!q->head) q->tail = 0; }
    return w;
}

static struct work *mkwork(int type) {
    struct work *w = zalloc(sizeof *w);
    w->type = type;
    return w;
}

static void deliver(struct thread *th);
static void kick_proc(struct proc *p);
static void node_work(struct node *n) {
    if (!n || !n->owner) return;
    if (n->has_strong && !n->has_weak) return;
    if (n->queued) return;
    if (n->internal_strong > 0 || n->pend_strong > 0 || n->pend_weak > 0) return;
    if (!n->has_strong && !n->has_weak) return;
    if (n->local_strong > 0 || n->local_weak > 0) return;
    if (!n->owner->threads) return;
    struct work *w = mkwork(W_NODE);
    w->node = n;
    n->queued = 1;
    wq_push(&n->owner->todo, w);
    kick_proc(n->owner);
}

static void node_free_if_unused(struct node *n) {
    if (!n) return;
    if (n->nrefs > 0 || n->internal_strong > 0 || n->local_strong > 0 || n->local_weak > 0) return;
    if (n->has_strong || n->has_weak || n->pend_strong || n->pend_weak) return;
    if (n->queued || n->has_async) return;
    if (n->owner) {
        u32 i = (u32)((n->ptr >> 4) & (NHASH - 1));
        struct node **pp = &n->owner->nodes[i];
        while (*pp) { if (*pp == n) { *pp = n->hnext; break; } pp = &(*pp)->hnext; }
    }
    if (ctx_mgr == n) ctx_mgr = 0;
    free(n);
}

static struct node *node_get(struct proc *p, u64 ptr, u64 cookie, int create) {
    u32 i = (u32)((ptr >> 4) & (NHASH - 1));
    struct node *n = p->nodes[i];
    while (n) { if (n->ptr == ptr && n->cookie == cookie) return n; n = n->hnext; }
    if (!create) return 0;
    n = zalloc(sizeof *n);
    n->owner = p;
    n->ptr = ptr;
    n->cookie = cookie;
    n->hnext = p->nodes[i];
    p->nodes[i] = n;
    return n;
}

static void ref_add_node(struct ref *r, struct node *n) {
    r->node = n;
    r->next_in_node = n->refs;
    n->refs = r;
    n->nrefs++;
}

static struct ref *ref_for_node(struct proc *p, struct node *n) {
    for (struct ref *r = n->refs; r; r = r->next_in_node) if (r->proc == p) return r;
    if (p->nrefs == 0) {
        p->refs = zalloc(sizeof(struct ref *) * 64);
        p->nrefs = 64;
        for (u32 i = 0; i < p->nrefs; i++) {
            p->refs[i] = zalloc(sizeof(struct ref));
            p->refs[i]->proc = p;
            p->refs[i]->desc = i;
        }
    }
    struct ref *free_ref = 0;
    for (u32 i = 1; i < p->nrefs; i++) {
        struct ref *c = p->refs[i];
        if (!c->node) { free_ref = c; break; }
    }
    if (!free_ref) {
        u32 old = p->nrefs;
        p->nrefs *= 2;
        p->refs = realloc(p->refs, sizeof(struct ref *) * p->nrefs);
        for (u32 i = old; i < p->nrefs; i++) {
            p->refs[i] = zalloc(sizeof(struct ref));
            p->refs[i]->proc = p;
            p->refs[i]->desc = i;
        }
        free_ref = p->refs[old];
    }
    ref_add_node(free_ref, n);
    return free_ref;
}

static struct ref *ref_get(struct proc *p, u32 desc) {
    if (desc == 0 || desc >= p->nrefs) return 0;
    struct ref *r = p->refs[desc];
    return r->node ? r : 0;
}

static void ref_delete(struct ref *r) {
    if (!r->node) return;
    struct node *n = r->node;
    struct ref **pp = &n->refs;
    while (*pp) { if (*pp == r) { *pp = r->next_in_node; break; } pp = &(*pp)->next_in_node; }
    n->nrefs--;
    r->node = 0;
    r->strong = r->weak = 0;
    r->has_death = 0;
    r->death = 0;
    r->next_in_node = 0;
    node_free_if_unused(n);
}

static void ref_inc(struct ref *r, int strong) {
    if (strong) r->strong++; else r->weak++;
}

static void ref_dec(struct ref *r, int strong) {
    if (strong) { if (r->strong > 0) r->strong--; }
    else { if (r->weak > 0) r->weak--; }
    if (!r->strong && !r->weak) ref_delete(r);
    else if (!r->strong) node_free_if_unused(r->node);
}

static void held_release(struct held *h, int n) {
    for (int i = 0; i < n; i++) {
        if (h[i].ref) ref_dec(h[i].ref, h[i].strong);
        if (h[i].node) {
            if (h[i].strong) { if (h[i].node->local_strong > 0) h[i].node->local_strong--; }
            else { if (h[i].node->local_weak > 0) h[i].node->local_weak--; }
            node_free_if_unused(h[i].node);
        }
    }
    free(h);
}

static void txn_free(struct txn *t) {
    if (!t) return;
    if (t->held) held_release(t->held, t->nheld);
    if (t->fds) { for (int i = 0; i < t->nfds; i++) close(t->fds[i]); free(t->fds); }
    if (t->memfd >= 0) close(t->memfd);
    free(t->data);
    free(t);
}

static struct thread *ready_thread(struct proc *p) {
    for (struct thread *th = p->threads; th; th = th->next)
        if (th->waiting && !th->stack && !th->todo.head) return th;
    return 0;
}

static void kick_proc(struct proc *p) {
    struct thread *th;
    while (p->todo.head && (th = ready_thread(p))) deliver(th);
    if (p->todo.head) for (th = p->threads; th; th = th->next) if (!th->waiting) deliver(th);
}

static void queue_txn(struct txn *t, struct thread *target) {
    struct work *w = mkwork(W_TXN);
    w->t = t;
    if (target) { wq_push(&target->todo, w); deliver(target); }
    else { wq_push(&t->to_proc->todo, w); kick_proc(t->to_proc); }
}static int translate(struct thread *th, struct proc *to, struct txn *t, char *data) {
    u64 n = t->offsets_size / 8, *offs = (u64 *)(data + ((t->data_size + 7) & ~7UL));
    struct proc *p = th->proc;
    t->held = zalloc((n + 2) * sizeof *t->held);
    int fdi = 0;
    for (u64 i = 0; i < n; i++) {
        if (offs[i] + sizeof(struct fbo) > t->data_size) return -1;
        struct fbo *o = (struct fbo *)(data + offs[i]);
        struct held *h = &t->held[t->nheld];
        switch (o->type) {
        case T_BINDER: case T_WEAK_BINDER: {
            struct node *nd = node_get(p, o->binder, o->cookie, 1);
            struct ref *r = ref_for_node(to, nd);
            int strong = o->type == T_BINDER;
            o->type = strong ? T_HANDLE : T_WEAK_HANDLE; o->binder = r->desc; o->cookie = 0;
            ref_inc(r, strong); h->ref = r; h->strong = strong; t->nheld++;
            break;
        }
        case T_HANDLE: case T_WEAK_HANDLE: {
            struct ref *r = ref_get(p, (u32)o->binder);
            int strong = o->type == T_HANDLE;
            if (!r) return -1;
            if (r->node->owner == to) {
                o->type = strong ? T_BINDER : T_WEAK_BINDER; o->binder = r->node->ptr; o->cookie = r->node->cookie;
                if (strong) { r->node->local_strong++; node_work(r->node); h->node = r->node; h->strong = 1; t->nheld++; }
            } else {
                struct ref *nr = ref_for_node(to, r->node);
                o->binder = nr->desc; o->cookie = 0;
                ref_inc(nr, strong); h->ref = nr; h->strong = strong; t->nheld++;
            }
            break;
        }
        case T_FD:
            if (fdi >= t->nfds) return -1;
            fdi++;
            break;
        default: return -1;
        }
    }
    return 0;
}

static void proc_release(struct proc *p);

static void do_transact(struct thread *th, struct sb_txn *m, char *payload, int *fds, int nfds, int reply) {
    struct proc *p = th->proc;
    struct txn *t = zalloc(sizeof *t);
    t->memfd = -1;
    t->code = m->code; t->flags = m->flags; t->reply = reply;
    t->sender_pid = p->pid; t->sender_euid = m->sender_euid;
    t->data_size = m->data_size; t->offsets_size = m->offsets_size;
    u64 total = ((t->data_size + 7) & ~7UL) + t->offsets_size;
    if (total > SB_MAXMSG) { free(t); return; }
    t->data = malloc(total ? total : 1);
    if (total) memcpy(t->data, payload, total);
    if (nfds) { t->fds = malloc(nfds * sizeof(int)); memcpy(t->fds, fds, nfds * sizeof(int)); t->nfds = nfds; }

    struct thread *target_thread = 0;
    struct proc *to = 0;
    struct txn *in_reply_to = 0;
    if (reply) {
        in_reply_to = th->stack;
        if (!in_reply_to) { txn_free(t); return; }
        th->stack = in_reply_to->from_parent;
        target_thread = in_reply_to->from;
        if (!target_thread) { txn_free(t); txn_free(in_reply_to); return; }
        to = target_thread->proc;
    } else {
        struct node *n;
        if (m->handle) { struct ref *r = ref_get(p, m->handle); n = r ? r->node : 0; }
        else n = ctx_mgr;
        if (!n || !n->owner) { txn_free(t); return; }
        t->target = n;
        to = n->owner;
    }
    t->to_proc = to;
    if (!reply) V("txn pid %d -> pid %d handle %u code %u flags %#x data %llu",
                  p->pid, to->pid, m->handle, m->code, m->flags, (unsigned long long)m->data_size);
    if (translate(th, to, t, t->data) < 0) { txn_free(t); return; }
    if (t->target) {
        t->target->local_strong++;
        node_work(t->target);
        t->held[t->nheld].node = t->target;
        t->held[t->nheld++].strong = 1;
    }
    t->need_reply = !reply && !(t->flags & TF_ONE_WAY);
    if (t->need_reply) {
        t->from = th;
        t->from_parent = th->stack;
        th->stack = t;
    }
    queue_txn(t, target_thread);
    // the sender is told that the transaction was taken in, so it can leave its ioctl and free the parcel:
    // without this the guest thread sits waiting for BR_TRANSACTION_COMPLETE forever (see SB_R_ACK)
    if (!reply) {
        struct work *done = mkwork(W_ACK);
        wq_push(&th->todo, done);
        deliver(th);
    }
    if (reply && in_reply_to) { in_reply_to->from = 0; txn_free(in_reply_to); }
}

static int emit_node(struct node *n, u32 *br) {
    int k = 0;
    int strong = n->internal_strong || n->local_strong;
    int weak = n->nrefs || n->local_weak || strong;
#define PUT(c) do { br[k] = (c); memcpy(br + k + 1, &n->ptr, 8); memcpy(br + k + 3, &n->cookie, 8); k += 5; } while (0)
    if (weak && !n->has_weak) { PUT(BR_INCREFS); n->has_weak = 1; n->pend_weak = 1; n->local_weak++; }
    if (strong && !n->has_strong) { PUT(BR_ACQUIRE); n->has_strong = 1; n->pend_strong = 1; n->local_strong++; }
    if (!strong && n->has_strong) { PUT(BR_RELEASE); n->has_strong = 0; }
    if (!weak && n->has_weak) { PUT(BR_DECREFS); n->has_weak = 0; }
#undef PUT
    return k;
}

static int deliverable(struct thread *th) {
    return th->todo.head || (!th->stack && th->proc->todo.head);
}static int wr_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = EPIPE; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int sb_send(struct thread *th, u32 type, const void *body, u32 blen, const int *fds, int nfds, u32 flags) {
    struct sb_hdr h;
    memset(&h, 0, sizeof h);
    h.len = (u32)(sizeof h + blen);
    h.type = (u16)type;
    h.nfds = (u16)nfds;
    h.tid = ++th->iseq;
    h.flags = SB_F_MAGIC | flags;
    if (nfds > 0) {
        struct iovec iov[1] = { { &h, sizeof h } };
        char cbuf[CMSG_SPACE(sizeof(int) * SB_MAXFDS)];
        struct msghdr m;
        memset(&m, 0, sizeof m);
        memset(cbuf, 0, sizeof cbuf);
        m.msg_iov = iov; m.msg_iovlen = 1;
        m.msg_control = cbuf;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfds);
        ssize_t n;
        do { n = sendmsg(th->fd, &m, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
        if (n != (ssize_t)sizeof h) return -1;
        if (!blen) return 0;
    } else {
        if (wr_all(th->fd, &h, sizeof h) < 0) return -1;
        if (!blen) return 0;
    }
    return wr_all(th->fd, body, blen);
}

static void deliver(struct thread *th) {
    if (!deliverable(th)) return;
    struct proc *p = th->proc;

    struct node *nodework[16];
    int nnodes = 0;
    struct txn *t = 0;
    u32 err = 0;
    int have_err = 0;
    u64 dead_cookie = 0;
    int have_dead = 0, have_clear = 0;
    u32 ack = 0;

    for (;;) {
        struct wq *q = th->todo.head ? &th->todo : &p->todo;
        struct work *w = wq_pop(q);
        if (!w) break;
        switch (w->type) {
        case W_COMPLETE:
        case W_ACK: ack++; break;
        case W_ERROR: err = w->err; have_err = 1; break;
        case W_NODE:
            w->node->queued = 0;
            if (nnodes < 16) nodework[nnodes++] = w->node;
            break;
        case W_DEAD_BINDER: dead_cookie = w->cookie; have_dead = 1; break;
        case W_CLEAR_DONE: dead_cookie = w->cookie; have_clear = 1; break;
        case W_TXN: t = w->t; break;
        }
        free(w);
        if (t || have_dead || have_clear || have_err) break;
    }

    if (have_err) { sb_send(th, SB_R_ERROR, &err, 4, 0, 0, 0); kick_proc(p); return; }
    if (have_dead) { sb_send(th, SB_R_DEAD_BINDER, &dead_cookie, 8, 0, 0, 0); return; }
    if (have_clear) { sb_send(th, SB_R_CLEAR_DEATH_DONE, &dead_cookie, 8, 0, 0, 0); return; }

    for (int i = 0; i < nnodes; i++) {
        u32 br[32];
        int k = emit_node(nodework[i], br);
        if (k > 0) {
            struct sb_ptr_cookie pc[8];
            int npc = 0;
            for (int j = 0; j + 5 <= k && npc < 8; j += 5) {
                u32 cmd = br[j];
                pc[npc].op = cmd == BR_INCREFS ? SB_REF_INCREFS :
                            cmd == BR_ACQUIRE ? SB_REF_ACQUIRE :
                            cmd == BR_RELEASE ? SB_REF_RELEASE : SB_REF_DECREFS;
                pc[npc].pad = 0;
                memcpy(&pc[npc].ptr, br + j + 1, 8);
                memcpy(&pc[npc].cookie, br + j + 3, 8);
                npc++;
            }
            if (npc) sb_send(th, SB_R_REF, pc, (u32)(npc * sizeof(struct sb_ptr_cookie)), 0, 0, 0);
        }
        node_free_if_unused(nodework[i]);
    }

    if (ack) sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0);

    if (t) {
        struct sb_txn m;
        memset(&m, 0, sizeof m);
        m.code = t->code;
        m.flags = t->flags;
        m.sender_pid = (u32)t->sender_pid;
        m.sender_euid = t->sender_euid;
        m.data_size = t->data_size;
        m.offsets_size = t->offsets_size;
        if (t->target) { m.node_ptr = t->target->ptr; m.node_cookie = t->target->cookie; }
        u64 total = ((t->data_size + 7) & ~7UL) + t->offsets_size;
        u8 *buf = malloc(sizeof m + (size_t)total + 8);
        memcpy(buf, &m, sizeof m);
        if (total) memcpy(buf + sizeof m, t->data, (size_t)total);
        if (t->target) {
            struct dbuf *b = zalloc(sizeof *b);
            b->proc = p;
            b->held = t->held;
            b->nheld = t->nheld;
            t->held = 0;
            t->nheld = 0;
            b->next = p->bufs;
            if (p->bufs) p->bufs->prev = b;
            p->bufs = b;
        }
        sb_send(th, t->reply ? SB_R_REPLY : SB_R_TRANSACTION, buf,
                (u32)(sizeof m + total), t->fds, t->nfds, 0);
        free(buf);
        if (t->need_reply) {
            t->to_parent = th->stack;
            t->to_thread = th;
            th->stack = t;
            if (t->fds) { for (int i = 0; i < t->nfds; i++) close(t->fds[i]); free(t->fds); t->fds = 0; t->nfds = 0; }
            free(t->data);
            t->data = 0;
        } else {
            txn_free(t);
        }
    }
    kick_proc(p);
}

static void thread_error(struct thread *th, u32 err) {
    struct work *w = mkwork(W_ERROR);
    w->err = err;
    wq_push(&th->todo, w);
    deliver(th);
}

static void proc_release(struct proc *p) {
    struct proc **pp = &procs;
    while (*pp) { if (*pp == p) { *pp = p->next; break; } pp = &(*pp)->next; }
    for (struct thread *th = p->threads, *nx; th; th = nx) {
        nx = th->next;
        if (th->fd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, th->fd, 0); close(th->fd); }
        free(th);
    }
    for (u32 i = 0; i < NHASH; i++) {
        struct node *n = p->nodes[i];
        while (n) {
            struct node *nx = n->hnext;
            n->owner = 0;
            for (struct ref *r = n->refs; r; r = r->next_in_node) r->node = 0;
            n->refs = 0;
            n->nrefs = 0;
            n->has_strong = n->has_weak = n->queued = n->has_async = 0;
            n->internal_strong = n->local_strong = n->local_weak = 0;
            free(n);
            n = nx;
        }
    }
    for (struct dbuf *b = p->bufs, *nx; b; b = nx) {
        nx = b->next;
        if (b->held) held_release(b->held, b->nheld);
        free(b);
    }
    struct work *w;
    while ((w = wq_pop(&p->todo))) { if (w->t) txn_free(w->t); free(w); }
    free(p);
}

static void do_ref(struct thread *th, struct sb_ref *m) {
    struct proc *p = th->proc;
    struct ref *r = ref_get(p, m->handle);
    if (!r) return;
    switch (m->op) {
    case SB_REF_INCREFS: ref_inc(r, 0); break;
    case SB_REF_ACQUIRE: ref_inc(r, 1); break;
    case SB_REF_RELEASE:
    case SB_REF_RELEASE_BUF: ref_dec(r, 1); break;
    case SB_REF_DECREFS:
    case SB_REF_DECREFS_BUF: ref_dec(r, 0); break;
    }
    node_work(r->node);
}

static void do_death(struct thread *th, struct sb_death *m) {
    struct proc *p = th->proc;
    if (m->op) return;
    struct ref *r = ref_get(p, m->handle);
    if (!r) return;
    r->has_death = 1;
    r->death = m->cookie;
}

static void buf_release(struct proc *p, u64 id) {
    (void)id;
    struct dbuf *b = p->bufs;
    if (!b) return;
    p->bufs = b->next;
    if (b->next) b->next->prev = 0;
    if (b->async_node) {
        struct node *n = b->async_node;
        struct work *w = wq_pop(&n->async);
        if (w) {
            if (n->owner) { wq_push(&n->owner->todo, w); kick_proc(n->owner); }
            else { txn_free(w->t); free(w); }
        } else n->has_async = 0;
    }
    if (b->held) held_release(b->held, b->nheld);
    free(b);
}

static uint8_t *rxbuf;
static size_t rxcap;

static void on_message(struct thread *th) {
    if (th->fd < 0) return;
    struct sb_hdr h;
    char cbuf[CMSG_SPACE(sizeof(int) * SB_MAXFDS)];
    struct iovec iov = { &h, sizeof h };
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof cbuf;
    ssize_t n;
    do { n = recvmsg(th->fd, &m, MSG_CMSG_CLOEXEC); } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            if (th->proc && th->is_main) proc_release(th->proc);
            else if (th->proc) {
                struct proc *p = th->proc;
                struct thread **pp = &p->threads;
                while (*pp) { if (*pp == th) { *pp = th->next; break; } pp = &(*pp)->next; }
                epoll_ctl(ep, EPOLL_CTL_DEL, th->fd, 0);
                close(th->fd);
                free(th);
            }
        }
        return;
    }
    if ((size_t)n != sizeof h) return;
    if ((h.flags & SB_F_MAGIC_MASK) != SB_F_MAGIC) {
        logf_("sensesbinderd: ★ out of sync: len=%u type=%u tid=%u flags=%#x", h.len, h.type, h.tid, h.flags);
        return;
    }
    int fds[SB_MAXFDS], nfds = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            nfds = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (nfds > SB_MAXFDS) nfds = SB_MAXFDS;
            memcpy(fds, CMSG_DATA(c), (size_t)nfds * sizeof(int));
        }
    u32 blen = h.len > sizeof h ? h.len - (u32)sizeof h : 0;
    if (blen > SB_MAXMSG) { for (int i = 0; i < nfds; i++) close(fds[i]); return; }
    if (blen > rxcap) {
        uint8_t *p = realloc(rxbuf, blen);
        if (!p) { for (int i = 0; i < nfds; i++) close(fds[i]); return; }
        rxbuf = p;
        rxcap = blen;
    }
    if (blen) {
        size_t got = 0;
        while (got < blen) {
            ssize_t r = read(th->fd, rxbuf + got, blen - got);
            if (r < 0) { if (errno == EINTR) continue; for (int i = 0; i < nfds; i++) close(fds[i]); return; }
            if (r == 0) { for (int i = 0; i < nfds; i++) close(fds[i]); return; }
            got += (size_t)r;
        }
    }
    cur = th;
    switch (h.type) {
    case SB_HELLO: {
        struct sb_hello *m2 = (struct sb_hello *)rxbuf;
        struct proc *p = procs;
        while (p && p->pid != (int)m2->pid) p = p->next;
        if (!p) {
            p = zalloc(sizeof *p);
            p->pid = (int)m2->pid;
            p->key = m2->uid;
            p->max_threads = 16;
            p->next = procs;
            procs = p;
            V("pid %d opened binder (uid %u)", p->pid, m2->uid);
        }
        th->proc = p;
        th->tid = (int)h.tid;
        th->is_main = m2->first == 1;
        th->iseq = h.tid;
        th->next = p->threads;
        p->threads = th;
        sb_send(th, SB_R_OK, 0, 0, 0, 0, 0);
        break;
    }
    case SB_SET_CTX_MGR: {
        struct proc *p = th->proc;
        if (p) {
            // the context manager is reached as handle 0, so it is not a node of any client's table:
            // it belongs to the daemon and is handed out for every transaction that names no handle
            if (!ctx_mgr) {
                ctx_mgr = zalloc(sizeof *ctx_mgr);
                ctx_mgr->owner = p;
                ctx_mgr->internal_strong = 1;
            } else {
                ctx_mgr->owner = p;
            }
            p->internal_ctx = 1;
            V("pid %d became the context manager", p->pid);
        }
        sb_send(th, SB_R_OK, 0, 0, 0, 0, 0);
        break;
    }
    case SB_SET_MAX_THREADS: {
        struct proc *p = th->proc;
        if (p && blen >= 4) { u32 v; memcpy(&v, rxbuf, 4); p->max_threads = (int)v; }
        sb_send(th, SB_R_OK, 0, 0, 0, 0, 0);
        break;
    }
    case SB_TRANSACT:
    case SB_REPLY:
        if (blen >= sizeof(struct sb_txn)) {
            struct sb_txn *tx = (struct sb_txn *)rxbuf;
            V(">> %s pid %d handle %u code %u data %llu",
              h.type == SB_REPLY ? "reply" : "transact",
              th->proc ? th->proc->pid : 0, tx->handle, tx->code,
              (unsigned long long)tx->data_size);
            do_transact(th, tx, (char *)rxbuf + sizeof(struct sb_txn), fds, nfds, h.type == SB_REPLY);
        }
        nfds = 0;
        break;
    case SB_REF:
        if (blen >= sizeof(struct sb_ref)) do_ref(th, (struct sb_ref *)rxbuf);
        break;
    case SB_DEATH:
        if (blen >= sizeof(struct sb_death)) do_death(th, (struct sb_death *)rxbuf);
        break;
    case SB_REF_DONE: {
        struct proc *p = th->proc;
        if (p && blen >= sizeof(struct sb_ptr_cookie)) {
            struct sb_ptr_cookie *pc = (struct sb_ptr_cookie *)rxbuf;
            struct node *n = node_get(p, pc->ptr, pc->cookie, 0);
            if (n) {
                if (n->pend_weak > 0) n->pend_weak--;
                if (n->pend_strong > 0) n->pend_strong--;
                node_work(n);
            }
        }
        break;
    }
    case SB_LOOPER: {
        struct proc *p = th->proc;
        if (p && blen >= 4) {
            u32 op;
            memcpy(&op, rxbuf, 4);
            if (op == 0) { th->looper = 1; p->started++; }
            else if (op == 1) th->looper |= 2;
            else { th->looper = 0; }
        }
        break;
    }
    case SB_WAIT:
        th->waiting = 1;
        th->nonblock = 0;
        th->read_size = 256 * 4;
        deliver(th);
        if (th->waiting && th->proc) kick_proc(th->proc);
        break;
    case SB_UNWAIT:
        th->waiting = 0;
        break;
    case SB_BYE: {
        struct proc *p = th->proc;
        if (!p) break;
        th->fd = -1;
        struct thread **pp = &p->threads;
        while (*pp) { if (*pp == th) { *pp = th->next; break; } pp = &(*pp)->next; }
        free(th);
        break;
    }
    case SB_ASYNC_DONE:
        if (th->proc && blen >= 8) { u64 id; memcpy(&id, rxbuf, 8); buf_release(th->proc, id); }
        break;
    case SB_NODE_FREE: {
        struct proc *p = th->proc;
        if (p && blen >= sizeof(struct sb_ptr_cookie)) {
            struct sb_ptr_cookie *pc = (struct sb_ptr_cookie *)rxbuf;
            if (pc->op == 1) {
                struct node *n = node_get(p, pc->ptr, pc->cookie, 0);
                if (n && n->owner) { if (n->local_strong > 0) n->local_strong--; node_free_if_unused(n); }
            }
        }
        break;
    }
    default:
        sb_send(th, SB_R_OK, 0, 0, 0, 0, 0);
        break;
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);
    cur = 0;
}

int main(int argc, char **argv) {
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] != '-') path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: sensesbinderd -s <socket> [-v]\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stderr, 0, _IOLBF, 0);
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
    unlink(path);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(ls, 128) < 0) {
        perror("sensesbinderd: bind");
        return 1;
    }
    ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.ptr = 0;
    epoll_ctl(ep, EPOLL_CTL_ADD, ls, &ev);
    logf_("sensesbinderd: listening on %s", path);
    for (;;) {
        struct epoll_event evs[64];
        int n = epoll_wait(ep, evs, 64, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; i++) {
            if (!evs[i].data.ptr) {
                int fd = accept4(ls, 0, 0, SOCK_CLOEXEC);
                if (fd < 0) continue;
                struct thread *th = zalloc(sizeof *th);
                th->fd = fd;
                memset(&ev, 0, sizeof ev);
                ev.events = EPOLLIN;
                ev.data.ptr = th;
                epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
            } else {
                on_message((struct thread *)evs[i].data.ptr);
                break;
            }
        }
    }
    return 0;
}