#include <assert.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NELEMENTS 4096
#define NTHREADS 4

typedef atomic_uintptr_t link_t;

#define is_marked(p) (bool)((uintptr_t)(p) & 0x01)
#define get_marked(p) ((uintptr_t)(p) | (0x01))
#define get_unmarked(p) ((uintptr_t)(p) & ~(0x01))

#define is_claimed(n) (bool)(((uintptr_t)(n)->refct_claim & 0x01) == 0x01)

typedef struct Node {
	atomic_uint_fast32_t refct_claim;
	void *value;
	alignas(64) link_t prev, next; /* Should be aligned to cache pipeline size */
} node_t;

typedef struct List {
	node_t heads, tails;
	node_t *head, *tail;
} list_t;

node_t *head, *tail;

#if defined(NDEBUG)
#define pnode(name, node, file, line)
#else
#define pnode(name, node, file, line) _pnode(name, node, file, line)
#endif

char *
X(char *buf, size_t len, node_t *node0) {
	bool deleted = is_marked(node0);
	node_t *node = (node_t *)get_unmarked(node0);
	if (deleted) {
		*buf = '*';
		buf++;
		len--;
	}
	if (node == head) {
		snprintf(buf, len, "HEAD");
	} else if (node == tail) {
		snprintf(buf, len, "TAIL");
	} else {
		snprintf(buf, len, "%p", node);
	}
	return buf;
}

__attribute__((unused)) static void _pnode(char *name, node_t *node, char *file, unsigned int line) {
	node_t *__node = (node_t *)get_unmarked(node);
	char node_buf[30] = { 0 };
	char prev_buf[30] = { 0 };
	char next_buf[30] = { 0 };

	fprintf(stderr, "%s:%s:%u:%u: %snode = %s, node->prev = %s, node->next = %s, node->refct_claim = %" PRIuFAST32 "\n",
		name,
		file,
		line,
		(unsigned int)pthread_self(),
		(is_marked(node)) ? "deleted " : "",
		X(node_buf, sizeof(node_buf), node),
		X(prev_buf, sizeof(prev_buf), (node_t *)__node->prev),
		X(next_buf, sizeof(next_buf), (node_t *)__node->next),
		__node->refct_claim);
}

/* function MALLOC_NODE():pointer to Node */
/* function READ_NODE(address:pointer to Link):pointer to Node */
/* function READ_DEL_NODE(address:pointer to Link):pointer to Node */
/* function COPY_NODE(node:pointer to Node):pointer to Node */
/* procedure RELEASE_NODE(node:pointer to Node) */

#if defined(NDEBUG) || 1
#define CheckConsistency(list) true
#else
#define CheckConsistency(list) _CheckConsistency(list)
#endif

__attribute__((unused)) static bool
_CheckConsistency(list_t *list);

/* Valois, Michael, Scott: Memory Management */

__attribute__((unused)) static void
ClearLowestBit(atomic_uint_fast32_t *ptr) {
	uint_fast32_t new = 0;
	uint_fast32_t old = atomic_load(ptr);
	do {
		new = old - 1;
	} while (!atomic_compare_exchange_weak(ptr, &old, new));
}

/* That would be NEW() in Michael&Scott's paper */
/*
 * The function MALLOC_NODE allocates a new node from the memory pool of pre-allocated nodes.
 */
static node_t *
_MALLOC_NODE(char *file, unsigned int line) {
	node_t *p = aligned_alloc(sizeof(void *), sizeof(*p));
	assert(p != NULL);
	*p = (node_t){ .refct_claim = ATOMIC_VAR_INIT(0) };
	/* atomic_init(&p->refct_claim, 3); */
	/* ClearLowestBit(&p->refct_claim); */
	/* Must not be claimed */
	pnode("MALLOC_NODE", p, file, line);
	return p;
}

#define MALLOC_NODE() _MALLOC_NODE(__FILE__, __LINE__)

/*
 * old = 5; new = 3; old - new = 2; result = 0
 * old = 3; new = 1; old - new = 2; result = 0;
 * old = 2; new = 1; old - new = 1; result = 1;
 * old = 1; -> assert
 */
