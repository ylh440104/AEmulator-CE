#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include "sb_proto.h"
#include "sb_transport.h"

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

/* the thread whose ioctl is being answered (see the definition further down) */
struct thread;
static struct thread *g_cur_th;
static int emit_node(struct node *n, u32 *br);

enum { W_TXN = 1, W_COMPLETE, W_NODE, W_DEAD_BINDER, W_CLEAR_DONE, W_ERROR, W_ACK, W_SPAWN, W_REF };

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
    u32 id;
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
    struct sb_io io;
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
    u32 next_buf;
    struct proc *next;
};

static struct proc *procs;
static struct node *ctx_mgr;
static int g_listen_fd = -1;
/** the thread that made itself the context manager: lookups go to it and to nobody else */
static struct thread *ctx_th;
static struct thread *cur;
static int ep, verbose;
static char g_bind_path[108];

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

#include "sb_transport.inc"

static void deliver(struct thread *th);
static void kick_proc(struct proc *p);
static int sb_send(struct thread *th, u32 type, const void *body, u32 blen, const int *fds, int nfds, u32 flags);
static void node_work(struct node *n) {
    if (!n || !n->owner || !n->ptr) return;
    if (n->queued) return;
    /*
     * The owner only has something to do when what it has been told differs from what is true.
     * Stating it that way covers the case that used to fall through: a node the owner has just
     * published holds its own reference, so there is a BR_ACQUIRE to deliver even though neither
     * flag is set yet. The old chain of early returns saw two zero flags and gave up.
     */
    /*
     * internal_strong mirrors "some handle still points at this node", and nrefs is that
     * count, so the two have to move together. It used to be raised at publish time and
     * never lowered, which left has_strong stuck at 1: the daemon sent 788 BR_ACQUIRE and
     * not a single BR_RELEASE in a whole session, every JavaBBinder kept a strong count it
     * should have given back, and the pool thread later walked a wp<> that had already been
     * cut loose — the fault at libandroid_runtime +0x3688a over a null object.
     */
    int strong = n->local_strong > 0;
    for (struct ref *r = n->refs; r; r = r->next_in_node)
        if (r->strong > 0) strong = 1;
    n->internal_strong = strong;
    int weak = n->nrefs > 0 || n->local_weak > 0 || strong;
    /*
     * A node that lost its last handle still owes the owner a BR_RELEASE, so it must not be
     * skipped here. emit_node turns strong off in that case and sends the release; returning
     * early because the flag happens to match is what kept has_strong stuck at 1 for a whole
     * session.
     */
    if (strong == n->has_strong && weak == n->has_weak && !(n->has_strong && n->nrefs <= 0)) return;
    if (n->pend_strong > 0 || n->pend_weak > 0) return;
    if (!n->owner->threads) return;
    struct work *w = mkwork(W_NODE);
    w->node = n;
    n->queued = 1;
    if (g_cur_th && g_cur_th->proc == n->owner && g_cur_th->fd >= 0)
        wq_push(&g_cur_th->todo, w);
    else
        wq_push(&n->owner->todo, w);
    /*
     * Do not kick the process whose own ioctl is being answered right now.
     *
     * When a process publishes an object and that same call is the one being translated, owner
     * and sender are the same process. Kicking it re-entered the delivery loop on the thread
     * that was still inside translate(): the log ended at "T_BINDER obj=..." with no reply, no
     * acknowledgement and no release path, and system_server's very first addService never
     * reached servicemanager. The work is on the queue already, and that thread leaves the ioctl
     * as soon as translate() returns, so the guest comes back for it by itself.
     */
    if (g_cur_th && g_cur_th->proc == n->owner) return;
    kick_proc(n->owner);
}

static void node_free_if_unused(struct node *n) {
    if (!n) return;
    /*
     * Nodes are not recycled while this image is being brought up.
     *
     * A handle the guest still holds must keep resolving: ref_get answers from the
     * node table, and freeing a node whose refs all dropped left the table entry
     * pointing at nothing, so the next transact on that handle found "node 0 owner 0"
     * and the caller was handed an error in the middle of a live sp<>. system_server
     * then ran incWeak on whatever that left behind and died on address 6.
     *
     * Leaking a node per published object costs a few kilobytes in a session; a
     * dangling handle costs the whole boot.
     */
    return;
    if (n->nrefs > 0 || n->internal_strong > 0) return;
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
    if (!ptr) {
        /*
         * No node may exist without an object address.
         *
         * One did: a reference command went out carrying ptr=0, the guest ran
         * incStrong on it and died reading *(0). Whatever path creates such a node has
         * to be found, so say so at the point of creation with the caller's cookie,
         * which is the one thing that identifies it.
         */
        logf_("binder: ★ node без адреса (cookie=0x%x) — не создаю", (unsigned)cookie);
        return 0;
    }
    n = zalloc(sizeof *n);
    n->owner = p;
    n->ptr = ptr;
    n->cookie = cookie;
    n->hnext = p->nodes[i];
    p->nodes[i] = n;
    return n;
}

/*
 * A published object handed back to its own process keeps no handle entry.
 *
 * The guest resolves a BINDER_TYPE_BINDER by pointer, so there is no handle number to hand
 * out and nothing to give back: a local JavaBBinder never produces BC_RELEASE. Recording a
 * ref anyway made nrefs climb for the whole session, which kept internal_strong up and
 * suppressed the release the owner was owed.
 *
 * The object still has to stay alive, and that is what internal_strong is for, so it is
 * raised here directly instead of through a ref.
 */
static void ref_inc_home(struct txn *t, struct node *n, int strong) {
    if (n->owner) {
        if (strong) n->local_strong++;
        else n->local_weak++;
        t->held[t->nheld].node = n;
        t->held[t->nheld++].strong = strong;
        node_work(n);
    }
}
static void ref_add_node(struct ref *r, struct node *n) {
    r->node = n;
    r->next_in_node = n->refs;
    n->refs = r;
    n->nrefs++;
}

