#include <assert.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <threads.h>

#include "hp.h"

#define NELEMENTS 1024
#define NTHREADS 8

#define DeleteNode(n) free(n);

/* PUBLIC */

typedef uintptr_t ll_key_t;
typedef struct ll_node ll_node_t;
typedef struct ll_list ll_list_t;

ll_list_t *
ll_list_new(void);
void
ll_list_destroy(ll_list_t *);
bool
ll_list_insert(ll_list_t *list, ll_key_t key);
bool
ll_list_delete(ll_list_t *list, ll_key_t key);
bool
ll_list_contains(ll_list_t *list, ll_key_t key);

/* PRIVATE */

#define HP_NEXT 0
#define HP_CURR 1
#define HP_PREV 2

#define ALIGNMENT 128

/* Santa's Little Helpers */

#define is_marked(p) (bool)((uintptr_t)(p) & 0x01)
#define get_marked(p) ((uintptr_t)(p) | (0x01))
#define get_unmarked(p) ((uintptr_t)(p) & (~0x01))

#define get_marked_node(p) ((ll_node_t *)get_marked(p))
#define get_unmarked_node(p) ((ll_node_t *)get_unmarked(p))

struct ll_node {
	atomic_uintptr_t next;
	ll_key_t key;
};

/* Per list variables */

struct ll_list {
	atomic_uintptr_t head;
	atomic_uintptr_t tail;
	ll_hp_t *hp;
};

ll_node_t *
ll_node_new(ll_key_t key) {
	ll_node_t *node = calloc(1, sizeof(*node));
	assert(node != NULL);
	*node = (ll_node_t){ .key = key };
	return (node);
}

void
ll_node_destroy(ll_node_t *node) {
	free(node);
}

static void
ll__list_node_delete(void *arg) {
	ll_node_t *node = (ll_node_t *)arg;
	ll_node_destroy(node);
}

static bool
ll__list_find(ll_list_t *list, ll_key_t *key, atomic_uintptr_t **par_prev, ll_node_t **par_curr, ll_node_t **par_next) {
	atomic_uintptr_t *prev = NULL;
	ll_node_t *curr = NULL, *next = NULL;

try_again:
	prev = &list->head;
	curr = (ll_node_t *)atomic_load(prev);
	(void)ll_hp_protect_ptr(list->hp, HP_CURR, (uintptr_t)curr);
	if (atomic_load(prev) != get_unmarked(curr)) {
		goto try_again;
	}
	while (true) {
		if (get_unmarked_node(curr) == NULL) {
			return false;
		}
		next = (ll_node_t *)atomic_load(&get_unmarked_node(curr)->next);
		(void)ll_hp_protect_ptr(list->hp, HP_NEXT, get_unmarked(next));
		if (atomic_load(&get_unmarked_node(curr)->next) != (uintptr_t)next) {
			break;
		}
		if (get_unmarked(next) == atomic_load((atomic_uintptr_t *)&list->tail)) {
			break;
		}
		if (atomic_load(prev) != get_unmarked(curr)) {
			goto try_again;
		}
		if (get_unmarked_node(next) == next) {
			if (!(get_unmarked_node(curr)->key < *key)) {
				*par_curr = curr;
				*par_prev = prev;
				*par_next = next;
				return (get_unmarked_node(curr)->key == *key);
			}
			prev = (atomic_uintptr_t *)&get_unmarked_node(curr)->next;
			(void)ll_hp_protect_release(list->hp, HP_PREV, get_unmarked(curr));
		} else {
			uintptr_t tmp = get_unmarked(curr);
			if (!atomic_compare_exchange_strong(prev, &tmp, get_unmarked(next))) {
				goto try_again;
			}
			ll_hp_retire(list->hp, get_unmarked(curr));
		}
		curr = next;
		(void)ll_hp_protect_release(list->hp, HP_CURR, get_unmarked(next));
	}
	*par_curr = curr;
	*par_prev = prev;
	*par_next = next;

	return false;
}

/* PUBLIC */

bool
ll_list_insert(ll_list_t *list, ll_key_t key) {
	ll_node_t *curr = NULL, *next = NULL;
	atomic_uintptr_t *prev = NULL;

	ll_node_t *node = ll_node_new(key);

	while (true) {
		if (ll__list_find(list, &key, &prev, &curr, &next)) {
			ll_node_destroy(node);
			ll_hp_clear(list->hp);
			return false;
		}
		atomic_store_explicit(&node->next, (uintptr_t)curr, memory_order_relaxed);
		uintptr_t tmp = get_unmarked(curr);
		if (atomic_compare_exchange_strong(prev, &tmp, (uintptr_t)node)) {
			ll_hp_clear(list->hp);
			return true;
		}
	}
}