static bool
DecrementAndTestAndSet(atomic_uint_fast32_t *ptr) {
	uint_fast32_t new, old;
	do {
		old = atomic_load(ptr);
		if (old < 2) {
			fprintf(stderr, "DecrementAndTestAndSet: old = %" PRIuFAST32 "\n", old);
			assert(old < 2);
		}
		new = old - 2;
		if (new == 0) {
			new = 1;
		}
	} while (!atomic_compare_exchange_weak(ptr, &old, new));
	return ((old - new) & 0x01) == 0x01;
}

void
ReleaseReferences(node_t *node);

/* That would be RELEASE in the paper */
/*
 * The function RELEASE_NODE decrements the reference counter on the corresponding give node. If the reference counter
 * reaches zero, the function the calls the ReleaseReferences function that will recursively call RELEASE_NODE on the
 * nodes that this node has owned pointers to, and the it reclaims the node.
*/
static void
_RELEASE_NODE(node_t *node, char *file, unsigned int line) {
	assert(node != NULL);
	pnode("RELEASE_NODE", node, file, line);
	assert(!is_marked(node));
	assert(!is_claimed(node));
	if (DecrementAndTestAndSet(&node->refct_claim) == false) {
		return;
	}
	/* MUST BE CLAIMED */
	assert(is_claimed(node));
	ReleaseReferences(node);
	memset(node, 0, sizeof(*node));
	free(node);
}

#define RELEASE_NODE(n) _RELEASE_NODE(n, __FILE__, __LINE__)

/* That would be SAFEREAD() in Michael&Scott's paper */
/*
 * The functions READ_NODE and READ_DEL_NODE atomically de-references the given link and increases the reference coutner
 * for the corresponding node. In case the deletion mark of the link is set, the READ_NODE function then returns NULL.
 */
static node_t *
__READ_NODE(link_t *address, bool allow_marked, char *file, unsigned int line) {
	for (;;) {
		link_t link = atomic_load(address);
		node_t *node = (node_t *)get_unmarked(link);
		assert(node != NULL);
		assert(!is_claimed(node));
		atomic_fetch_add(&node->refct_claim, 2);

		/* The underlying address has not changed meanwhile */
		if (link == atomic_load(address)) {
			if (!allow_marked && is_marked(link)) {
				return NULL;
			}
			return node;
		}
		/* If it did, release the node and retry */
		RELEASE_NODE(node);
	}
}

static inline node_t *
_READ_NODE(link_t *address, char *file, unsigned int line) {
	pnode("READ_NODE", (node_t *)*address, file, line);

	return __READ_NODE(address, false, file, line);
}

static inline node_t *
_READ_DEL_NODE(link_t *address, char *file, unsigned int line) {
	pnode("READ_DEL_NODE", (node_t *)*address, file, line);

	return __READ_NODE(address, true, file, line);
}

#define READ_NODE(a) _READ_NODE(a, __FILE__, __LINE__)
#define READ_DEL_NODE(a) _READ_DEL_NODE(a, __FILE__, __LINE__)

/*
 * The COPY_NODE function increases the reference counter for the corresponding given node.
 */
static node_t *
_COPY_NODE(node_t *node, char *file, unsigned int line) {
	pnode("COPY_NODE", node, file, line);
	assert(!is_marked(node));
	assert(!is_claimed(node));
	atomic_fetch_add(&node->refct_claim, 2);
	return (node);
}

#define COPY_NODE(n) _COPY_NODE(n, __FILE__, __LINE__)

/*
 * C1  node:=MALLOC_NODE();
 * C2  node.value:=value;
 * C3  return node;
 */
node_t *
CreateNode(void *value) {
	node_t *node = MALLOC_NODE(); /* C1 */
	node->value = value; /* C2 */
	return node; /* C3 */
}

/*
 * RR1 RELEASE_NODE(node.prev.p);
 * RR2 RELEASE_NODE(node.next.p);
 */
void
ReleaseReferences(node_t *node) {
	RELEASE_NODE((node_t *)get_unmarked(atomic_load(&node->prev))); /* RR1 */
	RELEASE_NODE((node_t *)get_unmarked(atomic_load(&node->next))); /* RR2 */
}