static struct ref *ref_for_node(struct proc *p, struct node *n) {
    /*
     * Match on pid, not on the struct proc pointer.
     *
     * A process can hold more than one binder connection — a fresh one is opened when the old
     * one is dropped — and each gets its own struct proc. Looking only for the same pointer
     * therefore missed the ref an earlier connection had already been given for this node,
     * allocated a second one, and handed out a handle number that the guest had already been
     * told means something else. Measured: getService answered with handle 1, which in
     * surfaceflinger is its own IServiceManager, so the client resolved SurfaceFlinger to the
     * wrong object and reported "not published, waiting..." forever.
     */
    for (struct ref *r = n->refs; r; r = r->next_in_node)
        if (r->proc == p) return r;
    if (p->nrefs == 0) {
        /* descriptor 0 is the context manager and is never handed out */
        p->refs = zalloc(sizeof(struct ref *) * 2);
        p->refs[0] = zalloc(sizeof(struct ref));
        p->refs[0]->proc = p;
        p->refs[0]->desc = 0;
        p->nrefs = 1;
    }
    /*
     * A handle is never reused for a different object.
     *
     * libbinder caches handle -> BpBinder inside ProcessState and only drops an entry when
     * the weak reference behind it fails to revive. Handing the same number to another
     * node while that cache still holds the old BpBinder makes every later transaction on
     * that handle reach the wrong object — and, once the cached one has been destroyed, an
     * incStrong over freed memory, which is the fault at 0x20306.
     *
     * kernel keeps the number bound for as long as the process holds a reference, so this
     * table only grows. A boot allocates a few hundred entries; the reuse it replaces
     * bought nothing.
     */
    u32 d = p->nrefs;
    p->nrefs = d + 1;
    p->refs = realloc(p->refs, sizeof(struct ref *) * p->nrefs);
    struct ref *free_ref = zalloc(sizeof(struct ref));
    free_ref->proc = p;
    free_ref->desc = d;
    p->refs[d] = free_ref;
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
    node_work(n);
    node_free_if_unused(n);
}

static void ref_inc(struct ref *r, int strong) {
    if (strong) r->strong++; else r->weak++;
    node_work(r->node);
    /*
     * Tell the holder its descriptor exists.
     *
     * The kernel does this from binder_inc_ref: the receiving process gets BR_INCREFS (and
     * BR_ACQUIRE for a strong reference) carrying the handle it was just given, and only then
     * can it resolve that number. Here nothing was sent at all, so a handle allocated by
     * ref_for_node existed only in the daemon's table.
     *
     * Measured on this image: addService reached servicemanager with
     *   obj[0] type=0x73682a85 binder=0x1 cookie=0x0
     * and answered -1, because handle 1 was not in its table — the daemon had never told it.
     * Every getService that followed was answered ffffffff from the same cause: 62 lookups,
     * 3 replies, all failures, and the boot sat there.
     */
    if (0 && r->proc && r->node) { (void)strong; }
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
            node_work(h[i].node);
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
        if (th->fd >= 0 && th->waiting && !th->is_pump && !th->stack && !th->todo.head) return th;
    return 0;
}

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
    if (p->pump && p->pump->fd >= 0 && !p->pump->io.head)
        sb_send(p->pump, SB_R_OK, 0, 0, 0, 0, 0);
}

