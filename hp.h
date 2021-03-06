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

#pragma once

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>

/*%
 * Hazard pointers are a mechanism for protecting objects in memory
 * from being deleted by other threads while in use. This allows
 * safe lock-free data structures.
 *
 * This is an adaptation of the ConcurrencyFreaks implementation in C.
 * More details available at https://github.com/pramalhe/ConcurrencyFreaks,
 * in the file HazardPointers.hpp.
 */

typedef struct ll_hp ll_hp_t;

typedef void(ll_hp_deletefunc_t)(void *);

void
ll_hp_init(int max_threads);
/*%<
 * Initialize hazard pointer constants - ll__hp_max_threads. If more threads
 * will try to access hp it will assert.
 */

ll_hp_t *
ll_hp_new(size_t max_hps, ll_hp_deletefunc_t *deletefunc);
/*%<
 * Create a new hazard pointer array of size 'max_hps' (or a reasonable
 * default value if 'max_hps' is 0). The function 'deletefunc' will be
 * used to delete objects protected by hazard pointers when it becomes
 * safe to retire them.
 */

void
ll_hp_destroy(ll_hp_t *hp);
/*%<
 * Destroy a hazard pointer array and clean up all objects protected
 * by hazard pointers.
 */

void
ll_hp_clear(ll_hp_t *hp);
/*%<
 * Clear all hazard pointers in the array for the current thread.
 *
 * Progress condition: wait-free bounded (by max_hps)
 */

void
ll_hp_clear_one(ll_hp_t *hp, int ihp);
/*%<
 * Clear a specified hazard pointer in the array for the current thread.
 *
 * Progress condition: wait-free population oblivious.
 */

uintptr_t
ll_hp_protect(ll_hp_t *hp, int ihp, atomic_uintptr_t *atom);
/*%<
 * Protect an object referenced by 'atom' with a hazard pointer for the
 * current thread.
 *
 * Progress condition: lock-free.
 */

uintptr_t
ll_hp_protect_ptr(ll_hp_t *hp, int ihp, uintptr_t ptr);
/*%<
 * This returns the same value that is passed as ptr, which is sometimes
 * useful.
 *
 * Progress condition: wait-free population oblivious.
 */

uintptr_t
ll_hp_protect_release(ll_hp_t *hp, int ihp, uintptr_t ptr);
/*%<
 * Same as ll_hp_protect_ptr(), but explicitly uses memory_order_release.
 *
 * Progress condition: wait-free population oblivious.
 */

void
ll_hp_retire(ll_hp_t *hp, uintptr_t ptr);
/*%<
 * Retire an object that is no longer in use by any thread, calling
 * the delete function that was specified in ll_hp_new().
 *
 * Progress condition: wait-free bounded (by the number of threads squared)
 */