#define FAA(address, number) atomic_fetch_add_acquire(address, number)

#define CAS(address, oldvalue, newvalue) \
	atomic_compare_exchange_weak(address, &(uintptr_t){ (uintptr_t)oldvalue }, (uintptr_t)newvalue)

static void
_PushCommon(list_t *list, node_t *node, node_t *next, char *file, unsigned int line);
static node_t *
_HelpInsert(list_t *list, node_t *prev, node_t *node, char *file, unsigned int line);
static void
_HelpDelete(list_t *list, node_t *node, char *file, unsigned int line);
static void
_RemoveCrossReference(list_t *list, node_t *node, char *file, unsigned int line);

#define PushCommon(l, n, e) _PushCommon(l, n, e, __FILE__, __LINE__)
#define HelpInsert(l, p, n) _HelpInsert(l, p, n, __FILE__, __LINE__)
#define HelpDelete(l, n) _HelpDelete(l, n, __FILE__, __LINE__)
#define RemoveCrossReference(l, n) _RemoveCrossReference(l, n, __FILE__, __LINE__)

/*
 * PL1  node:=CreateNode(value);
 * PL2  prev:=COPY_NODE(head);
 * PL3  next:=READ_NODE(&prev.next);
 * PL4  while true do
 * PL5    if prev.next != <next,false> then
 * PL6      RELEASE_NODE(next);
 * PL7      next:=READ_NODE(&prev.next);
 * PL8      continue;
 * PL9    node.prev:=<prev,false>
 * PL10   node.next:=<next,false>
 * PL11   if CAS(&prev.next,<next,false>,<node,false>) then
 * PL12     COPY_NODE(node);
 * PL13     break;
 * PL14   Back-Off
 * PL15 PushCommon(node,next);
 */
void
PushLeft(list_t *list, void *value) {
	node_t *node = CreateNode(value); /* PL1 */
	node_t *prev = COPY_NODE(list->head); /* PL2 */
	node_t *next = READ_NODE(&prev->next); /* PL3 */
	for (;;) { /* PL4 */
		if (atomic_load(&prev->next) != get_unmarked(next)) { /* PL5 */
			RELEASE_NODE(next); /* PL6 */
			next = READ_NODE(&prev->next); /* PL7 */
			continue; /* PL8 */
		}
		atomic_init(&node->prev, get_unmarked(prev)); /* PL9 */
		atomic_init(&node->next, get_unmarked(next)); /* PL10 */
		if (CAS(&prev->next, get_unmarked(next), get_unmarked(node))) { /* PL11 */
			COPY_NODE(node); /* PL12 */
			break; /* PL13 */
		}
		sched_yield(); /* PL14 */
	}
	PushCommon(list, node, next); /* PL15 */
}

/*
 * PR1  node:=CreateNode(value);
 * PR2  node:=COPY_NODE(tail);
 * PR3  prev:=READ_NODE(&next.prev);
 * PR4  while true do
 * PR5    if prev.next != <next,false> then
 * PR6      prev:=HelpInsert(prev,next);
 * PR7      continue;
 * PR8    node.prev:=<prev,false>
 * PR9    node.next:=<next,false>
 * PR10   if CAS(&prev.next,<next,false>,<node,false>) then
 * PR11     COPY_NODE(node);
 * PR12     break;
 * PR13   Back-Off
 * PR14 PushCommon(node,next);
 */
void
PushRight(list_t *list, void *value) {
	node_t *node = CreateNode(value); /* PR1 */
	node_t *next = COPY_NODE(list->tail); /* PR2 */
	node_t *prev = READ_NODE(&next->prev); /* PR3 */
	for (;;) { /* PR4 */
		if (atomic_load(&prev->next) != get_unmarked(next)) { /* PR5 */
			prev = HelpInsert(list, prev, next); /* PR6 */
			continue; /* PR7 */
		}
		atomic_init(&node->prev, get_unmarked(prev)); /* PR8 */
		atomic_init(&node->next, get_unmarked(next)); /* PR9 */
		if (CAS(&prev->next, get_unmarked(next), get_unmarked(node))) { /* PR10 */
			COPY_NODE(node); /* PR11 */
			break; /* PR12 */
		}
		sched_yield(); /* PR13 */
	}
	PushCommon(list, node, next); /* PR14 */
}