static void kick_proc(struct proc *p) {
    /*
     * This function is re-entered through its own tail: deliver() ends in a call back here, and
     * here we call deliver() again. On system_server that recursion never unwound: the daemon
     * printed the same stuck/wake pair for minutes, its main thread sat in waiting=1 stack=yes,
     * nothing was accepted on the listening socket, and every connecting guest was told
     * "Connection refused".
     *
     * A nesting counter turns an unbounded descent into a plain return. The work stays on the
     * queue; the outermost kick delivers it when the stack unwinds.
     */
    static __thread int depth;
    if (depth > 4) {
        if (depth == 5)
            logf_("sensesbinderd: kick_proc nesting %d on pid %d", depth, p ? p->pid : 0);
        return;
    }
    depth++;
    struct thread *th;
    /*
     * Bound the loop.
     *
     * deliver() calls back into kick_proc() at its tail, and this loop calls deliver() again, so a
     * thread that passes ready_thread() but fails deliverable() would spin here forever inside one
     * message handler — which is exactly the shape of the stall: the daemon goes silent on
     * "msg type=11" (SB_WAIT) and never services another connection, while the guests queue half a
     * million connect() calls behind it. A hard bound turns that into a logged skip instead of a
     * frozen daemon.
     */
    int guard = 0;
    while (p->todo.head && (th = ready_thread(p))) {
        if (++guard > 4096) {
            logf_("sensesbinderd: ★ kick_proc guard hit (pid %d, todo non-empty, no progress)", p->pid);
            break;
        }
        deliver(th);
    }
    // Nothing parked in SB_WAIT and the queue is still not empty: ring the alarm. A process whose
    // main thread only runs a frame loop (surfaceflinger) never blocks in ioctl, so writing to a
    // thread is not an option — the alarm thread turns this into POLLIN on the guest's binder
    // descriptor and the guest comes to collect the transaction itself.
    /*
     * A spawn request that no idle thread can carry goes to the main thread anyway.
     *
     * ready_thread() skips a thread that is waiting for a reply, and on this image that is the
     * only thread some processes have. W_SPAWN then stayed in the queue forever: the daemon
     * printed
     *   ask pid N for another binder thread (1/15)
     * again and again, started never grew, and every pass through kick_proc fell into the
     * stuck/wake loop — 19424 lines in one run. That loop is what kept the main thread out of
     * the accept path, so a new guest connection was reset by peer during its HELLO and every
     * later call on it failed with "Socket not connected".
     *
     * BR_SPAWN_LOOPER does not need an idle thread to act on: libbinder reads it out of the
     * read buffer and starts a pool thread itself. Handing it to whichever thread exists is
     * strictly better than never delivering it.
     */
    if (p->todo.head) {
        for (struct work *w = p->todo.head, *prev = 0; w; prev = w, w = w->next) {
            if (w->type != W_SPAWN) continue;
            struct thread *dst = 0;
            for (struct thread *d = p->threads; d; d = d->next)
                if (!d->is_pump) { dst = d; break; }
            if (!dst) break;
            if (prev) prev->next = w->next; else p->todo.head = w->next;
            if (p->todo.tail == w) p->todo.tail = prev;
            V("   -> pid %d/%d fd %d: spawn looper (forced)", p->pid, dst->tid, dst->fd);
            sb_send(dst, SB_R_SPAWN, 0, 0, 0, 0, 0);
            free(w);
            break;
        }
    }
    if (p->todo.head) {
        for (struct thread *d = p->threads; d; d = d->next)
            V("   stuck: tid %d fd %d%s waiting=%d stack=%s todo=%s", d->tid, d->fd,
              d->is_pump ? " (alarm)" : d->is_main ? " (main)" : "",
              d->waiting, d->stack ? "yes" : "no", d->todo.head ? "yes" : "no");
        ask_for_thread(p);
        wake_proc(p);
    }
    depth--;

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

static void queue_txn(struct txn *t, struct thread *target);
static int deliverable(struct thread *th);
static void queue_txn(struct txn *t, struct thread *target) {
    struct work *w = mkwork(W_TXN);
    w->t = t;
    /*
     * Only hand it to a specific thread when that thread can actually take it right now.
     *
     * The old code pushed the work onto the target and returned unconditionally. deliver()
     * refuses when the thread is busy — deliverable() wants an empty stack for work that comes
     * off a shared queue — so the transaction sat on a queue that nobody would ever look at
     * again. Measured: system_server's addService was acknowledged to the sender and then never
     * reached servicemanager, and the boot stopped at "Waiting for service SurfaceFlinger"
     * while the daemon logged "ask pid ... for another binder thread" over and over.
     */
    if (target) {
        wq_push(&target->todo, w);
        if (target->waiting) deliver(target);
        return;
    }
    if (!t->reply && (t->flags & TF_ONE_WAY) && t->target) {
        if (t->target->has_async) {
            wq_push(&t->target->async, w);
            return;
        }
        t->target->has_async = 1;
    }
    if (t->target && t->target == ctx_mgr) {
        struct thread *main_th = main_thread_of(t->to_proc);
        if (main_th && deliverable(main_th)) { wq_push(&main_th->todo, w); deliver(main_th); return; }
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
        if (t->data_size < sizeof(struct fbo) || offs[i] > t->data_size - sizeof(struct fbo) || (offs[i] & 3)) {
            V("   !! offset %llu out of %llu", (unsigned long long)offs[i], (unsigned long long)t->data_size);
            return -1;
        }
        struct fbo *o = (struct fbo *)(data + offs[i]);
        V("   fbo[%d] type=0x%08x binder=0x%08x cookie=0x%08x at %llu",
          (int)i, o->type, o->binder, o->cookie, (unsigned long long)offs[i]);
struct held *h = &t->held[t->nheld];
        switch (o->type) {
        case T_BINDER: case T_WEAK_BINDER: {
            /*
             * A pointer that cannot be an object is refused here rather than passed on.
             *
             * The guest hands its object address in this field, and on this firmware that
             * field sometimes carries something else: the run before this one had
             * 0xfffffff8 in it, an errno, and the acquire path duly walked *0xfffffff8 and
             * died. The value had come all the way from the guest, so no amount of fixing
             * the send path helps — the daemon has to stop recording it in the first place.
             *
             * Two ranges are rejected outright: anything below 0x10000, which no object on
             * a 32-bit image can be (the first pages belong to the loader and to null), and
             * the top sixteen, which is where the kernel's negative errno values live. A
             * transaction that tries to publish such a pointer is failed, and the caller
             * sees BR_FAILED_REPLY instead of a crash much later.
             */
            if (o->binder >= 0xfffff000u) {
                logf_("binder: ★ объект по адресу 0x%08x невозможен (cookie 0x%08x) — отказываю",
                    o->binder, o->cookie);
                return -1;
            }
            /*
             * The object address is in .cookie; .binder holds getWeakRefs().
             *
             * This image's flatten_binder writes the two in that order:
             *
             *   31c52: str r4, [sp, #12]     .cookie = the object
             *   31c54: str r0, [sp, #8]      .binder = getWeakRefs()
             *
             * and unflatten_binder of this era reads a BINDER_TYPE_BINDER back with
             * "*out = static_cast<IBinder*>(flat->cookie)".
             *
             * The daemon used .binder as the object address, so every BR_ACQUIRE told the
             * owner to incStrong a weakref_impl: *(weakrefs) is a vtable pointer, -12 lands
             * in front of it, and the fault is at libutils +0x20306. That is the crash every
             * round has been chasing.
             *
             * The node is keyed by both fields, so the pair stays unique; only which one is
             * treated as the object changes.
             */
            if (!o->binder && !o->cookie) break;
            struct node *nd = node_get(p, o->binder, o->cookie, 1);
            if (!nd) return -1;
            int strong = o->type == T_BINDER;
            /*
             * A binder object on its way home does not take a new reference.
             *
             * ref_for_node() used to be called before this decision, so every home delivery of a
             * published object still created a handle entry for its own process. The guest can
             * never give that entry back — a local JavaBBinder produces no BC_RELEASE — so nrefs
             * only ever climbed: 788 BR_ACQUIRE against 0 BR_RELEASE in one session. The count
             * that never drops also keeps internal_strong up, which in turn suppresses the
             * release the owner is owed, and the surplus wp<> is what the pool thread later walks
             * into at libandroid_runtime +0x3688a.
             *
             * The kernel does not do this either: binder_transaction hands the original pointer
             * back for BINDER_TYPE_BINDER without creating a ref for the receiving process.
             */
            if (nd->owner && nd->owner->pid == to->pid) {
                V("   home: obj=0x%x wr=0x%x owner=%d to=%d code=%u",
                  (unsigned)nd->ptr, (unsigned)nd->cookie,
                  nd->owner ? nd->owner->pid : 0, to->pid, (unsigned)t->code);
                o->type = strong ? T_BINDER : T_WEAK_BINDER;
                o->binder = (u32)nd->ptr;
                o->cookie = (u32)nd->cookie;
                ref_inc_home(t, nd, strong);
                break;
            }
            struct ref *r = ref_for_node(to, nd);
            logf_("binder: TB step pid %d nd=%p nrefs=%u internal=%d owner=%d to=%d code=%u",
                  p->pid, (void *)nd, (unsigned)nd->nrefs, nd->internal_strong,
                  nd->owner ? nd->owner->pid : 0, to->pid, (unsigned)t->code);
            /*
             * Publishing an object takes an internal strong reference, as the kernel does.
             *
             * binder_transaction calls binder_inc_node with internal=1 for a
             * BINDER_TYPE_BINDER, and the owner is told so through BR_ACQUIRE. That internal
             * reference is what keeps the object alive while handles to it exist elsewhere:
             * the owner's own sp<> is not what holds it, the driver is.
             *
             * Without it the owner drops its sp<> right after publishing, the object is
             * destroyed, and the next BR_ACQUIRE that the daemon sends for a handle reaches
             * libbinder with mRefs already overwritten — *(ptr)=1 in the last run, which
             * became (1-12) and faulted on 0xfffffff5.
             *
             * The shim cannot filter this out by looking at the object first: it checks when
             * the message arrives, and the guest acts on it some time later.
             */
            if (nd && nd->nrefs == 1 && !nd->internal_strong) {
                nd->internal_strong = 1;
                /*
                 * Hand the acquire to the owner right now, as the kernel does, instead of
                 * queueing it. The node is the one the owner just published, so the object
                 * is certainly alive at this point: it goes out with this ioctl's answer.
                 */
                V("   T_BINDER obj=0x%x wr=0x%x cur_th=%p cur_proc=%p p=%p has_strong=%d",
                  (unsigned)o->cookie, (unsigned)o->binder,
                  (void *)g_cur_th, g_cur_th ? (void *)g_cur_th->proc : 0, (void *)p, nd->has_strong);
                /*
                 * The acquire is queued, never piggybacked on this very answer.
                 *
                 * Handing it back inside the ioctl that delivered the transaction ran the guest's
                 * incStrong while its RefBase was still being built: the object already had its
                 * vtable written but mRefs was not assigned yet, so incStrong read this+4 as zero
                 * and faulted at libutils +0x20306. Measured words at the object were
                 * "fcb735ad 0000" — vtable present, mRefs empty.
                 *
                 * The kernel queues it for the same reason: the owner gets the acquire on a later
                 * read, by which time the object is fully constructed.
                 */
                node_work(nd);
            }
            /*
             * A binder object that goes home must go home as a binder object.
             *
             * unflatten_binder of this firmware reads the pointer out of .cookie
             * (libutils +0x311a4: ldr r1, [r0, #12]) and simply assigns it, so the guest
             * gets back the very object it published. What the daemon did instead was
             * rewrite the object into a handle, and the owner then built a BpBinder proxy
             * for its own service. That is not a cosmetic difference: ActivityManagerService
             * keeps its ProcessRecords in a map keyed by the ApplicationThread IBinder, and a
             * proxy is a different object, so every call from system_server to its own
             * services died with "Unable to find app for caller
             * android.app.ApplicationThreadProxy@... (pid=6082) when registering receiver".
             * The kernel hands the original pointer back for exactly this reason.
             */
            /*
             * addService gives the object to servicemanager, and there it must stay an object.
             *
             * Measured on this image: system_server's first addService went out as
             *   obj[0] type=0x73682a85 binder=0x1 cookie=0x0
             * (BINDER_TYPE_HANDLE, handle 1) and servicemanager then did nothing at all —
             * not one reply, not one further ioctl, its log file empty. It resolves a handle
             * against ITS OWN table, and handle 1 does not exist there: the number belonged
             * to the sender. The kernel keeps a BINDER_TYPE_BINDER a BINDER_TYPE_BINDER for
             * exactly this reason — binder_get_node() is what the service registry stores,
             * not a per-process number.
             *
             * Every other transfer to servicemanager keeps the handle form; only the publish
             * call carries an object it has to hold on to.
             */
            /*
             * Publishing to the context manager: the receiver keeps the node, not a number.
             *
             * addService is the one call where servicemanager has to hold on to the object
             * itself, and it stores it in a map keyed by the IBinder it was handed. Two shapes
             * were measured on this image and both failed, in different ways:
             *
             *   BINDER_TYPE_HANDLE, binder = sender's number (1)
             *     servicemanager resolves a handle against ITS OWN table; handle 1 is not there,
             *     so binder == NULL, addService answers -1, and every getService that follows
             *     is answered with ffffffff. Measured: 62 lookups, 3 replies, all -1.
             *
             *   BINDER_TYPE_BINDER, binder = sender's weakref, cookie = sender's object
             *     The receiver reads those addresses in ITS address space. Both read as zero
             *     (obj[0] ... binder=0xac38(*=00000000) cookie=0xab34(*=00000000)), the
             *     object is NULL, and addService answers -1 the same way.
             *
             * What the kernel does is different from both: it creates a node in the receiving
             * process (binder_get_ref with a real descriptor from that process's table) and
             * keeps the node alive for the registry. That is exactly ref_for_node(to, nd), and
             * it is what this branch already computed as r. The descriptor is only unusable if
             * the receiver's table never learns about it — so it has to be told, which is what
             * the reference command below does.
             */
            o->type = strong ? T_HANDLE : T_WEAK_HANDLE;
            o->binder = (u32)r->desc;
            o->cookie = 0;
            logf_("binder: PUBLISH->sm desc=%u node=%p owner=%d to=%d code=%u",
                  (unsigned)r->desc, (void *)nd, nd->owner ? nd->owner->pid : 0,
                  to->pid, (unsigned)t->code);
            ref_inc(r, strong); h->ref = r; h->strong = strong; t->nheld++;
            break;
        }
        case T_HANDLE: case T_WEAK_HANDLE: {
            struct ref *r = ref_get(p, o->binder);
            int strong = o->type == T_HANDLE;
            logf_("binder: HANDLE-IN pid %d desc %u -> %s (to=%d code=%u)",
                  p->pid, (unsigned)o->binder, r ? "resolved" : "MISS", to->pid, (unsigned)t->code);
            /*
             * A handle the sender never imported is passed through unchanged instead of
             * failing the whole transaction.
             *
             * Measured: zygote answers a call with 16 bytes of data whose single flat
             * object is BINDER_TYPE_HANDLE, binder = 1, cookie = 0, and translate()
             * returned -1 because ref_get(zygote_p, 1) is NULL. The reply then never
             * reached the caller and the call was lost.
             *
             * The value is not garbage — handle 1 is the number the sender itself sees,
             * and the guest resolves it against its own table. Refusing here bought
             * nothing: the alternative was dropping the answer, which is strictly worse
             * than handing over a number the receiver is about to look up anyway.
             */
            if (!r) {
                V("   handle %u unknown to pid %d", (unsigned)o->binder, p->pid);
                return -1;
            }
            /*
             * A handle always goes out as a handle, even when it is on its way back to the process
             * that published the object. Handing such an object home as BINDER_TYPE_BINDER with its
             * own address is what Parcel::readStrongBinder turns into refbase->incStrong(obj->binder),
             * and that call read [0x70754613 + 4] — no mapping — right after getService("power") came
             * back, which is where system_server died. Every other binder object this firmware passes
             * (media.*, SurfaceFlinger, IMemoryHeap) goes out as a handle and works, so the home case
             * stops being special.
             */
            /*
             * A handle that lands back on the process that published the object goes home as
             * the object itself.
             *
             * Measured: servicemanager answered getService("SurfaceFlinger") with handle 1, the
             * daemon resolved it to the node owned by pid 7619, and the receiver was pid 7619 —
             * surfaceflinger asking for its own service. Allocating a fresh handle there returned
             * the object to its owner as a BpBinder proxy, so SurfaceComposerClient kept printing
             * "SurfaceFlinger not published, waiting..." even though the lookup had succeeded.
             * The kernel hands the pointer back whenever target_node->proc == t->from->proc.
             */
            if (r->node->owner && r->node->owner->pid == to->pid) {
                o->type = strong ? T_BINDER : T_WEAK_BINDER;
                o->binder = (u32)r->node->ptr;
                o->cookie = (u32)r->node->cookie;
                ref_inc_home(t, r->node, strong);
                logf_("binder: HANDLE-HOME pid %d desc %u -> pid %d obj=0x%x code %u",
                      p->pid, (unsigned)o->binder, to->pid, (unsigned)o->cookie, (unsigned)t->code);
                break;
            }
            struct ref *nr = ref_for_node(to, r->node);
            logf_("binder: HANDLE-OUT pid %d desc %u -> pid %d desc %u (node of %d) code %u",
                  p->pid, (unsigned)o->binder, to->pid, (unsigned)nr->desc,
                  r->node->owner ? r->node->owner->pid : 0, (unsigned)t->code);
            o->binder = (u32)nr->desc; o->cookie = 0;
            ref_inc(nr, strong); h->ref = nr; h->strong = strong; t->nheld++;
            if (strong && r->node->owner) { r->node->internal_strong = 1; node_work(r->node); }
            break;
        }
        case T_FD:
            // The guest sends the raw descriptor here, but the receiver is told a position in the
            // SCM_RIGHTS array that travels with the message: guest_binder.c reads it back as
            // fds[fo->handle]. Leaving the raw number in place made every lookup miss and every
            // heap come back as -1, which is what killed system_server on its first IMemoryHeap.
            V("binder: T_FD guest fd %u -> index %d of %d (code %u to pid %d)",
              (unsigned)o->binder, fdi, t->nfds, (unsigned)t->code, to->pid);
            if (fdi >= t->nfds) return -1;
            o->binder = (u32)fdi;
            fdi++;
            break;
        default:
            /*
             * Name the object type that was rejected.
             *
             * translate() returning -1 makes the daemon answer BR_FAILED_REPLY, and the guest then
             * sees "!!! FAILED BINDER TRANSACTION !!!". Display.init() turns exactly that into an
             * IllegalArgumentException, which killed system_server at
             * WindowManagerImpl.getDefaultDisplay — after Connectivity Service had already started.
             * Without this line the type is invisible.
             */
            logf_("binder: ★ unknown fbo type %#x (code %u to pid %d) — rejecting the transaction",
                  o->type, (unsigned)t->code, to ? to->pid : -1);
            return -1;
        }
    }
    return 0;
}

static void proc_release(struct proc *p);
/*
 * Detach a process that lost its link, without touching what it published.
 *
 * proc_release() is the right thing when a process is really gone: it clears has_strong/has_weak on
 * every node and drops ctx_mgr if the caller owned it. Applied to a mere disconnect that is far too
 * destructive — servicemanager staying registered is the whole point of the service registry, and
 * the boot died at Starting Activity Manager every time this ran on it.
 *
 * The struct itself is deliberately leaked: nodes keep n->owner pointing at it, and freeing it here
 * would leave every one of them dangling. Losing a few hundred bytes per disconnect is cheaper than
 * the alternatives.
 */
static void proc_release_keep_nodes(struct proc *p) {
    struct proc **pp = &procs;
    while (*pp) { if (*pp == p) { *pp = p->next; break; } pp = &(*pp)->next; }
    for (struct work *w = p->todo.head, *nx; w; w = nx) { nx = w->next; free(w); }
    p->todo.head = p->todo.tail = 0;
    for (struct thread *th = p->threads, *nx; th; th = nx) {
        nx = th->next;
        if (th->todo.head) {
            for (struct work *w = th->todo.head, *wx; w; w = wx) { wx = w->next; free(w); }
            th->todo.head = th->todo.tail = 0;
        }
        if (th->fd >= 0 && th->fd != g_listen_fd) { epoll_ctl(ep, EPOLL_CTL_DEL, th->fd, 0); close(th->fd); }
        th->fd = -1;
        free(th);
    }
    p->threads = 0;
    p->pump = 0;
}

/**
 * Dump the head of a parcel as hex. Guessing at the layout cost several rounds: two attempts at
 * decoding String16 produced an empty name because the offsets were not where I assumed. Sixteen
 * bytes of the real thing answer the question outright.
 */
static void parcel_hex(const char *data, u64 size, char *out, size_t cap) {
    static const char d[] = "0123456789abcdef";
    // a servicemanager parcel starts with the interface token: a 4-byte length word plus 26 UTF-16
// characters, which comes to 56 bytes and is already 4-aligned, so the name it refers to begins
    // exactly at offset 56
    size_t n = size < 128 ? (size_t)size : 128, o = 0;
    for (size_t i = 0; i < n && o + 3 < cap; i++) {
        unsigned char b = (unsigned char)data[i];
        out[o++] = d[b >> 4];
        out[o++] = d[b & 15];
        if ((i & 3) == 3 && o + 2 < cap) out[o++] = ' ';
    }
    out[o] = 0;
}

/**
 * Print both things a servicemanager call carries: the interface token at offset 0 and the service
 * name that follows it. The name is what matters — it says whether a process is registering itself
 * or asking for somebody else, and the earlier "code 1"/"code 2" numbering said neither.
 */
static void parcel_dump(const char *data, u64 size, char *out, size_t cap) {
    size_t o = 0;
    for (int pass = 0; pass < 2 && o + 8 < cap; pass++) {
        u64 off = 0;
        if (pass == 1) {
            if (size < 4) break;
            u32 first;
            memcpy(&first, data, 4);
            off = 4 + (u64)first * 2;
            off = (off + 3) & ~3ULL;
            o += (size_t)snprintf(out + o, cap - o, " | name=");
        } else {
            o += (size_t)snprintf(out + o, cap - o, "token=");
        }
        if (off + 4 > size) { o += (size_t)snprintf(out + o, cap - o, "<short>"); break; }
        u32 len;
        memcpy(&len, data + off, 4);
        if (len > 200 || off + 4 + (u64)len * 2 > size) {
            o += (size_t)snprintf(out + o, cap - o, "<bad %u>", len);
            continue;
        }
        size_t i = 0;
        for (; i < len && o + 2 < cap; i++) {
            u16 ch;
            memcpy(&ch, data + off + 4 + i * 2, 2);
            out[o++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
    }
    out[o] = 0;
}

static void do_transact(struct thread *th, struct sb_txn *m, char *payload, int *fds, int nfds, int reply) {
    struct proc *p = th->proc;
    /* the answer to this ioctl is being built on this thread: see g_cur_th */
    g_cur_th = th;
    struct txn *t = zalloc(sizeof *t);
    t->memfd = -1;
    t->code = m->code; t->flags = m->flags; t->reply = reply;
    t->sender_pid = p->pid; t->sender_euid = m->sender_euid ? m->sender_euid : p->key;
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
        if (!in_reply_to || in_reply_to->to_thread != th) {
            sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0);
            sb_send(th, SB_R_FAILED_REPLY, 0, 0, 0, 0, 0);
            txn_free(t);
            return;
        }
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
            /*
             * A handle that no longer resolves is answered as a dead reply, not dropped.
             *
             * Dropping it left the caller inside a transact that never returned: the
             * caller's sp<> stayed live over a node the daemon had already recycled, and
             * the next incStrong on it walked freed memory (fault address 6). BR_DEAD_REPLY
             * is what the kernel sends in this case, and the parser of this image handles
             * it: the transaction fails cleanly and the caller drops its reference.
             */
            V("   !! no target for handle %u (node %p owner %p) -> error reply",
              m->handle, (void *)n, n ? (void *)n->owner : 0);
            sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0);
            sb_send(th, SB_R_DEAD_REPLY, 0, 0, 0, 0, 0);
            txn_free(t);
            return;
        }
        t->target = n;
        to = n->owner;
        if (!(t->flags & TF_ONE_WAY)) {
            struct txn *parent = th->stack;
            for (int level = 0; parent && level < 64; level++) {
                if (parent->to_thread != th) break;
                if (parent->from && parent->from->proc == to && parent->from->fd >= 0) {
                    target_thread = parent->from;
                    break;
                }
                parent = parent->to_parent;
            }
        }
    }
    t->to_proc = to;
    if (!reply) {
        // Every servicemanager call gets its parcel dumped. The name decoder produced an empty
        // string twice, and the only way to stop guessing at the layout is to look at the bytes.
        char hex[420], names[200];
        if (m->handle == 0 && m->code >= 1 && m->code <= 3) {
            parcel_dump(t->data, t->data_size, names, sizeof names);
            parcel_hex(t->data, t->data_size, hex, sizeof hex);
            V("txn pid %d -> pid %d handle %u code %u data %llu [%s] %s",
              p->pid, to->pid, m->handle, m->code, (unsigned long long)m->data_size, names, hex);
        } else {
            V("txn pid %d -> pid %d handle %u code %u flags %#x data %llu",
              p->pid, to->pid, m->handle, m->code, m->flags, (unsigned long long)m->data_size);
        }
    }
    if (translate(th, to, t, t->data) < 0) {
        V("   !! translate failed for code %u", m->code);
        sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0);
        sb_send(reply && target_thread ? target_thread : th, SB_R_FAILED_REPLY, 0, 0, 0, 0, 0);
        if (reply && in_reply_to) { in_reply_to->from = 0; txn_free(in_reply_to); }
        txn_free(t);
        return;
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
    while (th->todo.head && th->todo.head->type == W_NODE) {
        struct work *w = wq_pop(&th->todo);
        struct node *node = w->node;
        node->queued = 0;
        u32 commands[32];
        int count = emit_node(node, commands);
        struct sb_ptr_cookie changes[4];
        int used = 0;
        for (int i = 0; i + 5 <= count && used < 4; i += 5) {
            changes[used].op = commands[i] == BR_INCREFS ? SB_REF_INCREFS :
                               commands[i] == BR_ACQUIRE ? SB_REF_ACQUIRE :
                               commands[i] == BR_RELEASE ? SB_REF_RELEASE : SB_REF_DECREFS;
            changes[used].pad = 0;
            memcpy(&changes[used].ptr, commands + i + 1, 8);
            memcpy(&changes[used].cookie, commands + i + 3, 8);
            used++;
        }
        if (used) sb_send(th, SB_R_REF, changes, used * sizeof changes[0], 0, 0, 0);
        free(w);
    }
    sb_send(th, SB_R_ACK, 0, 0, 0, 0, 0);
    V("   -> pid %d/%d fd %d: ack (%s)", p->pid, th->tid, th->fd, reply ? "reply" : "transact");
    queue_txn(t, target_thread);
    if (reply && in_reply_to) { in_reply_to->from = 0; txn_free(in_reply_to); }
}

