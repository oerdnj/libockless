#include <assert.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>

#define NTHREADS 256U
#define NELEMENTS 64U

/* PUBLIC API */

typedef uint_fast32_t keytype_t;
typedef struct node node_t;
typedef struct list list_t;

list_t *
list_new(void);
void
list_destroy(list_t **list);
bool
list_insert(list_t *list, keytype_t key);
bool
list_delete(list_t *list, keytype_t search_key);
bool
list_find(list_t *list, keytype_t search_key);

/* PRIVATE */

#define CAS(p, e, v) atomic_compare_exchange_weak(p, &(uintptr_t){ (uintptr_t)e }, (uintptr_t)v)

#define is_marked_reference(p) (bool)((uintptr_t)(p) & 0x01)
#define get_marked_reference(p) (node_t *)((uintptr_t)(p) | (0x01))
#define get_unmarked_reference(p) (node_t *)((uintptr_t)(p) & ~(0x01))

struct node {
	keytype_t key;
	atomic_uintptr_t next;
};

struct list {
	alignas(64) node_t *head, *tail;
	alignas(64) node_t __storage[2];
};

static node_t *
search(list_t *, keytype_t, node_t **);

list_t *
list_new(void) {
	list_t *list = calloc(1, sizeof(*list));
	list->head = &list->__storage[0];
	list->tail = &list->__storage[1];
	atomic_init(&list->head->next, (uintptr_t)list->tail);
	return list;
}

void
list_destroy(list_t **listp) {
	assert(listp != NULL);
	list_t *list = *listp;
	*listp = NULL;

	if (list == NULL) {
		return;
	}
	free(list);
}

static node_t *
node_new(keytype_t key) {
	node_t *node = calloc(1, sizeof(*node));
	node->key = key;
	return node;
}

static void
node_destroy(node_t **nodep) {
	assert(nodep != NULL);
	node_t *node = *nodep;
	*nodep = node;

	if (node == NULL) {
		return;
	}
	free(node);
}

static node_t *
search(list_t *list, keytype_t search_key, node_t **left_node) {
	node_t *left_node_next, *right_node;

	assert(left_node != NULL);
	assert(*left_node == NULL);

search_again:
	for (;;) {
		node_t *t = list->head;
		node_t *t_next = (node_t *)atomic_load(&list->head->next);
		do {
			if (!is_marked_reference(t_next)) {
				(*left_node) = t;
				left_node_next = t_next;
			}
			t = (node_t *)get_unmarked_reference(t_next);
			if (t == list->tail) {
				break;
			}
			t_next = (node_t *)atomic_load(&t->next);
		} while (is_marked_reference(t_next) || (t->key < search_key));
		right_node = t;

		if (left_node_next == right_node) {
			if ((right_node != list->tail) && is_marked_reference(right_node->next)) {
				goto search_again;
			} else {
				return right_node;
			}
		}

		assert(*left_node != NULL);
		if (CAS(&(*left_node)->next, left_node_next, right_node)) {
			if ((right_node != list->tail) && is_marked_reference(atomic_load(&right_node->next))) {
				goto search_again;
			} else {
				return right_node;
			}
		}
	}
}

bool
list_insert(list_t *list, keytype_t key) {
	node_t *new_node = node_new(key);
	for (;;) {
		node_t *right_node, *left_node = NULL;
		right_node = search(list, key,  &left_node);
		if ((right_node != list->tail) && (right_node->key == key)) {
			return false;
		}
		atomic_store(&new_node->next, (uintptr_t)right_node);
		if (CAS(&left_node->next, right_node, new_node)) {
			return true;
		}
	}
}

bool
list_delete(list_t *list, keytype_t search_key) {
	node_t *right_node, *right_node_next, *left_node = NULL;

	for (;;) {
		right_node = search(list, search_key, &left_node);
		if ((right_node == list->tail) || (right_node->key != search_key)) {
			return false;
		}
		right_node_next = (node_t *)atomic_load(&right_node->next);
		if (!is_marked_reference(right_node_next)) {
			if (CAS(&right_node->next, right_node_next, get_marked_reference(right_node_next))) {
				break;
			}
		}
	}
	if (!CAS(&left_node->next, right_node, right_node_next)) {
		right_node = search(list, right_node->key, &left_node);
	}
	return true;
}

bool
list_find(list_t *list, keytype_t key) {
	node_t *right_node, *left_node = NULL;

	right_node = search(list, key, &left_node);
	if ((right_node == list->tail) || (right_node->key != key)) {
		return false;
	}
	return true;
}

static atomic_uint_fast32_t deletes = 0;
static atomic_uint_fast32_t inserts = 0;

static void *
insert_thread(void *arg) {
	list_t *list = (list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		if (list_insert(list, i)) {
			atomic_fetch_add(&inserts, 1);
		}
	}
	return NULL;
}

static void *
delete_thread(void *arg) {
	list_t *list = (list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		if (list_delete(list, i)) {
			atomic_fetch_add(&deletes, 1);
		}
	}
	return NULL;
}

int
main(void) {
	list_t *list = list_new();

	insert_thread(list);

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

	assert((node_t *)list->head->next == list->tail);

	fprintf(stderr, "expected = %u, inserts = %zu, deletes = %zu\n", NELEMENTS * NTHREADS / 2, atomic_load(&inserts), atomic_load(&deletes));

	list_destroy(&list);
}
