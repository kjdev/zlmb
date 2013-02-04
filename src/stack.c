#include <stdio.h>
#include <stdlib.h>

#include "stack.h"

zlmb_stack_item_t *
zlmb_stack_item_init(void)
{
    zlmb_stack_item_t *self;

    self = (zlmb_stack_item_t *)malloc(sizeof(zlmb_stack_item_t));
    if (!self) {
        return NULL;
    }

    self->next = NULL;
    self->prev = NULL;
    self->data = NULL;

    return self;
}

void
zlmb_stack_item_destroy(zlmb_stack_item_t **self)
{
    if (*self) {
        free(*self);
        *self = NULL;
    }
}

zlmb_stack_item_t *
zlmb_stack_item_next(zlmb_stack_item_t *self)
{
    if (!self) {
        return NULL;
    }
    return self->next;
}

zlmb_stack_item_t *
zlmb_stack_item_prev(zlmb_stack_item_t *self)
{
    if (!self) {
        return NULL;
    }
    return self->prev;
}

void *
zlmb_stack_item_data(zlmb_stack_item_t *self)
{
    if (!self) {
        return NULL;
    }
    return self->data;
}

zlmb_stack_t *
zlmb_stack_init(void)
{
    zlmb_stack_t *self;

    self = (zlmb_stack_t *)malloc(sizeof(zlmb_stack_t));
    if (!self) {
        return NULL;
    }

    self->head = NULL;
    self->tail = NULL;
    self->size = 0;

    return self;
}

void
zlmb_stack_destroy(zlmb_stack_t **self)
{
    if (*self) {
        zlmb_stack_item_t *item, *next;
        for (item = (*self)->head; item != NULL; item = next) {
            next = item->next;
            zlmb_stack_item_destroy(&item);
        }
        free(*self);
        *self = NULL;
    }
}

size_t
zlmb_stack_size(zlmb_stack_t *self)
{
    if (!self) {
        return 0;
    }
    return self->size;
}

int
zlmb_stack_unshift(zlmb_stack_t *self, void *data)
{
    zlmb_stack_item_t *item;

    if (!self || !data) {
        return -1;
    }

    item = zlmb_stack_item_init();
    if (!item) {
        return -1;
    }

    item->data = data;

    if (!self->tail) {
        self->tail = item;
    }

    if (self->head) {
        self->head->prev = item;
        item->next = self->head;
    }

    self->head = item;
    self->size++;

    return 0;
}

void *
zlmb_stack_shift(zlmb_stack_t *self)
{
    zlmb_stack_item_t *item;
    void *data;

    if (!self) {
        return NULL;
    }

    item = self->head;
    if (!item) {
        return NULL;
    }

    data = item->data;

    self->head = item->next;
    if (self->head) {
        self->head->prev = NULL;
    }

    zlmb_stack_item_destroy(&item);
    self->size--;

    if (self->size <= 0) {
        self->head = NULL;
        self->tail = NULL;
        self->size = 0;
    }

    return data;
}

int
zlmb_stack_push(zlmb_stack_t *self, void *data)
{
    zlmb_stack_item_t *item;

    if (!self || !data) {
        return -1;
    }

    item = zlmb_stack_item_init();
    if (!item) {
        return -1;
    }

    item->data = data;

    if (!self->head) {
        self->head = item;
    }

    if (self->tail) {
        self->tail->next = item;
        item->prev = self->tail;
    }

    self->tail = item;
    self->size++;

    return 0;
}

void *
zlmb_stack_pop(zlmb_stack_t *self)
{
    zlmb_stack_item_t *item;
    void *data;

    if (!self) {
        return NULL;
    }

    item = self->tail;
    if (!item) {
        return NULL;
    }

    data = item->data;

    self->tail = item->prev;
    if (self->tail) {
        self->tail->next = NULL;
    }

    zlmb_stack_item_destroy(&item);
    self->size--;

    if (self->size <= 0) {
        self->head = NULL;
        self->tail = NULL;
        self->size = 0;
    }

    return data;
}

zlmb_stack_item_t *
zlmb_stack_first(zlmb_stack_t *self)
{
    zlmb_stack_item_t *item;

    if (!self) {
        return NULL;
    }

    item = self->head;
    if (!item) {
        return NULL;
    }

    return item;
}

zlmb_stack_item_t *
zlmb_stack_last(zlmb_stack_t *self)
{
    zlmb_stack_item_t *item;

    if (!self) {
        return NULL;
    }

    item = self->tail;
    if (!item) {
        return NULL;
    }

    return item;
}
