#ifndef __ZLMB_STACK_H__
#define __ZLMB_STACK_H__

typedef struct zlmb_stack_item zlmb_stack_item_t;
struct zlmb_stack_item {
    zlmb_stack_item_t *next;
    zlmb_stack_item_t *prev;
    void *data;
};

zlmb_stack_item_t * zlmb_stack_item_init(void);
void zlmb_stack_item_destroy(zlmb_stack_item_t **self);
zlmb_stack_item_t * zlmb_stack_item_next(zlmb_stack_item_t *self);
zlmb_stack_item_t * zlmb_stack_item_prev(zlmb_stack_item_t *self);
void * zlmb_stack_item_data(zlmb_stack_item_t *self);

typedef struct zlmb_stack {
    zlmb_stack_item_t *head;
    zlmb_stack_item_t *tail;
    size_t size;
} zlmb_stack_t;

zlmb_stack_t * zlmb_stack_init(void);
void zlmb_stack_destroy(zlmb_stack_t **self);
size_t zlmb_stack_size(zlmb_stack_t *self);
int zlmb_stack_unshift(zlmb_stack_t *self, void *data);
void * zlmb_stack_shift(zlmb_stack_t *self);
int zlmb_stack_push(zlmb_stack_t *self, void *data);
void * zlmb_stack_pop(zlmb_stack_t *self);
zlmb_stack_item_t * zlmb_stack_first(zlmb_stack_t *self);
zlmb_stack_item_t * zlmb_stack_last(zlmb_stack_t *self);

#endif