/*
 * PC1  while true do
 * PC2    link1:=next.prev;
 * PC3    if link1.d = true or node.next != <next,false> then
 * PC4      break;
 * PC5    if CAS(&next.prev,link1,<node,false>) then
 * PC6      COPY_NODE(node);
 * PC7      RELEASE_NODE(link1.p);
 * PC8      if node.prev.d = true then
 * PC9        prev2:=COPY_NODE(node);
 * PC10       prev2:=HelpInsert(prev2,next);
 * PC11       RELEASE_NODE(prev2);
 * PC12     break;
 * PC13   Back-Off
 * PC14 RELEASE_NODE(next);
 * PC15 RELEASE_NODE(node);
 */
void
_PushCommon(list_t *list, node_t *node, node_t *next, char *file, unsigned int line) {
	pnode("PushCommon:node", node, file, line);
	pnode("PushCommon:next", next, file, line);
	for (;;) { /* PC1 */
		link_t link1 = atomic_load(&next->prev); /* PC2 */
		if (is_marked(link1) || atomic_load(&node->next) != get_unmarked(next)) { /* PC3 */
			break; /* PC4 */
		}
		if (CAS(&next->prev, link1, get_unmarked(node))) { /* PC5 */
			COPY_NODE(node); /* PC6 */
			RELEASE_NODE((node_t *)get_unmarked(link1)); /* PC7 */
			if (is_marked(atomic_load(&node->prev))) { /* PC8 */
				node_t *prev2 = COPY_NODE(node); /* PC9 */
				prev2 = HelpInsert(list, prev2, next); /* PC10 */
				RELEASE_NODE(prev2); /* PC11 */
			}
			break; /* PC12 */
		}
		sched_yield(); /* PC13 */
	}
	RELEASE_NODE(next); /* PC14 */
	RELEASE_NODE(node); /* PC15 */
}

/*
 * PL1  prev:=COPY_NODE(head);
 * PL2  while true do
 * PL3    node:=READ_NODE(&prev.next);
 * PL4    if node = tail then
 * PL5      RELEASE_NODE(node);
 * PL6      RELEASE_NODE(prev);
 * PL7      return ∅;
 * PL8    link1:=node.next;
 * PL9    if link1.d = true then
 * PL10     HelpDelete(node);
 * PL11     RELEASE_NODE(node);
 * PL12     continue;
 * PL13   if CAS(&node.next,link1,<link1.p,true>) then
 * PL14     HelpDelete(node);
 * PL15     next:=READ_DEL_NODE(&node.next);
 * PL16     prev:=HelpInsert(prev,next);
 * PL17     RELEASE_NODE(prev);
 * PL18     RELEASE_NODE(next);
 * PL19     value:=node.value;
 * PL20     break;
 * PL21   RELEASE_NODE(node);
 * PL22   Back-Off
 * PL23 RemoveCrossReference(node);
 * PL24 RELEASE_NODE(node);
 * PL25 return value;
 */
void *
PopLeft(list_t *list) {
	node_t *node = NULL;
	void *value = NULL;
	node_t *prev = COPY_NODE(list->head); /* PL1 */
	for (;;) { /* PL2 */
		node = READ_NODE(&prev->next); /* PL3 */
		if (node == list->tail) { /* PL4 */
			RELEASE_NODE(node); /* PL5 */
			RELEASE_NODE(prev); /* PL6 */
			return NULL; /* PL7 */
		}
		link_t link1 = atomic_load(&node->next); /* PL8 */
		if (is_marked(link1)) { /* PL9 */
			HelpDelete(list, node); /* PL10 */
			RELEASE_NODE(node); /* PL11 */
			continue; /* PL12 */
		}
		if (CAS(&node->next, link1, get_marked(link1))) { /* PL13 */
			HelpDelete(list, node); /* PL14 */
			node_t *next = READ_DEL_NODE(&node->next); /* PL15 */
			prev = HelpInsert(list, prev, next); /* PL16 */
			RELEASE_NODE(prev); /* PL17 */
			RELEASE_NODE(next); /* PL18 */
			value = node->value; /* PL19 */
			break; /* PL20 */
		}
		RELEASE_NODE(node); /* PL21 */
		sched_yield(); /* PL22 */
	}
	RemoveCrossReference(list, node); /* PL23 */
	RELEASE_NODE(node); /* PL24 */
	return value;
}

