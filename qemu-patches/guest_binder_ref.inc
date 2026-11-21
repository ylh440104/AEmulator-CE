    case SB_R_REF: {
        if (h->len < sizeof *h || (h->len - sizeof *h) % sizeof(struct sb_ptr_cookie)) {
            return out_put(o, BR_FAILED_REPLY, NULL, 0);
        }
        size_t count = (h->len - sizeof *h) / sizeof(struct sb_ptr_cookie);
        size_t needed = count * (4 + sizeof(struct binder_ptr_cookie));
        if (needed > o->cap - o->len) return -1;
        for (size_t i = 0; i < count; i++) {
            struct sb_ptr_cookie pc;
            memcpy(&pc, body + i * sizeof pc, sizeof pc);
            if (pc.op > SB_REF_DECREFS) return out_put(o, BR_FAILED_REPLY, NULL, 0);
        }
        for (size_t i = 0; i < count; i++) {
            struct sb_ptr_cookie pc;
            memcpy(&pc, body + i * sizeof pc, sizeof pc);
            struct binder_ptr_cookie b = {
                .ptr = (binder_uintptr_t)pc.ptr,
                .cookie = (binder_uintptr_t)pc.cookie
            };
            uint32_t cmd = pc.op == SB_REF_INCREFS ? BR_INCREFS :
                           pc.op == SB_REF_ACQUIRE ? BR_ACQUIRE :
                           pc.op == SB_REF_RELEASE ? BR_RELEASE : BR_DECREFS;
            out_put(o, cmd, &b, sizeof b);
        }
        return 0;
    }