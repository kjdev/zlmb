#ifndef __ZLMB_DUMP_H__
#define __ZLMB_DUMP_H__

#include <stdio.h>
#include <zmq.h>

#define ZLMB_DUMP_TYPE_BINARY         (1<<0)
#define ZLMB_DUMP_TYPE_PLAIN          (1<<1)
#define ZLMB_DUMP_TYPE_PLAIN_DATETIME (1<<2)
#define ZLMB_DUMP_TYPE_PLAIN_FLAGS    (1<<3)

typedef struct zlmb_dump {
    int type;
    const char *filename;
    size_t size;
    FILE *fp;
} zlmb_dump_t;

zlmb_dump_t * zlmb_dump_init(const char *filename, int type);
void zlmb_dump_destroy(zlmb_dump_t **self);
int zlmb_dump_write(zlmb_dump_t *self, zmq_msg_t *zmsg, int flags);
int zlmb_dump_close(zlmb_dump_t *self);
int zlmb_dump_read_open(zlmb_dump_t *self);
int zlmb_dump_read(zlmb_dump_t *self, zmq_msg_t *zmsg);
int zlmb_dump_truncate(zlmb_dump_t *self);

void zlmb_dump_print(FILE *out, char *buf, size_t len);

#define zlmb_dump_printmsg(_out, _msg) zlmb_dump_print(_out, (char *)zmq_msg_data(_msg), zmq_msg_size(_msg))

#endif
