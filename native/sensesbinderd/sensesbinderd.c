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

// The guest is 32-bit, so this is 16 bytes, not 24: binder_uintptr_t is a uint32_t there, and
// both binder/handle and cookie are four bytes each. inflating it made every offset check fail
// on the last object of a parcel and swallowed the whole transaction.
struct fbo { u32 type, flags; u32 binder; u32 cookie; };

struct proc; struct thread; struct node;

enum { W_TXN = 1, W_COMPLETE, W_NODE, W_DEAD_BINDER, W_CLEAR_DONE, W_ERROR, W_ACK, W_SPAWN };

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
    int fd, tid, is_main, is_pump, waiting, nonblock, looper, woken;
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
    /** the guest's alarm thread, if it started one: the only way to reach a process that waits in poll */
    struct thread *pump;
    struct wq todo;
    struct ref **refs;
    u32 nrefs;
    struct node *nodes[NHASH];
    struct dbuf *bufs;
    struct proc *next;
};

static struct proc *procs;
static struct node *ctx_mgr;
/** the thread that made itself the context manager: lookups go to it and to nobody else */
static struct thread *ctx_th;
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
static int sb_send(struct thread *th, u32 type, const void *body, u32 blen, const int *fds, int nfds, u32 flags);
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
    V("   ref drop pid %d desc %u -> node %p of pid %d",
      r->proc ? r->proc->pid : 0, r->desc, (void *)n, n->owner ? n->owner->pid : 0);
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
        if (th->waiting && !th->is_pump && !th->stack && !th->todo.head) return th;
    return 0;
}
static int wr_all(int fd, const void *buf, size_t len);

/**
 * Tell a process that work is waiting. guest_binder.c keeps one connection per process whose only
 * job is to receive this nudge and write a byte into the socket pair behind the guest's binder
 * descriptor; the guest sees POLLIN and comes into ioctl to collect the transaction itself.
 *
 * Without it a process that waits in poll — surfaceflinger's frame loop, anything built on Looper —
 * is unreachable: the daemon only ever wrote to threads parked in SB_WAIT, so a transaction
 * addressed to such a process sat in the queue and the sender waited forever.
 */
/**
 * Ask a process to start another binder thread, the way the real driver does when it runs out of
 * idle ones (BR_SPAWN_LOOPER). A process that has one worker thread and lost it — or whose only
 * worker is stuck in a call of its own — otherwise keeps a queued transaction forever with nobody
 * able to pick it up.
 */
static void ask_for_thread(struct proc *p) {
    if (p->requested - p->started > 0) return;      // already asked, still waiting for it to come up
    if (p->started >= p->max_threads) return;
    p->requested++;
    // Queue it like any other piece of work. BR_SPAWN_LOOPER only exists while libbinder reads it
    // out of ioctl, so it has to reach a worker, not the alarm thread.
    struct work *w = mkwork(W_SPAWN);
    wq_push(&p->todo, w);
    V("   ask pid %d for another binder thread (%d/%d)", p->pid, p->started, p->max_threads);
}

static void wake_proc(struct proc *p) {
    if (!p->pump || p->pump->fd < 0) return;
    struct sb_hdr h;
    memset(&h, 0, sizeof h);
    h.len = sizeof h;
    // the alarm thread does not look at the type, it just needs a well-formed header to know it is
    // a real message and not a desynchronised stream
    h.type = SB_R_OK;
    h.tid = (u32)p->pump->tid;
    h.flags = SB_F_MAGIC;
    V("   wake pid %d (alarm tid %d fd %d)", p->pid, p->pump->tid, p->pump->fd);
    if (wr_all(p->pump->fd, &h, sizeof h) < 0) {
        V("!! could not wake pid %d: %s", p->pid, strerror(errno));
        if (p->pump->fd >= 0) { epoll_ctl(ep, EPOLL_CTL_DEL, p->pump->fd, 0); close(p->pump->fd); }
        p->pump->fd = -1;
        p->pump = 0;
    }
}