/* PR1  next:=COPY_NODE(tail);
 * PR2  node:=READ_NODE(&next.prev);
 * PR3  while true do
 * PR4    if node.next != <next,false> then
 * PR5      node:=HelpInsert(node,next);
 * PR6      continue
 * PR7    if node = head then
 * PR8      RELEASE_NODE(node);
 * PR9      RELEASE_NODE(next);
 * PR10     return ∅;
 * PR11   if CAS(&node.next,<next,false>,<next,true>) then
 * PR12     HelpDelete(node);
 * PR13     prev:=READ_DEL_NODE(&node.prev);
 * PR14     prev:=HelpInsert(prev,next);
 * PR15     RELEASE_NODE(prev);
 * PR16     RELEASE_NODE(next);
 * PR17     value:=node.value;
 * PR18     break;
 * PR19   Back-Off
 * PR20 RemoveCrossReference(node);
 * PR21 RELEASE_NODE(node);
 * PR22 return value;
 */

void *
PopRight(list_t *list) {
	void *value = NULL;

	node_t *next = COPY_NODE(list->tail); /* PR1 */
	node_t *node = READ_NODE(&next->prev); /* PR2 */

	pnode("PopRight:node", node, __FILE__, __LINE__);

	for (;;) { /* PR3 */
		if (atomic_load(&node->next) != get_unmarked(next)) { /* PR4 */
			node = HelpInsert(list, node, next); /* PR5 */
			continue; /* PR6 */
		}
		if (node == list->head) { /* PR7 */
			RELEASE_NODE(node); /* PR8 */
			RELEASE_NODE(next); /* PR9 */
			return NULL; /* PR10 */
		}
		if (CAS(&node->next, get_unmarked(next), get_marked(next))) { /* PR11 */
			HelpDelete(list, node); /* PR12 */
			node_t *prev = READ_DEL_NODE(&node->prev); /* PR13 */
			prev = HelpInsert(list, prev, next); /* PR14 */
			RELEASE_NODE(prev); /* PR15 */
			RELEASE_NODE(next); /* PR16 */
			value = node->value; /* PR17 */
			break; /* PR18 */
		}
		sched_yield(); /* PR19 */
	}
	RemoveCrossReference(list, node); /* PR20 */
	RELEASE_NODE(node); /* PR21 */
	return value; /* PR22 */
}

/*
 * HD1  while true do
 * HD2    link1:=node.prev;
 * HD3    if link1.d = true or
 * HD4      CAS(&node.prev,link1,<link1.p,true>) then break;
 * HD5  lastlink.d:=true;
 * HD6  prev:=READ_DEL_NODE(&node.prev);
 * HD7  next:=READ_DEL_NODE(&node.next);
 * HD8  while true do
 * HD9    if prev = next then break;
 * HD10   if (next.next.d = true) then
 * HD11     next2:=READ_DEL_NODE(&next.next);
 * HD12     RELEASE_NODE(next);
 * HD13     next:=next2;
 * HD14     continue;
 * HD15   prev2:=READ_NODE(&prev.next);
 * HD16   if prev2 = NULL then
 * HD17     if lastlink.d = false then
 * HD18       HelpDelete(prev);
 * HD19       lastlink.d:=true;
 * HD20     prev2:=READ_DEL_NODE(&prev.prev);
 * HD21     RELEASE_NODE(prev);
 * HD22     prev:=prev2;
 * HD23     continue;
 * HD24   if prev2 != node then
 * HD25     lastlink.d:=false;
 * HD26     RELEASE_NODE(prev);
 * HD27     prev:=prev2;
 * HD28     continue;
 * HD29   RELEASE_NODE(prev2);
 * HD30   if CAS(&prev.next,<node,false>,<next,false>) then
 * HD31     COPY_NODE(next);
 * HD32     RELEASE_NODE(node);
 * HD33     break;
 * HD34   Back-Off
 * HD35 RELEASE_NODE(prev);
 * HD36 RELEASE_NODE(next);
 */
