// SPDX-License-Identifier: MIT
/*
 * Simple queue implementation
 *
 * Copyright 2024 RnD Center "ELVEES", JSC
 */

#ifndef __QUEUE_H__
#define __QUEUE_H__
#include <assert.h>
#include <search.h>
#include <stddef.h>

#define QUEUE_CONTAINER_OF(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))

typedef struct _q_elem_t
{
	struct _q_elem_t *forward;
	struct _q_elem_t *backward;
} q_elem_t;

static inline void INITIALISE_QUEUE_HEAD(q_elem_t *head)
{
	head->forward = head->backward = head;
	insque(head, head);
}

static inline int queue_is_empty(q_elem_t *head)
{
	return head->forward == head && head->backward == head;
}

static inline void queue_enqueue(q_elem_t *head, q_elem_t *element)
{
	assert(element->forward == NULL && element->backward == NULL);

	insque(element, head->backward);
}

static inline void queue_dequeue(q_elem_t *element)
{
	remque(element);

	element->forward = NULL;
	element->backward = NULL;
}

#endif