static void kick_proc(struct proc *p) {
    struct thread *th;
    while (p->todo.head && (th = ready_thread(p))) deliver(th);
    // Nothing parked in SB_WAIT and the queue is still not empty: ring the alarm. A process whose
    // main thread only runs a frame loop (surfaceflinger) never blocks in ioctl, so writing to a
    // thread is not an option — the alarm thread turns this into POLLIN on the guest's binder
    // descriptor and the guest comes to collect the transaction itself.
    if (p->todo.head) {
        for (struct thread *d = p->threads; d; d = d->next)
            V("   stuck: tid %d fd %d%s waiting=%d stack=%s todo=%s", d->tid, d->fd,
              d->is_pump ? " (alarm)" : d->is_main ? " (main)" : "",
              d->waiting, d->stack ? "yes" : "no", d->todo.head ? "yes" : "no");
        ask_for_thread(p);
        wake_proc(p);
    }
    // Only a thread that said SB_WAIT is reading the socket. A thread that merely opened a connection
    // and never waited is not: whatever lands on it sits in the buffer and nothing consumes it. That
    // is how the third lookup went to servicemanager's second connection (tid 943) while its main
    // thread (tid 921) sat waiting on the first, and the whole boot stopped there.
    if (p->todo.head) {
        for (th = p->threads; th; th = th->next)
            if (!th->waiting && th->looper) { deliver(th); break; }
    }
}
static struct thread *main_thread_of(struct proc *p) {
    // the thread that became the context manager is the one that serves lookups: it is the only one
    // whose sb_wait the daemon can answer, and picking any other member of the process leaves the
    // transaction in a socket nobody reads
    if (ctx_th && ctx_th->proc == p) return ctx_th;
    for (struct thread *th = p->threads; th; th = th->next) if (th->is_main) return th;
    for (struct thread *th = p->threads; th; th = th->next) if (th->waiting) return th;
    return 0;
}

static void queue_txn(struct txn *t, struct thread *target) {
    struct work *w = mkwork(W_TXN);
    w->t = t;
    if (target) { wq_push(&target->todo, w); deliver(target); return; }
    if (t->target && t->target == ctx_mgr) {
        struct thread *main_th = main_thread_of(t->to_proc);
        if (main_th) { wq_push(&main_th->todo, w); deliver(main_th); return; }
    }
    wq_push(&t->to_proc->todo, w);
    kick_proc(t->to_proc);
}