void
_HelpDelete(list_t *list, node_t *node, char *file, unsigned int line) {
	pnode("HelpDelete:node", node, file, line);
	for (;;) { /* HD1 */
		link_t link1 = atomic_load(&node->prev); /* HD2 */
		if (is_marked(link1) || /* HD3 */
		    CAS(&node->prev, link1, get_marked(link1))) { /* HD4 */
			break; /* HD4 */
		}
	}
	bool lastlink = true; /* HD5 */
	node_t *prev = READ_DEL_NODE(&node->prev); /* HD6 */
	node_t *next = READ_DEL_NODE(&node->next); /* HD7 */

	for (;;) { /* HD8 */
		if (prev == next) { /* HD9 */
			break; /* HD9 */
		}
		if (is_marked(atomic_load(&next->next))) { /* HD10 */
			node_t *next2 = READ_DEL_NODE(&next->next); /* HD11 */
			RELEASE_NODE(next); /* HD12 */
			next = next2; /* HD13 */
			continue; /* HD14 */
		}
		node_t *prev2 = READ_NODE(&prev->next); /* HD15 */
		if (prev2 == NULL) { /* HD16 */
			if (lastlink == false) { /* HD17 */
				HelpDelete(list, prev); /* HD18 */
				lastlink = true; /* HD19 */
			}
			prev2 = READ_DEL_NODE(&prev->prev); /* HD20 */
			RELEASE_NODE(prev); /* HD21 */
			prev = prev2; /* HD22 */
			continue; /* HD23 */
		}
		if (prev2 != node) { /* HD24 */
			lastlink = false; /* HD25 */
			RELEASE_NODE(prev); /* HD26 */
			prev = prev2; /* HD27 */
			continue; /* HD28 */
		}
		RELEASE_NODE(prev2); /* HD29 */
		if (CAS(&prev->next, get_unmarked(node), get_unmarked(next))) { /* HD30 */
			COPY_NODE(next); /* HD31 */
			RELEASE_NODE(node); /* HD32 */
			break; /* HD33 */
		}
		sched_yield(); /* HD34 */
	}
	RELEASE_NODE(prev); /* HD35 */
	RELEASE_NODE(next); /* HD36 */
}

/*
 * HI1  lastlink.d:=true;
 * HI2  while true do
 * HI3    prev2:=READ_NODE(&prev.next);
 * HI4    if prev2 = NULL then
 * HI5      if lastlink.d = false then
 * HI6        HelpDelete(prev);
 * HI7        lastlink=true;
 * HI8      prev2:=READ_DEL_NODE(&prev.prev);
 * HI9      RELEASE_NODE(prev);
 * HI10     prev:=prev2;
 * HI11     continue;
 * HI12   link1:=node.prev;
 * HI13   if link1.d = true then
 * HI14     RELEASE_NODE(prev2);
 * HI15     break;
 * HI16   if prev2 != node then
 * HI17     lastlink.d:=false;
 * HI18     RELEASE_NODE(prev);
 * HI19     prev:=prev2;
 * HI20     continue;
 * HI21   RELEASE_NODE(prev2);
 * HI22   if CAS(&node.prev,link1,<prev,false>) then
 * HI23     COPY_NODE(prev);
 * HI24     RELEASE_NODE(link1.p);
 * HI25     if prev.prev.d = true then continue;
 * HI26     break;
 * HI27   Back-Off
 * HI28 return prev;
 */