/*
 * The thread whose ioctl is being answered right now.
 *
 * kernel sends BR_ACQUIRE for a freshly published object inside the very same
 * BINDER_WRITE_READ that carried the transaction, so the owner runs incStrong before its
 * own sp<> has a chance to drop. The daemon used to queue that command and deliver it on
 * a later pass, which left a window: the owner released the object first and the acquire
 * then ran incStrong over freed memory. Sending it here, on the answering thread, closes
 * that window.
 */
static struct thread *g_cur_th;

#include "sb_nodes.inc"
static int deliverable(struct thread *th) {
    // A reply addressed to this thread carries the answer to the call sitting on its stack: the
    // stack is expected to be non-empty, and refusing it there was what kept every reply undelivered
    // and every stack uncleared. Work from the shared queue still needs an idle thread.
    /*
     * The alarm thread never reads commands.
     *
     * It exists only to receive the wake nudge and turn it into POLLIN on the guest's binder
     * descriptor (see wake_proc). Writing a BR_REPLY or a transaction to it put a command into a
     * socket that nobody consumes: measured on servicemanager — "cmd[0] = 0x80287202" followed by
     * "посылка БЕЗ чтения" — after which its stream was off by one message and binder_loop died
     * with "ioctl failed (Transport endpoint is not connected)".
     */
    if (!th || th->fd < 0 || !th->proc || th->is_pump || !th->waiting) return 0;
    if (th->todo.head) return 1;
    return !th->stack && th->proc->todo.head;
}
static int sb_send(struct thread *th, u32 type, const void *body, u32 blen, const int *fds, int nfds, u32 flags) {
    return io_send(th, type, body, blen, fds, nfds, flags);
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
        /*
         * A freshly spawned pool thread takes transactions, not reference commands.
         *
         * The node work lives on the process queue, so any thread of the process could
         * pick it up — including one that libbinder has only just started, whose view of
         * the engine's buffers is the one its first ioctl builds. Handing such a thread a
         * BR_ACQUIRE put it into executeCommand with a pointer it had never seen accepted,
         * and the fault surfaced in joinThreadPool of that very thread:
         *
         *   androidCreateRawThreadEtc -> Thread::_threadLoop -> spawnPooledThread
         *     -> IPCThreadState::joinThreadPool -> executeCommand
         *
         * Reference commands therefore go to the main thread only; the process queue is
         * reached from it alone. Transactions stay available to every thread, which is
         * what the pool is for.
         */
        /*
         * Transactions belong to every thread of the process, reference commands to the main
         * one. The queue selection used to enforce that by giving the process queue to the main
         * thread alone, which also hid every transaction from the pool: surfaceflinger keeps its
         * main thread inside SurfaceFlingerSynchro::wait() and never comes back to binder, so a
         * transaction parked on the process queue was never collected. It sat there while
         * system_server asked for the control block, and the whole boot stopped at
         * "Waiting for service SurfaceFlinger".
         *
         * So the pool may drain the process queue too, and only a W_NODE item is put back for the
         * main thread to handle.
         */
        struct wq *q = th->todo.head ? &th->todo : th->stack ? 0 : &p->todo;
        if (q && q->head && q->head->type == W_NODE && nnodes == 16) break;
        struct work *w = q ? wq_pop(q) : 0;
        if (!w) break;
        /*
         * Reference commands are not the main thread's business.
         *
         * W_NODE was pushed back on the process queue here whenever the thread that popped it
         * was not the main one, so a pool thread could take the item, decide it was not allowed
         * to act on it, put it back and leave — doing no work at all. With the main thread
         * sitting in waiting=1 (it is inside a call to servicemanager), ready_thread() never
         * gives it anything, the item stays at the head of the queue, and kick_proc spins:
         *
         *   stuck: tid 9131 fd 11 (main) waiting=1 stack=yes todo=no
         *   stuck: tid 9139 fd 12 waiting=0 stack=no todo=no
         *
         * 6465 times per thread in one run. The daemon never returned to accept(), the next
         * guest connection was reset during its HELLO, and everything after it failed with
         * "Socket not connected".
         *
         * emit_node only builds reference commands and writes them to the socket of whichever
         * thread is doing the work; nothing in it needs the main thread.
         */
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
        case W_REF: break;
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
        /*
         * The pair the guest uses to rebuild the object: .cookie is the object, .binder its
         * weakref. Both were being filled with ptr, so unflatten_binder of this firmware read
         * the object out of .cookie and got the weakref instead — every rebuilt local binder
         * pointed at a weakref_impl, and the first incStrong on it walked a vtable that belonged
         * to a counter block.
         */
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
            do { b->id = ++p->next_buf; } while (!b->id);
            m.pad = b->id;
            memcpy(buf, &m, sizeof m);
            b->async_node = !t->reply && (t->flags & TF_ONE_WAY) ? t->target : 0;
            b->held = t->held;
            b->nheld = t->nheld;
            t->held = 0;
            t->nheld = 0;
            b->next = p->bufs;
            if (p->bufs) p->bufs->prev = b;
            p->bufs = b;
        }
        u32 more_flag = (th->todo.head || p->todo.head) ? SB_F_MORE : 0;
        // A 4-byte reply to a servicemanager call is the int32 result: zero means the service was
        // added or found, anything else means it was refused. Printing it is the only way to tell
        // "registered" apart from "already there", which the guest log words identically.
        if (t->reply && t->data_size == 4 && t->code == 0) {
            int res;
            memcpy(&res, t->data, 4);
            V("   reply result for pid %d: %d", p->pid, res);
        }
        // A reply carrying a flat_binder_object is how a lookup hands back a binder. If the object's
        // type or handle is wrong, the guest treats the cookie as a pointer and dies on it — which is
        // exactly what a SIGSEGV at si_addr=0x707545e7 after this ioctl looks like. Print the bytes.
        if (t->reply && t->data_size && t->data_size <= 64) {
            char rh[420], rn[200];
            parcel_dump(t->data, t->data_size, rn, sizeof rn);
            parcel_hex(t->data, t->data_size, rh, sizeof rh);
            V("   reply body pid %d code %u size %llu [%s] %s",
              p->pid, t->code, (unsigned long long)t->data_size, rn, rh);
        }
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
    /*
     * Drop the queued work that names this process's nodes before the nodes go away.
     *
     * node_work parks a W_NODE item carrying a bare struct node * on this very queue. The
     * nodes are freed below, and the freed blocks are handed straight back out by the
     * allocator — measured: the dead pointer and the txn allocated immediately after it
     * were 0x767f0ba430 and 0x767f0ba240. emit_node then read owner/ptr out of reused
     * memory, called the node dead, and the reference command the owner was owed never
     * went out. SurfaceFlinger stayed at "not published, waiting..." because the object it
     * had just registered never got its acquire.
     */
    for (struct work *w = p->todo.head, *nx; w; w = nx) {
        nx = w->next;
        free(w);
    }
    p->todo.head = p->todo.tail = 0;
    for (struct thread *th = p->threads, *nx; th; th = nx) {
        nx = th->next;
        if (th->todo.head) {
            for (struct work *w = th->todo.head, *wx; w; w = wx) { wx = w->next; free(w); }
            th->todo.head = th->todo.tail = 0;
        }
        if (th->fd >= 0 && th->fd != g_listen_fd) { epoll_ctl(ep, EPOLL_CTL_DEL, th->fd, 0); close(th->fd); }
        /*
         * Detach, never free.
         *
         * epoll_wait may already have returned a batch of events that names another descriptor of
         * this same process. Freeing the structures here leaves those pending entries pointing at
         * released memory, and the next one dereferenced kills the daemon: measured on the ADP1
         * image — SurfaceFlinger's two links close, then "binderd exited, code 139" (SIGSEGV), after
         * which every binder call fails and SurfaceFlinger itself returns from main with -1 (exit 255).
         * Marking them is enough: on_message() ignores a thread whose proc is gone.
         */
        th->fd = -1;
        th->proc = 0;
        th->next = 0;
    }
    for (u32 i = 0; i < NHASH; i++) {
        struct node *n = p->nodes[i];
        while (n) {
            struct node *nx = n->hnext;
            /*
             * The node outlives its owner.
             *
             * Freeing it here left every other process's handle dangling: the refs of other
             * processes were rewritten to r->node = 0, so a later transact on such a handle
             * found nothing, and — worse — node_work could still hold the pointer in a queued
             * W_NODE. The kernel keeps a node alive as long as anybody holds a reference to
             * it and only reports the owner as gone; the same applies here. node_free_if_unused
             * decides when it can actually go, and today it deliberately keeps them.
             */
            n->owner = 0;
            n->has_strong = n->has_weak = n->queued = n->has_async = 0;
            n->internal_strong = n->local_strong = n->local_weak = 0;
            n->hnext = 0;
            n->pend_strong = n->pend_weak = 0;
            n = nx;
        }
        p->nodes[i] = 0;
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
    struct dbuf *b = p->bufs;
    while (b && b->id != id) b = b->next;
    if (!b) return;
    if (b->prev) b->prev->next = b->next;
    else p->bufs = b->next;
    if (b->next) b->next->prev = b->prev;
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

static void disconnect_thread(struct thread *th) {
    if (th->fd >= 0) {
        epoll_ctl(ep, EPOLL_CTL_DEL, th->fd, 0);
        close(th->fd);
        th->fd = -1;
    }
    io_dispose(th);
    th->waiting = 0;
    if (th->proc) {
        if (th->proc->pump == th) th->proc->pump = 0;
        if (ctx_th == th) ctx_th = 0;
    }
}

static int valid_message(struct thread *th, const struct sb_hdr *h, const u8 *body, u32 len) {
    if (h->type == SB_HELLO)
        return !th->proc && len == sizeof(struct sb_hello) && h->nfds == 0;
    if (!th->proc || h->tid != (u32)th->tid) return 0;
    switch (h->type) {
    case SB_TRANSACT:
    case SB_REPLY: {
        if (len < sizeof(struct sb_txn)) return 0;
        struct sb_txn tx;
        memcpy(&tx, body, sizeof tx);
        u64 room = len - sizeof tx;
        return tx.data_size <= room && tx.offsets_size == room - tx.data_size &&
               tx.offsets_size % sizeof(u64) == 0;
    }
    case SB_SET_MAX_THREADS:
    case SB_LOOPER: return len == 4 && h->nfds == 0;
    case SB_REF: return len == sizeof(struct sb_ref) && h->nfds == 0;
    case SB_REF_DONE:
    case SB_NODE_FREE: return len == sizeof(struct sb_ptr_cookie) && h->nfds == 0;
    case SB_DEATH: return len == sizeof(struct sb_death) && h->nfds == 0;
    case SB_DEAD_DONE: return len == 8 && h->nfds == 0;
    case SB_ASYNC_DONE: return (len == 0 || len == 8) && h->nfds == 0;
    case SB_SET_CTX_MGR:
    case SB_WAIT:
    case SB_UNWAIT:
    case SB_BYE: return len == 0 && h->nfds == 0;
    default: return 0;
    }
}

static void on_message(struct thread *th) {
    if (th->fd < 0) return;
    int received = io_receive(th);
    if (received == 0) return;
    if (received < 0) {
        V("binder: link closed tid %d pid %d errno=%d", th->tid,
          th->proc ? th->proc->pid : -1, errno);
        disconnect_thread(th);
        return;
    }
    struct sb_hdr h = th->io.header;
    u32 blen = h.len - sizeof h;
    uint8_t *rxbuf = th->io.body;
    if (!valid_message(th, &h, rxbuf, blen)) {
        V("binder: rejected frame type=%u tid=%u len=%u", h.type, h.tid, h.len);
        disconnect_thread(th);
        return;
    }
    int fds[SB_MAXFDS], nfds = th->io.nfds;
    memcpy(fds, th->io.fds, (size_t)nfds * sizeof(int));
    th->io.nfds = 0;
    th->io.header_used = 0;
    th->io.body_used = 0;
    V("binder: msg type=%u len=%u tid=%u fd=%d", h.type, h.len, h.tid, th->fd);
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
            /*
             * One thread until the guest asks for more.
             *
             * This used to default to 16, and the daemon then believed every process
             * could take another binder thread. servicemanager never calls
             * setThreadPoolMaxThreadCount — it is a single-threaded loop over its
             * buffer — so as soon as work queued up it was sent BR_SPAWN_LOOPER, which
             * its parser does not know:
             *
             *   Binder: parse: OOPS 29197          (0x720D, nr 13 = BR_SPAWN_LOOPER)
             *   Binder: binder_loop: io error -1 Unknown error: 0
             *
             * The context manager exited there, every addService that followed failed,
             * and system_server died soon after with its own fault.
             *
             * zygote and system_server do ask (setThreadPoolMaxThreadCount(16)) and get
             * their pool; nobody else is asked to spawn anything.
             */
            p->max_threads = 1;
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
                if (pc->op == SB_REF_INCREFS && n->pend_weak > 0) n->pend_weak--;
                if (pc->op == SB_REF_ACQUIRE && n->pend_strong > 0) n->pend_strong--;
                if (n->ptr) node_work(n); else { n->pend_strong = 0; n->internal_strong = 0; }
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
        {
            static unsigned seen_in, seen_out;
            if (seen_in < 40) { seen_in++; logf_("binder: SB_WAIT enter tid=%d pid=%d", th->tid, th->proc ? th->proc->pid : -1); }
            deliver(th);
            if (seen_out < 40) { seen_out++; logf_("binder: SB_WAIT exit  tid=%d", th->tid); }
        }
        if (th->waiting && th->proc) kick_proc(th->proc);
        break;
    case SB_UNWAIT:
        th->waiting = 0;
        break;
    case SB_BYE: {
        struct proc *p = th->proc;
        if (!p) break;
        /*
         * The descriptor and the epoll registration belong to the thread that is leaving.
         *
         * Only the number was cleared here: the socket stayed open with nobody holding it, and
         * the epoll entry still pointed at the struct that is freed two lines later. Both were
         * leaks that grew with every thread that left — and every pool thread that exits does
         * this. Del the registration first, then close, then free.
         */
        /*
         * Never touch the listening socket from here.
         *
         * The descriptor numbers of threads that died are handed straight back out by the
         * allocator, so a stale th->fd can name the listening socket or a socket that now
         * belongs to a different thread of the same process. EPOLL_CTL_DEL on the listening
         * socket removes it from the set for good: the daemon keeps running (heartbeats still
         * appear) but never accepts again, and every guest connect is answered with
         * ECONNREFUSED. Measured: errno=111 on 30 consecutive connect attempts while the daemon
         * was alive, and "accepted fd" stuck at 9.
         *
         * Ownership is decided by what the epoll set actually holds: the thread pointer is only
         * valid as a key while the socket it names is the one that was registered for it.
         */
        if (th->fd >= 0 && th->fd != g_listen_fd) {
            epoll_ctl(ep, EPOLL_CTL_DEL, th->fd, 0);
            close(th->fd);
        }
        th->fd = -1;
        struct thread **pp = &p->threads;
        while (*pp) { if (*pp == th) { *pp = th->next; break; } pp = &(*pp)->next; }
        if (p->pump == th) p->pump = 0;
        io_dispose(th);
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
    g_cur_th = 0;
}

/*
 * Independent heartbeat.
 *
 * Every symptom left in this project depends on knowing whether the daemon is still turning:
 * the guest reports ECONNREFUSED in thousands while the main-loop log goes quiet, and a quiet
 * log cannot tell "stuck in a call" from "process gone". This thread prints a counter every
 * second no matter what the main loop is doing, so one run answers it.
 */
#include "sb_main.inc"