static int translate(struct thread *th, struct proc *to, struct txn *t, char *data) {
    // The guest's own offset table is a run of four-byte entries, but guest_binder.c widens it to
// eight at the protocol boundary ("пересчитываем на границе", noff * 8), so inside this message
// the entries really are uint64_t. What was wrong was never the width: it was where the table
// starts. The table sits immediately after data_size, with no padding, and the align-to-eight
// this code used to apply shifted the whole read by a few bytes, which made the last object of
// every parcel look out of bounds and got the transaction dropped.
    u64 n = t->offsets_size / 8;
    const u64 *offs = (const u64 *)(data + t->data_size);
    struct proc *p = th->proc;
    t->held = zalloc((n + 2) * sizeof *t->held);
    int fdi = 0;
    if (n) V("   translate code %u: %llu offsets, data %llu, at %llu/%llu", t->code,
             (unsigned long long)n, (unsigned long long)t->data_size,
             (unsigned long long)offs[0], n > 1 ? (unsigned long long)offs[1] : 0ULL);
    for (u64 i = 0; i < n; i++) {
        if (offs[i] + sizeof(struct fbo) > t->data_size) {
            V("   !! offset %llu out of %llu", (unsigned long long)offs[i], (unsigned long long)t->data_size);
            return -1;
        }
        struct fbo *o = (struct fbo *)(data + offs[i]);
        struct held *h = &t->held[t->nheld];
        switch (o->type) {
        case T_BINDER: case T_WEAK_BINDER: {
            struct node *nd = node_get(p, o->binder, o->cookie, 1);
            struct ref *r = ref_for_node(to, nd);
            int strong = o->type == T_BINDER;
            o->type = strong ? T_HANDLE : T_WEAK_HANDLE; o->binder = (u32)r->desc; o->cookie = 0;
            ref_inc(r, strong); h->ref = r; h->strong = strong; t->nheld++;
            break;
        }
        case T_HANDLE: case T_WEAK_HANDLE: {
            struct ref *r = ref_get(p, o->binder);
            int strong = o->type == T_HANDLE;
            if (!r) return -1;
            if (r->node->owner == to) {
                o->type = strong ? T_BINDER : T_WEAK_BINDER; o->binder = (u32)r->node->ptr; o->cookie = (u32)r->node->cookie;
                if (strong) { r->node->local_strong++; node_work(r->node); h->node = r->node; h->strong = 1; t->nheld++; }
            } else {
                struct ref *nr = ref_for_node(to, r->node);
                o->binder = (u32)nr->desc; o->cookie = 0;
                ref_inc(nr, strong); h->ref = nr; h->strong = strong; t->nheld++;
            }
            break;
        }
        case T_FD:
            // The guest sends the raw descriptor here, but the receiver is told a position in the
            // SCM_RIGHTS array that travels with the message: guest_binder.c reads it back as
            // fds[fo->handle]. Leaving the raw number in place made every lookup miss and every
            // heap come back as -1, which is what killed system_server on its first IMemoryHeap.
            if (fdi >= t->nfds) return -1;
            o->binder = (u32)fdi;
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
    // no padding between the parcel and its offset table: the guest writes them back to back
    // and reads them back the same way, so rounding up here desynchronised the two sides
    u64 total = t->data_size + t->offsets_size;
    if (total > SB_MAXMSG) { free(t); return; }
    t->data = malloc(total ? total : 1);
    if (total) memcpy(t->data, payload, total);
    if (nfds) { t->fds = malloc(nfds * sizeof(int)); memcpy(t->fds, fds, nfds * sizeof(int)); t->nfds = nfds; }

    struct thread *target_thread = 0;
    struct proc *to = 0;
    struct txn *in_reply_to = 0;
    if (reply) {
        in_reply_to = th->stack;
        V("   reply from pid %d: stack=%s", p->pid, in_reply_to ? "yes" : "EMPTY");
        if (!in_reply_to) { sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0); txn_free(t); return; }
        // Two frames were pushed for this call: one on the caller, when the transaction went out,
        // and one on whoever received it, when it was handed over. Only the receiver's frame was
        // being popped here, and with the caller's parent pointer on top of that, so every call
        // left a frame behind on the caller and the thread never counted as idle again. Pop both,
        // each against its own parent.
        th->stack = in_reply_to->to_parent;
        if (in_reply_to->from && in_reply_to->from != th)
            in_reply_to->from->stack = in_reply_to->from_parent;
        V("   stack pop on tid %d: txn %p code %u (now %s), caller tid %d now %s",
          th->tid, (void *)in_reply_to, in_reply_to->code, th->stack ? "non-empty" : "empty",
          in_reply_to->from ? in_reply_to->from->tid : -1,
          (in_reply_to->from && in_reply_to->from->stack) ? "non-empty" : "empty");
        target_thread = in_reply_to->from;
        if (!target_thread) { txn_free(t); txn_free(in_reply_to); return; }
        to = target_thread->proc;
    } else {
        struct node *n;
        if (m->handle) { struct ref *r = ref_get(p, m->handle); n = r ? r->node : 0; }
        else n = ctx_mgr;
        if (!n || !n->owner) {
        V("   !! no target for handle %u (node %p owner %p)", m->handle, (void *)n, n ? (void *)n->owner : 0);
        sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0); txn_free(t); return;
    }
        t->target = n;
        to = n->owner;
    }
    t->to_proc = to;
    if (!reply) V("txn pid %d -> pid %d handle %u code %u flags %#x data %llu",
                  p->pid, to->pid, m->handle, m->code, m->flags, (unsigned long long)m->data_size);
    if (translate(th, to, t, t->data) < 0) {
        V("   !! translate failed for code %u", m->code);
        sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0); txn_free(t); return;
    }
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
        V("   stack push on tid %d: txn %p code %u from pid %d (depth %d)",
          th->tid, (void *)t, t->code, p->pid,
          th->stack->from_parent ? 2 : 1);
    }
    // the sender is told that its message was taken in, so it can leave the ioctl and free the parcel.
    // The guest counts one acceptance for every command it writes — a transaction and a reply alike
    // (guest_binder.c: t->acks++ right after the send) — and waits in SB_WAIT until the count drops.
    // It has to leave before the transaction itself: libbinder expects the acceptance first, and a
    // transaction that overtakes it makes the guest read its own answer as somebody else's.
    sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0);
    V("   -> pid %d/%d fd %d: ack (%s)", p->pid, th->tid, th->fd, reply ? "reply" : "transact");
    queue_txn(t, target_thread);
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
    // A reply addressed to this thread carries the answer to the call sitting on its stack: the
    // stack is expected to be non-empty, and refusing it there was what kept every reply undelivered
    // and every stack uncleared. Work from the shared queue still needs an idle thread.
    if (th->todo.head) return 1;
    return !th->stack && th->proc->todo.head;
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
    th->waiting = 0;

    struct node *nodework[16];
    int nnodes = 0;
    struct txn *t = 0;
    u32 err = 0;
    int have_err = 0;
    u64 dead_cookie = 0;
    int have_dead = 0, have_clear = 0;
    u32 ack = 0;
    int have_spawn = 0;

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
        case W_SPAWN: have_spawn = 1; break;
        }
        free(w);
        if (t || have_dead || have_clear || have_err || have_spawn) break;
    }

    if (have_spawn) {
        // BR_SPAWN_LOOPER on the guest side: libbinder answers it by starting another pool thread
        V("   -> pid %d/%d fd %d: spawn looper", p->pid, th->tid, th->fd);
        sb_send(th, SB_R_SPAWN, 0, 0, 0, 0, 0);
        kick_proc(p);
        return;
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

    if (ack) { V("   -> pid %d: %u ack (queued)", p->pid, ack); sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0); }

    if (t) {
        struct sb_txn m;
        memset(&m, 0, sizeof m);
        V("<< %s -> pid %d code %u stack %s", t->reply ? "reply" : "txn", p->pid, t->code, th->stack ? "busy" : "idle");
        m.code = t->code;
        m.flags = t->flags;
        m.sender_pid = (u32)t->sender_pid;
        m.sender_euid = t->sender_euid;
        m.data_size = t->data_size;
        m.offsets_size = t->offsets_size;
        if (t->target) { m.node_ptr = t->target->ptr; m.node_cookie = t->target->cookie; }
        // no padding between the parcel and its offset table: the guest writes them back to back
    // and reads them back the same way, so rounding up here desynchronised the two sides
    u64 total = t->data_size + t->offsets_size;
        u8 *buf = malloc(sizeof m + (size_t)total + 8);
        memcpy(buf, &m, sizeof m);
        if (total) memcpy(buf + sizeof m, t->data, (size_t)total);
        // Everything the translation handed to the receiver has to outlive this message: the guest
        // owns those references now and gives them back through BC_FREE_BUFFER. A reply carries
        // objects too (that is how a lookup returns a binder), and it has no target node, so keying
        // this off t->target meant a reply's references were dropped the instant it was sent — the
        // receiver got a handle that no longer resolved. Keep the whole buffer alive for the guest
        // whenever the message carried anything, and let txn_free handle only what is left.
        if (t->nheld || t->nfds || t->target) {
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
        u32 more_flag = (th->todo.head || p->todo.head) ? SB_F_MORE : 0;
        V("   send %s to pid %d/%d fd %d: %u bytes, more=%u",
          t->reply ? "reply" : "txn", p->pid, th->tid, th->fd,
          (u32)(sizeof m + total), more_flag);
        if (sb_send(th, t->reply ? SB_R_REPLY : SB_R_TRANSACTION, buf,
                    (u32)(sizeof m + total), t->fds, t->nfds, more_flag) < 0)
            V("!! send to pid %d failed: %s", p->pid, strerror(errno));
        free(buf);
        if (t->need_reply) {
            t->to_parent = th->stack;
            t->to_thread = th;
            th->stack = t;
            V("   stack push on tid %d: txn %p code %u for receiver (depth %d)",
              th->tid, (void *)t, t->code, t->to_parent ? 2 : 1);
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
        // first == 2 is the guest's alarm thread: it never carries work itself, it exists so the
        // daemon has somewhere to say "there is something for you". Every process that waits in
        // poll instead of a blocking ioctl needs one, otherwise transactions pile up in the queue
        // forever — which is exactly what happened to surfaceflinger, whose main thread only runs
        // the frame loop.
        th->is_pump = m2->first == 2;
        if (th->is_pump) { p->pump = th; V("pid %d has an alarm thread (tid %d)", p->pid, th->tid); }
        else if (!th->is_main) {
            // count the workers the guest actually brought up, so ask_for_thread() knows whether
            // the request it sent is still outstanding
            if (p->started < p->requested) p->started++;
            if (p->started < 1) p->started = 1;
        } else if (p->started < 1) p->started = 1;
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
            ctx_th = th;
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
            V(">> %s pid %d/%d handle %u code %u data %llu",
              h.type == SB_REPLY ? "reply" : "transact",
              th->proc ? th->proc->pid : 0, th->tid, tx->handle, tx->code,
              (unsigned long long)tx->data_size);
            if (blen > sizeof(*tx) && tx->data_size >= 8) {
                // a servicemanager Parcel starts with the interface token and only then names the
                // service, so skip the first string and read the second: that one is the answer to
                // "what is it looking for"
                const u8 *base = (const u8 *)rxbuf + sizeof(*tx);
                u64 room = (u64)blen - sizeof(*tx), off = 0;
                for (int k = 0; k < 2; k++) {
                    u32 slen = 0;
                    if (off + 4 > room) break;
                    memcpy(&slen, base + off, 4);
                    if (slen > 200 || off + 4 + (u64)slen * 2 > room) break;
                    if (k == 1) {
                        char nm[256]; u32 i;
                        for (i = 0; i < slen && i < 200; i++) {
                            u16 ch; memcpy(&ch, base + off + 4 + i * 2, 2);
                            nm[i] = (ch < 128 && ch >= 32) ? (char)ch : '?';
                        }
                        nm[i] = 0;
                        V("   name \"%s\"", nm);
                    }
                    off += 4 + (u64)slen * 2;
                    off = (off + 3) & ~3ULL;
                }
            }
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
        // Never answer a message we do not understand: a reply the guest did not ask for is read
        // as the answer to whatever it is waiting on, and the real answer then arrives out of step.
        logf_("sensesbinderd: ★ unknown message type %u (%u bytes) — ignored", h.type, blen);
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