static node_t *
_HelpInsert(list_t *list, node_t *prev, node_t *node, char *file, unsigned int line) {
	pnode("HelpInsert:prev", prev, file, line);
	pnode("HelpInsert:node", node, file, line);
	bool lastlink = true; /* HI1 */
	for (;;) { /* HI2 */
		node_t *prev2 = READ_NODE(&prev->next); /* HI3 */
		if (prev2 == NULL) { /* HI4 */
			if (lastlink == false) { /* HI5 */
				HelpDelete(list, prev); /* HI6 */
				lastlink = true; /* HI7 */
			}
			prev2 = READ_DEL_NODE(&prev->prev); /* HI8 */
			RELEASE_NODE(prev); /* HI9 */
			prev = prev2; /* HI10 */
			continue; /* HI11 */
		}
		/* link_t link1 = atomic_load(&node->prev); /\* HI12 *\/ */
		node_t *link1 = READ_DEL_NODE(&node->prev); /* HI12 fixed */
		if (is_marked(link1)) { /* HI13 */
			RELEASE_NODE((node_t *)get_unmarked(link1)); /* HI24 */
			RELEASE_NODE(prev2); /* HI14 */
			break; /* HI15 */
		}
		if (prev2 != node) { /* HI16 */
			lastlink = false; /* HI17 */
			RELEASE_NODE((node_t *)get_unmarked(link1)); /* HI24 */
			RELEASE_NODE(prev); /* HI18 */
			prev = prev2; /* HI19 */
			continue; /* HI20 */
		}
		RELEASE_NODE(prev2); /* HI21 */
		if (CAS(&node->prev, link1, get_unmarked(prev))) { /* HI22 */
			COPY_NODE(prev); /* HI23 */
			RELEASE_NODE((node_t *)get_unmarked(link1)); /* HI24 */
			if (is_marked(atomic_load(&prev->prev))) { /* HI25 */
				continue; /* HI25 */
			}
			break; /* HI26 */
		}
		sched_yield(); /* HI27 */
	}
	return prev; /* HI28 */
}

/*
 * RC1  while true do
 * RC2    prev:=node.prev.p;
 * RC3    if prev.next.d = true then
 * RC4      prev2:=READ_DEL_NODE(&prev.prev);
 * RC5      node.prev:=<prev2,true>
 * RC6      RELEASE_NODE(prev);
 * RC7      continue;
 * RC8    next:=node.next.p;
 * RC9    if next.next.d = true then
 * RC10     next2:=READ_DEL_NODE(&next.next);
 * RC11     node.next:=<next2,true>
 * RC12     RELEASE_NODE(next);
 * RC13     continue;
 * RC14   break;
 */
static void
_RemoveCrossReference(list_t *list __attribute__((unused)), node_t *node, char *file, unsigned int line) {
	pnode("RemoveCrossReference:node", node, file, line);
	for (;;) { /* RC1 */
		node_t *prev = (node_t *)get_unmarked(atomic_load(&node->prev)); /* RC2 */
		if (is_marked(atomic_load(&prev->next))) { /* RC3 */
			node_t *prev2 = READ_DEL_NODE(&prev->prev); /* RC4 */
			atomic_store(&node->prev, get_marked(prev2)); /* RC5 */
			RELEASE_NODE(prev); /* RC6 */
			continue; /* RC7 */
		}
		node_t *next = (node_t *)get_unmarked(atomic_load(&node->next)); /* RC8 */
		if (is_marked(atomic_load(&next->next))) { /* RC9 */
			node_t *next2 = READ_DEL_NODE(&next->next); /* RC10 */
			atomic_store(&node->next, get_marked(next2)); /* RC11 */
			RELEASE_NODE(next); /* RC12 */
			continue; /* RC13 */
		}
		break; /* RC14 */
	}
}

#define MAGIC (void *)0xdeadbeee

static atomic_uint_fast32_t deletes = 0;
static atomic_uint_fast32_t inserts = 0;