bool
ll_list_delete(ll_list_t *list, ll_key_t key) {
	ll_node_t *curr, *next;
	atomic_uintptr_t *prev;
	while (true) {
		if (!ll__list_find(list, &key, &prev, &curr, &next)) {
			ll_hp_clear(list->hp);
			return false;
		}

		uintptr_t tmp = get_unmarked(next);

		if (!atomic_compare_exchange_strong(&curr->next, &tmp, get_marked(next))) {
			continue;
		}

		if (atomic_compare_exchange_strong(prev, &tmp, get_unmarked(next))) {
			ll_hp_clear(list->hp);
			ll_hp_retire(list->hp, get_unmarked(curr));
		} else {
			/* ll__list_find(list, &key, &prev, &curr, &next); */
			ll_hp_clear(list->hp);
		}
		return true;
	}
}

bool
ll_list_contains(ll_list_t *list, ll_key_t key) {
	ll_node_t *curr, *next;
	atomic_uintptr_t *prev;
	bool result = ll__list_find(list, &key, &prev, &curr, &next);
	ll_hp_clear(list->hp);
	return result;
}

ll_list_t *
ll_list_new(void) {
	ll_list_t *list = calloc(1, sizeof(*list));
	ll_node_t *head = ll_node_new(0);
	ll_node_t *tail = ll_node_new(UINTPTR_MAX);
	ll_hp_t *hp = ll_hp_new(3, ll__list_node_delete);

	assert(list != NULL);
	assert(head != NULL);
	assert(tail != NULL);
	atomic_init(&head->next, (uintptr_t)tail);
	*list = (ll_list_t){
		.hp = hp
	};
	atomic_init(&list->head, (uintptr_t)head);
	atomic_init(&list->tail, (uintptr_t)tail);

	return list;
}

void
ll_list_destroy(ll_list_t *list) {
	assert(list != NULL);
	ll_hp_destroy(list->hp);
	ll_node_destroy((ll_node_t *)list->head);
	ll_node_destroy((ll_node_t *)list->tail);
	free(list);
}

static atomic_uint_fast32_t deletes = 0;
static atomic_uint_fast32_t inserts = 0;

static uintptr_t elements[NTHREADS + 1][NELEMENTS];

#define TID_UNKNOWN -1

static atomic_int_fast32_t tid_v_base = ATOMIC_VAR_INIT(0);

static thread_local int tid_v = TID_UNKNOWN;

static inline int
tid(void) {
	if (tid_v == TID_UNKNOWN) {
		tid_v = atomic_fetch_add(&tid_v_base, 1);
		assert(tid_v < NTHREADS + 1);
	}

	return (tid_v);
}

static void *
insert_thread(void *arg) {
	ll_list_t *list = (ll_list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		ll_list_insert(list, elements[tid()][i]);
		(void)atomic_fetch_add(&inserts, 1);
	}
	return NULL;
}

static void *
delete_thread(void *arg) {
	ll_list_t *list = (ll_list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		(void)atomic_fetch_add(&deletes, 1);
		ll_list_delete(list, elements[tid()][i]);
	}
	return NULL;
}

int
main(void) {
	ll_list_t *list = ll_list_new();

	/* insert_thread(list); */

	size_t nthreads = NTHREADS;
	if (nthreads > 0) {
		pthread_t threads[nthreads];

		assert(nthreads / 2 >= 1);
		assert(nthreads % 2 == 0);

		for (size_t i = 0; i < nthreads; i++) {
			switch (i % 2) {
			case 0: pthread_create(&threads[i], NULL, insert_thread, list); break;
			case 1: pthread_create(&threads[i], NULL, delete_thread, list); break;
			default: assert(0);
			}
		}

		for (size_t i = 0; i < nthreads; i++) {
			pthread_join(threads[i], NULL);
		}
	}

	delete_thread(list);
	delete_thread(list);

	ll_list_destroy(list);

	fprintf(stderr, "inserts = %zu, deletes = %zu\n", atomic_load(&inserts), atomic_load(&deletes));

	return (0);
}
