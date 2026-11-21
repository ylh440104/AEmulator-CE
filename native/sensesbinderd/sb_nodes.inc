static int emit_node(struct node *n, u32 *br) {
    if (!n || !n->owner || !n->ptr) return 0;
    int strong = n->local_strong > 0;
    for (struct ref *r = n->refs; r; r = r->next_in_node)
        if (r->strong > 0) strong = 1;
    n->internal_strong = strong;
    int weak = n->nrefs > 0 || n->local_weak > 0 || strong;
    u32 cmds[4];
    int count = 0;
    if (weak && !n->has_weak) {
        n->has_weak = 1;
        n->pend_weak = 1;
        cmds[count++] = BR_INCREFS;
    }
    if (strong && !n->has_strong) {
        n->has_strong = 1;
        n->pend_strong = 1;
        cmds[count++] = BR_ACQUIRE;
    }
    if (!strong && n->has_strong && !n->pend_strong) {
        n->has_strong = 0;
        cmds[count++] = BR_RELEASE;
    }
    if (!weak && n->has_weak && !n->pend_weak && !n->has_strong) {
        n->has_weak = 0;
        cmds[count++] = BR_DECREFS;
    }
    for (int i = 0; i < count; i++) {
        br[i * 5] = cmds[i];
        memcpy(br + i * 5 + 1, &n->ptr, 8);
        memcpy(br + i * 5 + 3, &n->cookie, 8);
    }
    return count * 5;
}