static bool
_CheckConsistency(list_t *list) {
	size_t i = 0;
	node_t *node = list->head;
	/* assert((node_t *)list->head->prev == NULL); */
	/* assert((node_t *)list->tail->next == NULL); */

	while (node != list->tail) {
		__attribute__((unused)) node_t *prev = (node_t *)get_unmarked(node);
		node = (node_t *)get_unmarked(node->next);

		/* if ((node_t *)get_unmarked(node->prev) != prev) { */
		/* 	return false; */
		/* } */
		if (i > (atomic_load(&inserts) + 1 + NTHREADS - atomic_load(&deletes))) {
/*			fprintf(stderr, "%zu %zu %zu %zu\n", i, atomic_load(&inserts), atomic_load(&deletes), inserts + 1 - deletes); */
		}
		assert(i <= atomic_load(&inserts));
		i++;
	}
	node = list->tail;
	i = 0;
	while (node != list->head) {
		__attribute__((unused)) node_t *next = (node_t *)get_unmarked(node);
		node = (node_t *)get_unmarked(node->prev);
		/* if ((node_t *)get_unmarked(node->next) != next) { */
		/* 	return false; */
		/* } */
		if (i > (atomic_load(&inserts) + 1 + NTHREADS - atomic_load(&deletes))) {
/*			fprintf(stderr, "%zu %zu %zu %zu\n", i, atomic_load(&inserts), atomic_load(&deletes), inserts + 1 - deletes); */
		}
		assert(i <= atomic_load(&inserts));
		i++;
	}
	return true;
}

#define dir(x) (x)?"right":"left"

static void *
insert_thread_right(void *arg) {
	list_t *list = (list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		(void)atomic_fetch_add(&inserts, 1);
		PushRight(list, MAGIC);
	}
	return NULL;
}

static void *
delete_thread_right(void *arg) {
	list_t *list = (list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		void *value = PopRight(list);
		if (value == NULL) {
			return NULL;
		}
		(void)atomic_fetch_add(&deletes, 1);
		assert(value == MAGIC);
	}
	return NULL;
}

static void *
insert_thread_left(void *arg) {
	list_t *list = (list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		(void)atomic_fetch_add(&inserts, 1);
		PushLeft(list, MAGIC);
	}
	return NULL;
}

static void *
delete_thread_left(void *arg) {
	list_t *list = (list_t *)arg;

	for (size_t i = 0; i < NELEMENTS; i++) {
		void *value = PopLeft(list);
		if (value == NULL) {
			return NULL;
		}
		(void)atomic_fetch_add(&deletes, 1);
		assert(value == MAGIC);
	}
	return NULL;
}

int
main(void) {
	list_t *list = calloc(1, sizeof(*list));
	list->head = &list->heads;
	list->tail = &list->tails;
	head = list->head;
	tail = list->tail;

	atomic_init(&tail->prev, (uintptr_t)COPY_NODE(head));
	atomic_init(&head->next, (uintptr_t)COPY_NODE(tail));

	atomic_init(&tail->next, (uintptr_t)COPY_NODE(tail));
	atomic_init(&head->prev, (uintptr_t)COPY_NODE(head));

	fprintf(stderr, "IR\n");

	insert_thread_right(list);

	assert(CheckConsistency(list));
	fprintf(stderr, "IL\n");

	insert_thread_left(list);

	size_t nthreads = NTHREADS;
	if (nthreads > 0) {
		pthread_t threads[nthreads];

		assert(nthreads / 4 >= 1);
		assert(nthreads % 4 == 0);

		for (size_t i = 0; i < nthreads; i++) {
			switch (i % 4) {
			case 0: pthread_create(&threads[i], NULL, insert_thread_right, list); break;
			case 1: pthread_create(&threads[i], NULL, delete_thread_left, list); break;
			case 2: pthread_create(&threads[i], NULL, insert_thread_left, list); break;
			case 3: pthread_create(&threads[i], NULL, delete_thread_right, list); break;
			default: assert(0);
			}
		}

		for (size_t i = 0; i < nthreads; i++) {
			pthread_join(threads[i], NULL);
		}
	}

	assert(CheckConsistency(list));
	fprintf(stderr, "DR\n");

	delete_thread_right(list);

	assert(CheckConsistency(list));
	fprintf(stderr, "DL\n");

	delete_thread_left(list);

	fprintf(stderr, "Checking consistency...");
	assert(CheckConsistency(list));
	fprintf(stderr, "done.\n");

	while (PopLeft(list) != NULL) {}

	fprintf(stderr, "Checking consistency...");
	assert(CheckConsistency(list));
	fprintf(stderr, "done.\n");

	free(list);
}
