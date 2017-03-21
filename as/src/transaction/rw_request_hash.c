/*
 * rw_request_hash.c
 *
 * Copyright (C) 2016 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

//==========================================================
// Includes.
//

#include "transaction/rw_request_hash.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "citrusleaf/alloc.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_clock.h"
#include "citrusleaf/cf_rchash.h"

#include "fault.h"
#include "msg.h"
#include "util.h"

#include "base/cfg.h"
#include "base/datamodel.h"
#include "base/proto.h"
#include "base/transaction.h"
#include "base/transaction_policy.h"
#include "fabric/fabric.h"
#include "transaction/duplicate_resolve.h"
#include "transaction/replica_write.h"
#include "transaction/rw_request.h"
#include "transaction/rw_utils.h"


//==========================================================
// Typedefs & constants.
//

const msg_template rw_mt[] = {
		{ RW_FIELD_OP, M_FT_UINT32 },
		{ RW_FIELD_RESULT, M_FT_UINT32 },
		{ RW_FIELD_NAMESPACE, M_FT_BUF },
		{ RW_FIELD_NS_ID, M_FT_UINT32 },
		{ RW_FIELD_GENERATION, M_FT_UINT32 },
		{ RW_FIELD_DIGEST, M_FT_BUF },
		{ RW_FIELD_VINFOSET, M_FT_BUF },
		{ RW_FIELD_AS_MSG, M_FT_BUF },
		{ RW_FIELD_CLUSTER_KEY, M_FT_UINT64 },
		{ RW_FIELD_RECORD, M_FT_BUF },
		{ RW_FIELD_TID, M_FT_UINT32 },
		{ RW_FIELD_VOID_TIME, M_FT_UINT32 },
		{ RW_FIELD_INFO, M_FT_UINT32 },
		{ RW_FIELD_REC_PROPS, M_FT_BUF },
		{ RW_FIELD_MULTIOP, M_FT_BUF },
		{ RW_FIELD_LDT_VERSION, M_FT_UINT64 },
		{ RW_FIELD_LAST_UPDATE_TIME, M_FT_UINT64 }
};

COMPILER_ASSERT(sizeof(rw_mt) / sizeof(msg_template) == NUM_RW_FIELDS);

#define RW_MSG_SCRATCH_SIZE 128


//==========================================================
// Forward Declarations.
//

uint32_t rw_request_hash_fn(const void* value, uint32_t value_len);
transaction_status handle_hot_key(rw_request* rw0, as_transaction* tr);

void* run_retransmit(void* arg);
int retransmit_reduce_fn(const void* key, uint32_t keylen, void* data, void* udata);
void update_retransmit_stats(const rw_request* rw);

int rw_msg_cb(cf_node id, msg* m, void* udata);


//==========================================================
// Globals.
//

static cf_rchash* g_rw_request_hash = NULL;


//==========================================================
// Public API.
//

void
as_rw_init()
{
	cf_rchash_create(&g_rw_request_hash, rw_request_hash_fn,
			rw_request_hdestroy, sizeof(rw_request_hkey), 32 * 1024,
			CF_RCHASH_CR_MT_MANYLOCK);

	pthread_t thread;
	pthread_attr_t attrs;

	pthread_attr_init(&attrs);
	pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

	if (pthread_create(&thread, &attrs, run_retransmit, NULL) != 0) {
		cf_crash(AS_RW, "failed to create retransmit thread");
	}

	as_fabric_register_msg_fn(M_TYPE_RW, rw_mt, sizeof(rw_mt),
			RW_MSG_SCRATCH_SIZE, rw_msg_cb, NULL);
}


uint32_t
rw_request_hash_count()
{
	return cf_rchash_get_size(g_rw_request_hash);
}


transaction_status
rw_request_hash_insert(rw_request_hkey* hkey, rw_request* rw,
		as_transaction* tr)
{
	int insert_rv;

	while ((insert_rv = cf_rchash_put_unique(g_rw_request_hash, hkey,
			sizeof(*hkey), rw)) != CF_RCHASH_OK) {

		if (insert_rv != CF_RCHASH_ERR_FOUND) {
			tr->result_code = AS_PROTO_RESULT_FAIL_UNKNOWN; // malloc failure
			return TRANS_DONE_ERROR;
		}
		// else - rw_request with this digest already in hash - get it.

		rw_request* rw0;
		int get_rv = cf_rchash_get(g_rw_request_hash, hkey, sizeof(*hkey),
				(void**)&rw0);

		if (get_rv == CF_RCHASH_ERR_NOTFOUND) {
			// Try insertion again immediately.
			continue;
		}
		// else - got it - handle "hot key" scenario.
		cf_assert(get_rv == CF_RCHASH_OK, AS_RW, "cf_rchash_get error");

		pthread_mutex_lock(&rw0->lock);

		transaction_status status = handle_hot_key(rw0, tr);

		pthread_mutex_unlock(&rw0->lock);
		rw_request_release(rw0);

		return status; // rw_request was not inserted in the hash
	}

	return TRANS_IN_PROGRESS; // rw_request was inserted in the hash
}


void
rw_request_hash_delete(rw_request_hkey* hkey, rw_request* rw)
{
	cf_rchash_delete_object(g_rw_request_hash, hkey, sizeof(*hkey), rw);
}


rw_request*
rw_request_hash_get(rw_request_hkey* hkey)
{
	rw_request* rw = NULL;

	cf_rchash_get(g_rw_request_hash, hkey, sizeof(*hkey), (void**)&rw);

	return rw;
}


// For debugging only.
void
rw_request_hash_dump()
{
	cf_info(AS_RW, "rw_request_hash dump not yet implemented");
	// TODO - implement something, or deprecate.
}


//==========================================================
// Local helpers - hash insertion.
//

uint32_t
rw_request_hash_fn(const void* key, uint32_t key_size)
{
	rw_request_hkey* hkey = (rw_request_hkey*)key;

	return *(uint32_t*)&hkey->keyd.digest[DIGEST_SCRAMBLE_BYTE1];
}


transaction_status
handle_hot_key(rw_request* rw0, as_transaction* tr)
{
	if (rw0->is_set_up &&
			rw0->origin == FROM_PROXY && tr->origin == FROM_PROXY &&
			rw0->from.proxy_node == tr->from.proxy_node &&
			rw0->from_data.proxy_tid == tr->from_data.proxy_tid) {
		// If the new transaction is a retransmitted proxy request, don't
		// queue it or reply to origin, just drop it and feign success.

		return TRANS_DONE_SUCCESS;
	}
	else if (g_config.transaction_pending_limit != 0 &&
			rw_request_wait_q_depth(rw0) > g_config.transaction_pending_limit) {
		// If we're over the hot key pending limit, fail this transaction.
		cf_atomic64_incr(&tr->rsv.ns->n_fail_key_busy);
		tr->result_code = AS_PROTO_RESULT_FAIL_KEY_BUSY;

		return TRANS_DONE_ERROR;
	}
	else {
		// Queue this transaction on the original rw_request - it will be
		// retried when the original is complete.

		rw_wait_ele* e = cf_malloc(sizeof(rw_wait_ele));
		cf_assert(e, AS_RW, "alloc rw_wait_ele");

		as_transaction_copy_head(&e->tr, tr);
		tr->from.any = NULL;
		tr->msgp = NULL;

		e->next = rw0->wait_queue_head;
		rw0->wait_queue_head = e;

		return TRANS_WAITING;
	}
}


//==========================================================
// Local helpers - retransmit.
//

void*
run_retransmit(void* arg)
{
	while (true) {
		usleep(130 * 1000);

		now_times now;

		now.now_ns = cf_getns();
		now.now_ms = now.now_ns / 1000000;

		cf_rchash_reduce(g_rw_request_hash, retransmit_reduce_fn, &now);
	}

	return NULL;
}


int
retransmit_reduce_fn(const void* key, uint32_t keylen, void* data, void* udata)
{
	rw_request* rw = data;
	now_times* now = (now_times*)udata;

	if (! rw->is_set_up) {
		return 0;
	}

	if (now->now_ns > rw->end_time) {
		pthread_mutex_lock(&rw->lock);

		rw->timeout_cb(rw);

		pthread_mutex_unlock(&rw->lock);

		return CF_RCHASH_REDUCE_DELETE;
	}

	if (rw->xmit_ms < now->now_ms) {
		pthread_mutex_lock(&rw->lock);

		if (rw->from.any) {
			rw->xmit_ms = now->now_ms + rw->retry_interval_ms;
			rw->retry_interval_ms *= 2;

			send_rw_messages(rw);
			update_retransmit_stats(rw);
		}
		// else - lost race against dup-res or repl-write callback.

		pthread_mutex_unlock(&rw->lock);
	}

	return 0;
}


void
update_retransmit_stats(const rw_request* rw)
{
	// Note - rw->msgp can be null if it's a ship-op.
	if (! rw->msgp) {
		return;
	}

	as_namespace* ns = rw->rsv.ns;
	as_msg* m = &rw->msgp->msg;
	bool is_dup_res = rw->repl_write_cb == NULL;

	// Note - only one retransmit thread, so no need for atomic increments.

	switch (rw->origin) {
	case FROM_CLIENT: {
			bool is_write = (m->info2 & AS_MSG_INFO2_WRITE) != 0;
			bool is_delete = (m->info2 & AS_MSG_INFO2_DELETE) != 0;
			bool is_udf = (rw->msg_fields & AS_MSG_FIELD_BIT_UDF_FILENAME) != 0;

			if (is_dup_res) {
				if (is_write) {
					if (is_delete) {
						ns->n_retransmit_client_delete_dup_res++;
					}
					else if (is_udf) {
						ns->n_retransmit_client_udf_dup_res++;
					}
					else {
						ns->n_retransmit_client_write_dup_res++;
					}
				}
				else {
					ns->n_retransmit_client_read_dup_res++;
				}
			}
			else {
				cf_assert(is_write, AS_RW, "read doing replica write");

				if (is_delete) {
					ns->n_retransmit_client_delete_repl_write++;
				}
				else if (is_udf) {
					ns->n_retransmit_client_udf_repl_write++;
				}
				else {
					ns->n_retransmit_client_write_repl_write++;
				}
			}
		}
		break;
	case FROM_PROXY:
		// For now we don't report proxyee stats.
		break;
	case FROM_BATCH:
		// For now batch sub transactions are read-only.
		ns->n_retransmit_batch_sub_dup_res++;
		break;
	case FROM_IUDF:
		if (is_dup_res) {
			ns->n_retransmit_udf_sub_dup_res++;
		}
		else {
			ns->n_retransmit_udf_sub_repl_write++;
		}
		break;
	case FROM_NSUP:
		// nsup deletes don't duplicate resolve.
		ns->n_retransmit_nsup_repl_write++;
		break;
	default:
		cf_crash(AS_RW, "unexpected transaction origin %u", rw->origin);
		break;
	}
}


//==========================================================
// Local helpers - handle RW fabric messages.
//

int
rw_msg_cb(cf_node id, msg* m, void* udata)
{
	uint32_t op;

	if (msg_get_uint32(m, RW_FIELD_OP, &op) != 0) {
		cf_warning(AS_RW, "got rw msg without op field");
		as_fabric_msg_put(m);
		return 0;
	}

	switch (op) {
	//--------------------------------------------
	// Duplicate resolution:
	//
	case RW_OP_DUP:
		dup_res_handle_request(id, m);
		break;
	case RW_OP_DUP_ACK:
		dup_res_handle_ack(id, m);
		break;

	//--------------------------------------------
	// Replica writes:
	//
	case RW_OP_WRITE:
		repl_write_handle_op(id, m);
		break;
	case RW_OP_WRITE_ACK:
		repl_write_handle_ack(id, m);
		break;

	//--------------------------------------------
	// LDT-related:
	//
	case RW_OP_MULTI:
		{
			uint64_t start_ns = g_config.ldt_benchmarks ?  cf_getns() : 0;

			repl_write_handle_multiop(id, m);

			if (start_ns != 0) {
				histogram_insert_data_point(g_stats.ldt_multiop_prole_hist,
						start_ns);
			}
		}
		break;
	case RW_OP_MULTI_ACK:
		repl_write_handle_multiop_ack(id, m);
		break;

	default:
		cf_warning(AS_RW, "got rw msg with unrecognized op %u", op);
		as_fabric_msg_put(m);
		break;
	}

	return 0;
}
