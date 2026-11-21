#ifndef SB_TRANSPORT_H
#define SB_TRANSPORT_H

struct sb_output {
    struct sb_output *next;
    unsigned char *data;
    size_t size;
    size_t sent;
    int nfds;
    int fds[SB_MAXFDS];
};

struct sb_io {
    struct sb_hdr header;
    size_t header_used;
    unsigned char *body;
    size_t body_used;
    size_t body_cap;
    int nfds;
    int fds[SB_MAXFDS];
    struct sb_output *head;
    struct sb_output *tail;
    size_t queued;
    int error;
};

#endif
