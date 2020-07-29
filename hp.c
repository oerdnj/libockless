/*
 * Copyright (C) Internet Systems Consortium, Inc. ("ISC")
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * See the COPYRIGHT file distributed with this work for additional
 * information regarding copyright ownership.
 */

/*
 * Hazard Pointer implementation.
 *
 * This work is based on C++ code available from:
 * https://github.com/pramalhe/ConcurrencyFreaks/
 *
 * Copyright (c) 2014-2016, Pedro Ramalhete, Andreia Correia
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Concurrency Freaks nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER>
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <assert.h>
#include <inttypes.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <threads.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "hp.h"

#define HP_MAX_THREADS 128
static int ll__hp_max_threads = HP_MAX_THREADS;
#define HP_MAX_HPS     5 /* This is named 'K' in the HP paper */
#define CLPAD	       (128 / sizeof(uintptr_t))
#define HP_THRESHOLD_R 0 /* This is named 'R' in the HP paper */

/* Maximum number of retired objects per thread */
static int ll__hp_max_retired = HP_MAX_THREADS * HP_MAX_HPS;

#define TID_UNKNOWN -1

static atomic_int_fast32_t tid_v_base = ATOMIC_VAR_INIT(0);

static thread_local int tid_v = TID_UNKNOWN;

typedef struct retirelist {
	int size;
	uintptr_t *list;
} retirelist_t;

struct ll_hp {
	int max_hps;
	alignas(128) atomic_uintptr_t *hp[HP_MAX_THREADS];
	alignas(128) retirelist_t *rl[HP_MAX_THREADS*CLPAD];
	ll_hp_deletefunc_t *deletefunc;
};

static inline int
tid(void) {
	if (tid_v == TID_UNKNOWN) {
		tid_v = atomic_fetch_add(&tid_v_base, 1);
		assert(tid_v < ll__hp_max_threads);
	}

	return (tid_v);
}

void
ll_hp_init(int max_threads) {
	ll__hp_max_threads = max_threads;
	ll__hp_max_retired = max_threads * HP_MAX_HPS;
}

ll_hp_t *
ll_hp_new(size_t max_hps, ll_hp_deletefunc_t *deletefunc) {
	ll_hp_t *hp = calloc(1, sizeof(*hp));

	if (max_hps == 0) {
		max_hps = HP_MAX_HPS;
	}

	*hp = (ll_hp_t){ .max_hps = max_hps, .deletefunc = deletefunc };

	for (int i = 0; i < ll__hp_max_threads; i++) {
		hp->hp[i] = calloc(CLPAD * 2, sizeof(hp->hp[i][0]));
		hp->rl[i*CLPAD] = calloc(1, sizeof(*hp->rl[0]));
		for (int j = 0; j < hp->max_hps; j++) {
			atomic_init(&hp->hp[i][j], 0);
		}
		hp->rl[i*CLPAD]->list = calloc(ll__hp_max_retired, sizeof(uintptr_t));
	}

	return (hp);
}

void
ll_hp_destroy(ll_hp_t *hp) {
	for (int i = 0; i < ll__hp_max_threads; i++) {
		free(hp->hp[i]);
		retirelist_t *rl = hp->rl[i*CLPAD];
		for (int j = 0; j < rl->size; j++) {
			void *data = (void *)rl->list[j];
			hp->deletefunc(data);
		}
		free(rl->list);
		free(rl);
	}
	free(hp);
}

void
ll_hp_clear(ll_hp_t *hp) {
	for (int i = 0; i < hp->max_hps; i++) {
		atomic_store_explicit(&hp->hp[tid()][i], 0, memory_order_release);
	}
}

void
ll_hp_clear_one(ll_hp_t *hp, int ihp) {
	atomic_store_explicit(&hp->hp[tid()][ihp], 0, memory_order_release);
}

uintptr_t
ll_hp_protect(ll_hp_t *hp, int ihp, atomic_uintptr_t *atom) {
	uintptr_t n = 0;
	uintptr_t ret;
	while ((ret = atomic_load(atom)) != n) {
		atomic_store(&hp->hp[tid()][ihp], ret);
		n = ret;
	}
	return (ret);
}

uintptr_t
ll_hp_protect_ptr(ll_hp_t *hp, int ihp, uintptr_t ptr) {
	atomic_store(&hp->hp[tid()][ihp], ptr);
	return (ptr);
}

uintptr_t
ll_hp_protect_release(ll_hp_t *hp, int ihp, uintptr_t ptr) {
	atomic_store_explicit(&hp->hp[tid()][ihp], ptr, memory_order_release);
	return (ptr);
}

void
ll_hp_retire(ll_hp_t *hp, uintptr_t ptr) {
	retirelist_t *rl = hp->rl[tid()*CLPAD];
	rl->list[rl->size++] = ptr;
	assert(rl->size < ll__hp_max_retired);

	if (rl->size < HP_THRESHOLD_R) {
		return;
	}

	for (size_t iret = 0; iret < rl->size; iret++) {
		uintptr_t obj = rl->list[iret];
		bool can_delete = true;
		for (int itid = 0; itid < ll__hp_max_threads && can_delete; itid++) {
			for (int ihp = hp->max_hps - 1; ihp >= 0; ihp--) {
				if (atomic_load(&hp->hp[itid][ihp]) == obj) {
					can_delete = false;
					break;
				}
			}
		}

		if (can_delete) {
			size_t bytes = (rl->size - iret) * sizeof(rl->list[0]);
			memmove(&rl->list[iret], &rl->list[iret + 1], bytes);
			rl->size--;
			hp->deletefunc((void *)obj);
		}
	}
}
