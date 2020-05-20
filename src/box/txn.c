/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "txn.h"
#include "engine.h"
#include "tuple.h"
#include "journal.h"
#include <fiber.h>
#include "xrow.h"
#include "errinj.h"
#include "iproto_constants.h"
#include "small/mempool.h"

static uint32_t
txm_story_key_hash(const struct tuple *a)
{
	uintptr_t u = (uintptr_t)a;
	if (sizeof(uintptr_t) <= sizeof(uint32_t))
		return u;
	else
		return u ^ (u >> 32);
}

#define mh_name _history
#define mh_key_t struct tuple *
#define mh_node_t struct txm_story *
#define mh_arg_t int
#define mh_hash(a, arg) (txm_story_key_hash((*(a))->tuple))
#define mh_hash_key(a, arg) (txm_story_key_hash(a))
#define mh_cmp(a, b, arg) ((*(a))->tuple != (*(b))->tuple)
#define mh_cmp_key(a, b, arg) ((a) != (*(b))->tuple)
#define MH_SOURCE
#include "salad/mhash.h"

struct tx_manager
{
	/** Last prepare-sequence-number that was assigned to preped TX. */
	int64_t last_psn;
	/** Mempools for tx_story objects with difference index count. */
	struct mempool txm_story_pool[BOX_INDEX_MAX];
	/** Hash table tuple -> txm_story of that tuple. */
	struct mh_history_t *history;
	/** List of all txm_story objects. */
	struct rlist all_stories;
	/** Iterator that sequentially traverses all txm_story objects. */
	struct rlist *traverse_all_stories;
};

/** The one and only instance of tx_manager. */
static struct tx_manager txm;

struct tx_conflict_tracker {
	struct txn *wreaker;
	struct txn *victim;
	struct rlist in_conflict_list;
	struct rlist in_conflicted_by_list;
};

double too_long_threshold;

/* Txn cache. */
static struct stailq txn_cache = {NULL, &txn_cache.first};

static int
txn_on_stop(struct trigger *trigger, void *event);

static int
txn_on_yield(struct trigger *trigger, void *event);

static void
txn_run_rollback_triggers(struct txn *txn, struct rlist *triggers);

static int
txn_add_redo(struct txn *txn, struct txn_stmt *stmt, struct request *request)
{
	/* Create a redo log row. */
	int size;
	struct xrow_header *row;
	row = region_alloc_object(&txn->region, struct xrow_header, &size);
	if (row == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "row");
		return -1;
	}
	if (request->header != NULL) {
		*row = *request->header;
	} else {
		/* Initialize members explicitly to save time on memset() */
		row->type = request->type;
		row->replica_id = 0;
		row->lsn = 0;
		row->sync = 0;
		row->tm = 0;
	}
	/*
	 * Group ID should be set both for requests not having a
	 * header, and for the ones who have it. This is because
	 * even if a request has a header, the group id could be
	 * omitted in it, and is default - 0. Even if the space's
	 * real group id is different.
	 */
	struct space *space = stmt->space;
	row->group_id = space != NULL ? space_group_id(space) : 0;
	row->bodycnt = xrow_encode_dml(request, &txn->region, row->body);
	if (row->bodycnt < 0)
		return -1;
	stmt->row = row;
	return 0;
}

/** Initialize a new stmt object within txn. */
static struct txn_stmt *
txn_stmt_new(struct region *region)
{
	int size;
	struct txn_stmt *stmt;
	stmt = region_alloc_object(region, struct txn_stmt, &size);
	if (stmt == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "stmt");
		return NULL;
	}

	/* Initialize members explicitly to save time on memset() */
	stmt->txn = in_txn();
	stmt->space = NULL;
	stmt->old_tuple = NULL;
	stmt->new_tuple = NULL;
	stmt->add_story = NULL;
	stmt->del_story = NULL;
	stmt->next_in_del_list = NULL;
	stmt->engine_savepoint = NULL;
	stmt->row = NULL;
	stmt->has_triggers = false;
	stmt->preserve_old_tuple = false;
	return stmt;
}

static inline void
txn_stmt_destroy(struct txn_stmt *stmt)
{
	if (stmt->old_tuple != NULL)
		tuple_unref(stmt->old_tuple);
	if (stmt->new_tuple != NULL)
		tuple_unref(stmt->new_tuple);
}

/*
 * Undo changes done by a statement and run the corresponding
 * rollback triggers.
 *
 * Note, a trigger set by a particular statement must be run right
 * after the statement is rolled back, because rollback triggers
 * installed by DDL statements restore the schema cache, which is
 * necessary to roll back previous statements. For example, to roll
 * back a DML statement applied to a space whose index is dropped
 * later in the same transaction, we must restore the dropped index
 * first.
 */
static void
txn_rollback_one_stmt(struct txn *txn, struct txn_stmt *stmt)
{
	if (txn->engine != NULL && stmt->space != NULL)
		engine_rollback_statement(txn->engine, txn, stmt);
	if (stmt->has_triggers)
		txn_run_rollback_triggers(txn, &stmt->on_rollback);
}

static void
txn_rollback_to_svp(struct txn *txn, struct stailq_entry *svp)
{
	struct txn_stmt *stmt;
	struct stailq rollback;
	stailq_cut_tail(&txn->stmts, svp, &rollback);
	stailq_reverse(&rollback);
	stailq_foreach_entry(stmt, &rollback, next) {
		txn_rollback_one_stmt(txn, stmt);
		if (stmt->row != NULL && stmt->row->replica_id == 0) {
			assert(txn->n_new_rows > 0);
			txn->n_new_rows--;
			if (stmt->row->group_id == GROUP_LOCAL)
				txn->n_local_rows--;
		}
		if (stmt->row != NULL && stmt->row->replica_id != 0) {
			assert(txn->n_applier_rows > 0);
			txn->n_applier_rows--;
		}
		txn_stmt_destroy(stmt);
		stmt->space = NULL;
		stmt->row = NULL;
	}
}

/*
 * Return a txn from cache or create a new one if cache is empty.
 */
inline static struct txn *
txn_new(void)
{
	if (!stailq_empty(&txn_cache))
		return stailq_shift_entry(&txn_cache, struct txn, in_txn_cache);

	/* Create a region. */
	struct region region;
	region_create(&region, &cord()->slabc);

	/* Place txn structure on the region. */
	int size;
	struct txn *txn = region_alloc_object(&region, struct txn, &size);
	if (txn == NULL) {
		diag_set(OutOfMemory, size, "region_alloc_object", "txn");
		return NULL;
	}
	assert(region_used(&region) == sizeof(*txn));
	txn->region = region;
	rlist_create(&txn->conflict_list);
	rlist_create(&txn->conflicted_by_list);
	return txn;
}

/*
 * Free txn memory and return it to a cache.
 */
inline static void
txn_free(struct txn *txn)
{
	struct tx_conflict_tracker *entry, *next;
	rlist_foreach_entry_safe(entry, &txn->conflict_list,
				 in_conflict_list, next) {
		rlist_del(&entry->in_conflict_list);
		rlist_del(&entry->in_conflicted_by_list);
	}
	rlist_foreach_entry_safe(entry, &txn->conflicted_by_list,
				 in_conflicted_by_list, next) {
		rlist_del(&entry->in_conflict_list);
		rlist_del(&entry->in_conflicted_by_list);
	}
	assert(rlist_empty(&txn->conflict_list));
	assert(rlist_empty(&txn->conflicted_by_list));

	struct txn_stmt *stmt;
	stailq_foreach_entry(stmt, &txn->stmts, next)
		txn_stmt_destroy(stmt);

	/* Truncate region up to struct txn size. */
	region_truncate(&txn->region, sizeof(struct txn));
	stailq_add(&txn_cache, &txn->in_txn_cache);
}

struct txn *
txn_begin(void)
{
	static int64_t tsn = 0;
	assert(! in_txn());
	struct txn *txn = txn_new();
	if (txn == NULL)
		return NULL;
	assert(rlist_empty(&txn->conflict_list));
	assert(rlist_empty(&txn->conflicted_by_list));

	/* Initialize members explicitly to save time on memset() */
	stailq_create(&txn->stmts);
	txn->n_new_rows = 0;
	txn->n_local_rows = 0;
	txn->n_applier_rows = 0;
	txn->flags = 0;
	txn->in_sub_stmt = 0;
	txn->id = ++tsn;
	txn->status = TXN_INPROGRESS;
	txn->signature = -1;
	txn->engine = NULL;
	txn->engine_tx = NULL;
	txn->fk_deferred_count = 0;
	rlist_create(&txn->savepoints);
	txn->fiber = NULL;
	fiber_set_txn(fiber(), txn);
	/* fiber_on_yield is initialized by engine on demand */
	trigger_create(&txn->fiber_on_stop, txn_on_stop, NULL, NULL);
	trigger_add(&fiber()->on_stop, &txn->fiber_on_stop);
	/*
	 * By default all transactions may yield.
	 * It's a responsibility of an engine to disable yields
	 * if they are not supported.
	 */
	txn_set_flag(txn, TXN_CAN_YIELD);
	return txn;
}

int
txn_begin_in_engine(struct engine *engine, struct txn *txn)
{
	if (engine->flags & ENGINE_BYPASS_TX)
		return 0;
	if (txn->engine == NULL) {
		txn->engine = engine;
		return engine_begin(engine, txn);
	} else if (txn->engine != engine) {
		/**
		 * Only one engine can be used in
		 * a multi-statement transaction currently.
		 */
		diag_set(ClientError, ER_CROSS_ENGINE_TRANSACTION);
		return -1;
	}
	return 0;
}

int
txn_begin_stmt(struct txn *txn, struct space *space)
{
	assert(txn == in_txn());
	assert(txn != NULL);
	if (txn->in_sub_stmt > TXN_SUB_STMT_MAX) {
		diag_set(ClientError, ER_SUB_STMT_MAX);
		return -1;
	}

	/*
	 * A conflict have happened; there is no reason to continue the TX.
	 */
	if (txn->status == TXN_CONFLICTED) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}

	struct txn_stmt *stmt = txn_stmt_new(&txn->region);
	if (stmt == NULL)
		return -1;

	/* Set the savepoint for statement rollback. */
	txn->sub_stmt_begin[txn->in_sub_stmt] = stailq_last(&txn->stmts);
	txn->in_sub_stmt++;
	stailq_add_tail_entry(&txn->stmts, stmt, next);

	if (space == NULL)
		return 0;

	struct engine *engine = space->engine;
	if (txn_begin_in_engine(engine, txn) != 0)
		goto fail;

	stmt->space = space;
	if (engine_begin_statement(engine, txn) != 0)
		goto fail;

	return 0;
fail:
	txn_rollback_stmt(txn);
	return -1;
}

bool
txn_is_distributed(struct txn *txn)
{
	assert(txn == in_txn());
	/**
	 * Transaction has both new and applier rows, and some of
	 * the new rows need to be replicated back to the
	 * server of transaction origin.
	 */
	return (txn->n_new_rows > 0 && txn->n_applier_rows > 0 &&
		txn->n_new_rows != txn->n_local_rows);
}

/**
 * End a statement.
 */
int
txn_commit_stmt(struct txn *txn, struct request *request)
{
	assert(txn->in_sub_stmt > 0);
	/*
	 * Run on_replace triggers. For now, disallow mutation
	 * of tuples in the trigger.
	 */
	struct txn_stmt *stmt = txn_current_stmt(txn);

	/* Create WAL record for the write requests in non-temporary spaces.
	 * stmt->space can be NULL for IRPOTO_NOP.
	 */
	if (stmt->space == NULL || !space_is_temporary(stmt->space)) {
		if (txn_add_redo(txn, stmt, request) != 0)
			goto fail;
		assert(stmt->row != NULL);
		if (stmt->row->replica_id == 0) {
			++txn->n_new_rows;
			if (stmt->row->group_id == GROUP_LOCAL)
				++txn->n_local_rows;

		} else {
			++txn->n_applier_rows;
		}
	}
	/*
	 * If there are triggers, and they are not disabled, and
	 * the statement found any rows, run triggers.
	 * XXX:
	 * - vinyl doesn't set old/new tuple, so triggers don't
	 *   work for it
	 * - perhaps we should run triggers even for deletes which
	 *   doesn't find any rows
	 */
	if (stmt->space != NULL && !rlist_empty(&stmt->space->on_replace) &&
	    stmt->space->run_triggers && (stmt->old_tuple || stmt->new_tuple)) {
		/* Triggers see old_tuple and that tuple must remain the same */
		stmt->preserve_old_tuple = true;
		int rc = 0;
		if(!space_is_temporary(stmt->space)) {
			rc = trigger_run(&stmt->space->on_replace, txn);
		} else {
			/*
			 * There is no row attached to txn_stmt for
			 * temporary spaces, since DML operations on them
			 * are not written to WAL. Fake a row to pass operation
			 * type to lua on_replace triggers.
			 */
			assert(stmt->row == NULL);
			struct xrow_header temp_header;
			temp_header.type = request->type;
			stmt->row = &temp_header;
			rc = trigger_run(&stmt->space->on_replace, txn);
			stmt->row = NULL;
		}
		if (rc != 0)
			goto fail;
	}
	--txn->in_sub_stmt;
	return 0;
fail:
	txn_rollback_stmt(txn);
	return -1;
}

/*
 * A helper function to process on_commit triggers.
 */
static void
txn_run_commit_triggers(struct txn *txn, struct rlist *triggers)
{
	/*
	 * Commit triggers must be run in the same order they
	 * were added so that a trigger sees the changes done
	 * by previous triggers (this is vital for DDL).
	 */
	if (trigger_run_reverse(triggers, txn) != 0) {
		/*
		 * As transaction couldn't handle a trigger error so
		 * there is no option except panic.
		 */
		diag_log();
		unreachable();
		panic("commit trigger failed");
	}
}

/*
 * A helper function to process on_rollback triggers.
 */
static void
txn_run_rollback_triggers(struct txn *txn, struct rlist *triggers)
{
	if (trigger_run(triggers, txn) != 0) {
		/*
		 * As transaction couldn't handle a trigger error so
		 * there is no option except panic.
		 */
		diag_log();
		unreachable();
		panic("rollback trigger failed");
	}
}

/**
 * Complete transaction processing.
 */
static void
txn_complete(struct txn *txn)
{
	/*
	 * Note, engine can be NULL if transaction contains
	 * IPROTO_NOP statements only.
	 */
	if (txn->signature < 0) {
		txn->status = TXN_ABORTED;
		/* Undo the transaction. */
		struct txn_stmt *stmt;
		stailq_reverse(&txn->stmts);
		stailq_foreach_entry(stmt, &txn->stmts, next)
			txn_rollback_one_stmt(txn, stmt);
		if (txn->engine)
			engine_rollback(txn->engine, txn);
		if (txn_has_flag(txn, TXN_HAS_TRIGGERS))
			txn_run_rollback_triggers(txn, &txn->on_rollback);
	} else {
		txn->status = TXN_COMMITTED;
		/* Commit the transaction. */
		if (txn->engine != NULL)
			engine_commit(txn->engine, txn);
		if (txn_has_flag(txn, TXN_HAS_TRIGGERS))
			txn_run_commit_triggers(txn, &txn->on_commit);

		double stop_tm = ev_monotonic_now(loop());
		if (stop_tm - txn->start_tm > too_long_threshold) {
			int n_rows = txn->n_new_rows + txn->n_applier_rows;
			say_warn_ratelimited("too long WAL write: %d rows at "
					     "LSN %lld: %.3f sec", n_rows,
					     txn->signature - n_rows + 1,
					     stop_tm - txn->start_tm);
		}
	}
	/*
	 * If there is no fiber waiting for the transaction then
	 * the transaction could be safely freed. In the opposite case
	 * the fiber is in duty to free this transaction.
	 */
	if (txn->fiber == NULL)
		txn_free(txn);
	else {
		txn_set_flag(txn, TXN_IS_DONE);
		if (txn->fiber != fiber())
			/* Wake a waiting fiber up. */
			fiber_wakeup(txn->fiber);
	}
}

void
txn_complete_async(struct journal_entry *entry)
{
	struct txn *txn = entry->complete_data;
	txn->signature = entry->res;
	/*
	 * Some commit/rollback triggers require for in_txn fiber
	 * variable to be set so restore it for the time triggers
	 * are in progress.
	 */
	assert(in_txn() == NULL);
	fiber_set_txn(fiber(), txn);
	txn_complete(txn);
	fiber_set_txn(fiber(), NULL);
}

static struct journal_entry *
txn_journal_entry_new(struct txn *txn)
{
	struct journal_entry *req;
	struct txn_stmt *stmt;

	assert(txn->n_new_rows + txn->n_applier_rows > 0);

	/* Save space for an additional NOP row just in case. */
	req = journal_entry_new(txn->n_new_rows + txn->n_applier_rows + 1,
				&txn->region, txn);
	if (req == NULL)
		return NULL;

	struct xrow_header **remote_row = req->rows;
	struct xrow_header **local_row = req->rows + txn->n_applier_rows;

	stailq_foreach_entry(stmt, &txn->stmts, next) {
		if (stmt->has_triggers) {
			txn_init_triggers(txn);
			rlist_splice(&txn->on_commit, &stmt->on_commit);
		}

		/* A read (e.g. select) request */
		if (stmt->row == NULL)
			continue;

		if (stmt->row->replica_id == 0)
			*local_row++ = stmt->row;
		else
			*remote_row++ = stmt->row;

		req->approx_len += xrow_approx_len(stmt->row);
	}

	assert(remote_row == req->rows + txn->n_applier_rows);
	assert(local_row == remote_row + txn->n_new_rows);

	/*
	 * Append a dummy NOP statement to preserve replication tx
	 * boundaries when the last tx row is a local one, and the
	 * transaction has at least one global row.
	 */
	if (txn->n_local_rows > 0 &&
	    (txn->n_local_rows != txn->n_new_rows || txn->n_applier_rows > 0) &&
	    (*(local_row - 1))->group_id == GROUP_LOCAL) {
		size_t size;
		*local_row = region_alloc_object(&txn->region,
						 typeof(**local_row), &size);
		if (*local_row == NULL) {
			diag_set(OutOfMemory, size, "region_alloc_object",
				 "row");
			return NULL;
		}
		memset(*local_row, 0, sizeof(**local_row));
		(*local_row)->type = IPROTO_NOP;
		(*local_row)->group_id = GROUP_DEFAULT;
	} else {
		--req->n_rows;
	}

	return req;
}

/*
 * Prepare a transaction using engines.
 */
static int
txn_prepare(struct txn *txn)
{
	txn->psn = ++txm.last_psn;

	if (txn_has_flag(txn, TXN_IS_ABORTED_BY_YIELD)) {
		assert(!txn_has_flag(txn, TXN_CAN_YIELD));
		diag_set(ClientError, ER_TRANSACTION_YIELD);
		diag_log();
		return -1;
	}
	/*
	 * If transaction has been started in SQL, deferred
	 * foreign key constraints must not be violated.
	 * If not so, just rollback transaction.
	 */
	if (txn->fk_deferred_count != 0) {
		diag_set(ClientError, ER_FOREIGN_KEY_CONSTRAINT);
		return -1;
	}

	/*
	 * Somebody else has written some value that we have read.
	 * The transaction is not possible.
	 */
	if (txn->status == TXN_CONFLICTED) {
		diag_set(ClientError, ER_TRANSACTION_CONFLICT);
		return -1;
	}

	/*
	 * Perform transaction conflict resolution. Engine == NULL when
	 * we have a bunch of IPROTO_NOP statements.
	 */
	if (txn->engine != NULL) {
		if (engine_prepare(txn->engine, txn) != 0)
			return -1;
	}

	struct tx_conflict_tracker *entry, *next;
	rlist_foreach_entry_safe(entry, &txn->conflict_list,
				 in_conflict_list, next) {
		if (entry->victim->status == TXN_INPROGRESS)
			entry->victim->status = TXN_CONFLICTED;
		rlist_del(&entry->in_conflict_list);
		rlist_del(&entry->in_conflicted_by_list);
	}


	trigger_clear(&txn->fiber_on_stop);
	if (!txn_has_flag(txn, TXN_CAN_YIELD))
		trigger_clear(&txn->fiber_on_yield);

	txn->start_tm = ev_monotonic_now(loop());
	txn->status = TXN_PREPARED;
	return 0;
}

/**
 * Complete transaction early if it is barely nop.
 */
static bool
txn_commit_nop(struct txn *txn)
{
	if (txn->n_new_rows + txn->n_applier_rows == 0) {
		txn->signature = 0;
		txn_complete(txn);
		fiber_set_txn(fiber(), NULL);
		return true;
	}

	return false;
}

int
txn_commit_async(struct txn *txn)
{
	struct journal_entry *req;

	ERROR_INJECT(ERRINJ_TXN_COMMIT_ASYNC, {
		diag_set(ClientError, ER_INJECTION,
			 "txn commit async injection");
		/*
		 * Log it for the testing sake: we grep
		 * output to mark this event.
		 */
		diag_log();
		txn_rollback(txn);
		return -1;
	});

	if (txn_prepare(txn) != 0) {
		txn_rollback(txn);
		return -1;
	}

	if (txn_commit_nop(txn))
		return 0;

	req = txn_journal_entry_new(txn);
	if (req == NULL) {
		txn_rollback(txn);
		return -1;
	}

	fiber_set_txn(fiber(), NULL);
	if (journal_write_async(req) != 0) {
		fiber_set_txn(fiber(), txn);
		txn_rollback(txn);

		diag_set(ClientError, ER_WAL_IO);
		diag_log();
		return -1;
	}

	return 0;
}

int
txn_commit(struct txn *txn)
{
	struct journal_entry *req;

	txn->fiber = fiber();

	if (txn_prepare(txn) != 0) {
		txn_rollback(txn);
		txn_free(txn);
		return -1;
	}

	if (txn_commit_nop(txn)) {
		txn_free(txn);
		return 0;
	}

	req = txn_journal_entry_new(txn);
	if (req == NULL) {
		txn_rollback(txn);
		txn_free(txn);
		return -1;
	}

	fiber_set_txn(fiber(), NULL);
	if (journal_write(req) != 0) {
		fiber_set_txn(fiber(), txn);
		txn_rollback(txn);
		txn_free(txn);

		diag_set(ClientError, ER_WAL_IO);
		diag_log();
		return -1;
	}

	if (!txn_has_flag(txn, TXN_IS_DONE)) {
		txn->signature = req->res;
		txn_complete(txn);
	}

	int res = txn->signature >= 0 ? 0 : -1;
	if (res != 0) {
		diag_set(ClientError, ER_WAL_IO);
		diag_log();
	}

	/* Synchronous transactions are freed by the calling fiber. */
	txn_free(txn);
	return res;
}

void
txn_rollback_stmt(struct txn *txn)
{
	if (txn == NULL || txn->in_sub_stmt == 0)
		return;
	txn->in_sub_stmt--;
	txn_rollback_to_svp(txn, txn->sub_stmt_begin[txn->in_sub_stmt]);
}

void
txn_rollback(struct txn *txn)
{
	assert(txn == in_txn());
	txn->status = TXN_ABORTED;
	trigger_clear(&txn->fiber_on_stop);
	if (!txn_has_flag(txn, TXN_CAN_YIELD))
		trigger_clear(&txn->fiber_on_yield);
	txn->signature = -1;
	txn_complete(txn);
	fiber_set_txn(fiber(), NULL);
}

int
txn_check_singlestatement(struct txn *txn, const char *where)
{
	if (!txn_is_first_statement(txn)) {
		diag_set(ClientError, ER_MULTISTATEMENT_TRANSACTION, where);
		return -1;
	}
	return 0;
}

void
txn_can_yield(struct txn *txn, bool set)
{
	assert(txn == in_txn());
	if (set) {
		assert(!txn_has_flag(txn, TXN_CAN_YIELD));
		txn_set_flag(txn, TXN_CAN_YIELD);
		trigger_clear(&txn->fiber_on_yield);
	} else {
		assert(txn_has_flag(txn, TXN_CAN_YIELD));
		txn_clear_flag(txn, TXN_CAN_YIELD);
		trigger_create(&txn->fiber_on_yield, txn_on_yield, NULL, NULL);
		trigger_add(&fiber()->on_yield, &txn->fiber_on_yield);
	}
}

int64_t
box_txn_id(void)
{
	struct txn *txn = in_txn();
	if (txn != NULL)
		return txn->id;
	else
		return -1;
}

bool
box_txn(void)
{
	return in_txn() != NULL;
}

int
box_txn_begin(void)
{
	if (in_txn()) {
		diag_set(ClientError, ER_ACTIVE_TRANSACTION);
		return -1;
	}
	if (txn_begin() == NULL)
		return -1;
	return 0;
}

int
box_txn_commit(void)
{
	struct txn *txn = in_txn();
	/**
	 * COMMIT is like BEGIN or ROLLBACK
	 * a "transaction-initiating statement".
	 * Do nothing if transaction is not started,
	 * it's the same as BEGIN + COMMIT.
	*/
	if (! txn)
		return 0;
	if (txn->in_sub_stmt) {
		diag_set(ClientError, ER_COMMIT_IN_SUB_STMT);
		return -1;
	}
	int rc = txn_commit(txn);
	fiber_gc();
	return rc;
}

int
box_txn_rollback(void)
{
	struct txn *txn = in_txn();
	if (txn == NULL)
		return 0;
	if (txn && txn->in_sub_stmt) {
		diag_set(ClientError, ER_ROLLBACK_IN_SUB_STMT);
		return -1;
	}
	txn_rollback(txn); /* doesn't throw */
	fiber_gc();
	return 0;
}

void *
box_txn_alloc(size_t size)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		/* There are no transaction yet - return an error. */
		diag_set(ClientError, ER_NO_TRANSACTION);
		return NULL;
	}
	union natural_align {
		void *p;
		double lf;
		long l;
	};
	return region_aligned_alloc(&txn->region, size,
	                            alignof(union natural_align));
}

struct txn_savepoint *
txn_savepoint_new(struct txn *txn, const char *name)
{
	assert(txn == in_txn());
	int name_len = name != NULL ? strlen(name) : 0;
	struct txn_savepoint *svp;
	static_assert(sizeof(svp->name) == 1,
		      "name member already has 1 byte for 0 termination");
	size_t size = sizeof(*svp) + name_len;
	svp = (struct txn_savepoint *)region_aligned_alloc(&txn->region, size,
							   alignof(*svp));
	if (svp == NULL) {
		diag_set(OutOfMemory, size, "region_aligned_alloc", "svp");
		return NULL;
	}
	svp->stmt = stailq_last(&txn->stmts);
	svp->in_sub_stmt = txn->in_sub_stmt;
	svp->fk_deferred_count = txn->fk_deferred_count;
	if (name != NULL) {
		/*
		 * If savepoint with given name already exists,
		 * erase it from the list. This has to be done
		 * in accordance with ANSI SQL compliance.
		 */
		struct txn_savepoint *old_svp =
			txn_savepoint_by_name(txn, name);
		if (old_svp != NULL)
			rlist_del(&old_svp->link);
		memcpy(svp->name, name, name_len + 1);
	} else {
		svp->name[0] = 0;
	}
	rlist_add_entry(&txn->savepoints, svp, link);
	return svp;
}

struct txn_savepoint *
txn_savepoint_by_name(struct txn *txn, const char *name)
{
	assert(txn == in_txn());
	struct txn_savepoint *sv;
	rlist_foreach_entry(sv, &txn->savepoints, link) {
		if (strcmp(sv->name, name) == 0)
			return sv;
	}
	return NULL;
}

box_txn_savepoint_t *
box_txn_savepoint(void)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return NULL;
	}
	return txn_savepoint_new(txn, NULL);
}

int
box_txn_rollback_to_savepoint(box_txn_savepoint_t *svp)
{
	struct txn *txn = in_txn();
	if (txn == NULL) {
		diag_set(ClientError, ER_NO_TRANSACTION);
		return -1;
	}
	struct txn_stmt *stmt = svp->stmt == NULL ? NULL :
			stailq_entry(svp->stmt, struct txn_stmt, next);
	if (stmt != NULL && stmt->space == NULL && stmt->row == NULL) {
		/*
		 * The statement at which this savepoint was
		 * created has been rolled back.
		 */
		diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
		return -1;
	}
	if (svp->in_sub_stmt != txn->in_sub_stmt) {
		diag_set(ClientError, ER_NO_SUCH_SAVEPOINT);
		return -1;
	}
	txn_rollback_to_svp(txn, svp->stmt);
	/* Discard from list all newer savepoints. */
	RLIST_HEAD(discard);
	rlist_cut_before(&discard, &txn->savepoints, &svp->link);
	txn->fk_deferred_count = svp->fk_deferred_count;
	return 0;
}

void
txn_savepoint_release(struct txn_savepoint *svp)
{
	struct txn *txn = in_txn();
	assert(txn != NULL);
	/* Make sure that savepoint hasn't been released yet. */
	struct txn_stmt *stmt = svp->stmt == NULL ? NULL :
				stailq_entry(svp->stmt, struct txn_stmt, next);
	assert(stmt == NULL || (stmt->space != NULL && stmt->row != NULL));
	(void) stmt;
	/*
	 * Discard current savepoint alongside with all
	 * created after it savepoints.
	 */
	RLIST_HEAD(discard);
	rlist_cut_before(&discard, &txn->savepoints, rlist_next(&svp->link));
}

static int
txn_on_stop(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	txn_rollback(in_txn());                 /* doesn't yield or fail */
	fiber_gc();
	return 0;
}

/**
 * Memtx yield-in-transaction trigger callback.
 *
 * In case of a yield inside memtx multi-statement transaction
 * we must, first of all, roll back the effects of the transaction
 * so that concurrent transactions won't see dirty, uncommitted
 * data.
 *
 * Second, we must abort the transaction, since it has been rolled
 * back implicitly. The transaction can not be rolled back
 * completely from within a yield trigger, since a yield trigger
 * can't fail. Instead, we mark the transaction as aborted and
 * raise an error on attempt to commit it.
 *
 * So much hassle to be user-friendly until we have a true
 * interactive transaction support in memtx.
 */
static int
txn_on_yield(struct trigger *trigger, void *event)
{
	(void) trigger;
	(void) event;
	struct txn *txn = in_txn();
	assert(txn != NULL);
	assert(!txn_has_flag(txn, TXN_CAN_YIELD));
	txn_rollback_to_svp(txn, NULL);
	txn_set_flag(txn, TXN_IS_ABORTED_BY_YIELD);
	return 0;
}

void
tx_manager_init()
{
	for (size_t i = 0; i < BOX_INDEX_MAX; i++) {
		size_t item_size = sizeof(struct txm_story) +
			i * sizeof(struct txm_story_link);
		mempool_create(&txm.txm_story_pool[i],
			       cord_slab_cache(), item_size);
	}
	txm.history = mh_history_new();
	rlist_create(&txm.all_stories);
	txm.traverse_all_stories = &txm.all_stories;
}

void
tx_manager_free()
{
	mh_history_delete(txm.history);
	for (size_t i = 0; i < BOX_INDEX_MAX; i++)
		mempool_destroy(&txm.txm_story_pool[i]);
}

int
txm_cause_conflict(struct txn *wreaker, struct txn *victim)
{
	struct tx_conflict_tracker *tracker = NULL;
	struct rlist *r1 = wreaker->conflict_list.next;
	struct rlist *r2 = wreaker->conflicted_by_list.next;
	while (r1 != &wreaker->conflict_list &&
	       r2 != &wreaker->conflicted_by_list) {
		tracker = rlist_entry(r1, struct tx_conflict_tracker,
				      in_conflict_list);
		if (tracker->wreaker == wreaker && tracker->victim == victim)
			break;
		tracker = rlist_entry(r2, struct tx_conflict_tracker,
				      in_conflicted_by_list);
		if (tracker->wreaker == wreaker && tracker->victim == victim)
			break;
		tracker = NULL;
		r1 = r1->next;
		r2 = r2->next;
	}
	if (tracker != NULL) {
		/* Move to the beginning of a list
		 * for a case of subsequent lookups */
		rlist_del(&tracker->in_conflict_list);
		rlist_del(&tracker->in_conflicted_by_list);
	} else {
		size_t size;
		tracker = region_alloc_object(&victim->region,
					      struct tx_conflict_tracker,
					      &size);
		if (tracker == NULL) {
			diag_set(OutOfMemory, size, "tx region",
				 "conflict_tracker");
			return -1;
		}
	}
	tracker->wreaker = wreaker;
	tracker->victim = victim;
	rlist_add(&wreaker->conflict_list, &tracker->in_conflict_list);
	rlist_add(&wreaker->conflicted_by_list, &tracker->in_conflicted_by_list);
	return 0;
}

static struct txm_story *
txm_story_new(struct space *space, struct tuple *tuple)
{
	assert(!tuple->is_dirty);
	uint32_t index_count = space->index_count;
	assert(index_count < BOX_INDEX_MAX);
	struct mempool *pool = &txm.txm_story_pool[index_count];
	struct txm_story *story = (struct txm_story *)mempool_alloc(pool);
	if (story == NULL) {
		size_t item_size = sizeof(struct txm_story) +
			index_count * sizeof(struct txm_story_link);
		diag_set(OutOfMemory, item_size,
			 "tx_manager", "tx story");
		return story;
	}
	story->tuple = tuple;

	const struct txm_story **put_story = (const struct txm_story **)&story;
	struct txm_story **empty = NULL;
	mh_int_t pos = mh_history_put(txm.history, put_story, &empty, 0);
	if (pos == mh_end(txm.history)) {
		mempool_free(pool, story);
		diag_set(OutOfMemory, pos + 1,
			 "tx_manager", "tx history hash table");
		return NULL;
	}
	tuple->is_dirty = true;
	tuple_ref(tuple);

	story->index_count = index_count;
	story->add_stmt = NULL;
	story->add_psn = 0;
	story->del_stmt = NULL;
	story->del_psn = 0;
	rlist_create(&story->reader_list);
	rlist_add(&txm.all_stories, &story->in_all_stories);
	memset(story->link, 0, sizeof(story->link[0]) * index_count);
	return story;
}

static void
txm_story_delete(struct txm_story *story);

static struct txm_story *
txm_story_new_add_stmt(struct tuple *tuple, struct txn_stmt *stmt)
{
	struct txm_story *res = txm_story_new(stmt->space, tuple);
	if (res == NULL)
		return NULL;
	res->add_stmt = stmt;
	assert(stmt->add_story == NULL);
	stmt->add_story = res;
	return res;
}

static struct txm_story *
txm_story_new_del_stmt(struct tuple *tuple, struct txn_stmt *stmt)
{
	struct txm_story *res = txm_story_new(stmt->space, tuple);
	if (res == NULL)
		return NULL;
	res->del_stmt = stmt;
	assert(stmt->del_story == NULL);
	stmt->del_story = res;
	return res;
}

static void
txm_story_delete_add_stmt(struct txm_story *story)
{
	story->add_stmt->add_story = NULL;
	story->add_stmt = NULL;
	txm_story_delete(story);
}

static void
txm_story_delete_del_stmt(struct txm_story *story)
{
	story->del_stmt->del_story = NULL;
	story->del_stmt = NULL;
	txm_story_delete(story);
}


static struct txm_story *
txm_story_get(struct tuple *tuple)
{
	assert(tuple->is_dirty);

	mh_int_t pos = mh_history_find(txm.history, tuple, 0);
	assert(pos != mh_end(txm.history));
	return *mh_history_node(txm.history, pos);
}

static struct tuple *
txm_story_older_tuple(struct txm_story_link *link)
{
	return link->older.is_story ? link->older.story->tuple
				    : link->older.tuple;
}

static void
txm_story_link_story(struct txm_story *story, struct txm_story *older_story,
		     uint32_t index)
{
	assert(older_story != NULL);
	struct txm_story_link *link = &story->link[index];
	/* Must be unlinked. */
	assert(!link->older.is_story);
	assert(link->older.tuple == NULL);
	link->older.is_story = true;
	link->older.story = older_story;
	older_story->link[index].newer_story = story;
}

static void
txm_story_link_tuple(struct txm_story *story, struct tuple *older_tuple,
                     uint32_t index)
{
	struct txm_story_link *link = &story->link[index];
	/* Must be unlinked. */
	assert(!link->older.is_story);
	assert(link->older.tuple == NULL);
	if (older_tuple == NULL)
		return;
	if (older_tuple->is_dirty) {
		txm_story_link_story(story, txm_story_get(older_tuple), index);
		return;
	}
	tusp_setNT(&link->older.tuple, older_tuple);
}

static void
txm_story_unlink(struct txm_story *story, uint32_t index)
{
	struct txm_story_link *link = &story->link[index];
	if (link->older.is_story)
		link->older.story->link[index].newer_story = NULL;
	else
		tusp_setUN(&link->older.tuple, NULL);
	link->older.is_story = false;
	link->older.tuple = NULL;
}

static bool
txm_story_is_visible(struct txm_story *story, struct txn *txn,
		      struct tuple **visible_tuple, bool prepared_ok,
		     bool *own_change)
{
	*own_change = false;
	struct txn_stmt *dels = story->del_stmt;
	while (dels != NULL) {
		if (dels->txn == txn) {
			/* deleted by us. */
			*visible_tuple = NULL;
			*own_change = true;
			return true;
		}
		dels = dels->next_in_del_list;
	}
	if (prepared_ok && story->del_psn != 0) {
		/* deleted by at least prepared TX. */
		*visible_tuple = NULL;
		return true;
	}
	if (story->del_psn != 0 && story->del_stmt == NULL) {
		/* deleted by committed TX. */
		*visible_tuple = NULL;
		return true;
	}

	if (story->add_stmt != NULL && story->add_stmt->txn == txn) {
		/* added by us. */
		*visible_tuple = story->tuple;
		*own_change = true;
		return true;
	}
	if (prepared_ok && story->add_psn != 0) {
		/* added by at least prepared TX. */
		*visible_tuple = story->tuple;
		return true;
	}
	if (story->add_psn != 0 && story->add_stmt == NULL) {
		/* added by committed TX. */
		*visible_tuple = story->tuple;
		return true;
	}
	return false;
}

/** Temporary allocated on region that stores a conflicting TX. */
struct txn_conflict
{
	struct txn *other_txn;
	struct txn_conflict *next;
};

static int
txm_save_conflict(struct txn *other_txn, struct txn_conflict **coflicts_head,
		  struct region *region)
{
	size_t err_size;
	struct txn_conflict *next_conflict;
	next_conflict = region_alloc_object(region, struct txn_conflict,
					    &err_size);
	if (next_conflict == NULL) {
		diag_set(OutOfMemory, err_size,
			 "txn_region", "txn conflict");
		return -1;
	}
	next_conflict->other_txn = other_txn;
	next_conflict->next = *coflicts_head;
	*coflicts_head = next_conflict;
	return 0;
}

int
txm_story_find_visible(struct txn_stmt *stmt, struct txm_story *story,
		       uint32_t ind, struct tuple **visible_replaced,
		       struct txn_conflict **collected_conflicts,
		       struct region *region)
{
	while (true) {
		if (!story->link[ind].older.is_story) {
			/*
			 * the tuple is so old that we doesn't
			 * know its story.
			 */
			*visible_replaced = story->link[ind].older.tuple;
			assert(*visible_replaced == NULL ||
			       !(*visible_replaced)->is_dirty);
			break;
		}
		story = story->link[ind].older.story;
		bool unused;
		if (txm_story_is_visible(story, stmt->txn,
					 visible_replaced, true, &unused))
			break;

		/*
		 * We skip the story but once the story is committed
		 * before out TX that may cause conflict.
		 * The conflict will be inavoidable if this statement
		 * relies on old_tuple. If not (it's a replace),
		 * the conflict will take place only for secondary
		 * index if the story will not be overwritten in primary
		 * index.
		 */
		bool cross_conflict = false;
		if (stmt->preserve_old_tuple) {
			cross_conflict = true;
		} else if (ind != 0) {
			struct txm_story *look_up = story;
			cross_conflict = true;
			while (look_up->link[ind].newer_story != NULL) {
				struct txm_story *over;
				over = look_up->link[ind].newer_story;
				if (over->add_stmt->txn == stmt->txn) {
					cross_conflict = false;
					break;
				}
				look_up = over;
			}
		}
		if (cross_conflict) {
			if (txm_save_conflict(story->add_stmt->txn,
					      collected_conflicts,
					      region) != 0)
				return -1;

		}
	}
	return 0;
}

int
txm_history_link_stmt(struct txn_stmt *stmt,
		      struct tuple *old_tuple, struct tuple *new_tuple,
		      enum dup_replace_mode mode, struct tuple **result)
{
	struct txm_story *add_story = NULL;
	uint32_t add_story_linked = 0;
	struct txm_story *del_story = NULL;
	bool del_story_created = false;
	struct region *region = &stmt->txn->region;
	size_t region_svp = region_used(region);

	/**
	 * List of transactions that will conflict us once one of them
	 * become committed.
	 */
	struct txn_conflict *collected_conflicts = NULL;

	if (new_tuple != NULL) {
		add_story = txm_story_new_add_stmt(new_tuple, stmt);
		if (add_story == NULL)
			goto fail;

		for (uint32_t i = 0; i < stmt->space->index_count; i++) {
			struct tuple *replaced;
			struct index *index = stmt->space->index[i];
			if (index_replace(index, NULL, new_tuple,
					  DUP_REPLACE_OR_INSERT,
					  &replaced) != 0)
				goto fail;
			txm_story_link_tuple(add_story, replaced, i);
			add_story_linked++;

			struct tuple *visible_replaced = NULL;
			if (txm_story_find_visible(stmt, add_story, i,
						   &visible_replaced,
						   &collected_conflicts,
						   region) != 0)
				goto fail;

			uint32_t errcode;
			errcode = replace_check_dup(old_tuple,
						    visible_replaced,
						    i == 0 ? mode : DUP_INSERT);
			if (errcode != 0) {
				struct space *sp = stmt->space;
				if (sp != NULL)
					diag_set(ClientError, errcode,
						 sp->index[i]->def->name,
						 space_name(sp));
				goto fail;
			}

			if (i == 0) {
				old_tuple = visible_replaced;
				*result = visible_replaced;
			}
		}
	} else {
		assert(old_tuple != NULL);
		*result = old_tuple;
	}

	struct tuple *del_tuple = NULL;
	if (new_tuple != NULL) {
		struct txm_story_link *link = &add_story->link[0];
		if (link->older.is_story) {
			del_story = link->older.story;
			del_tuple = del_story->tuple;
		} else {
			del_tuple = link->older.tuple;
		}
	} else {
		del_tuple = old_tuple;
	}
	if (del_tuple != NULL && del_story == NULL) {
		if (del_tuple->is_dirty) {
			del_story = txm_story_get(del_tuple);
		} else {
			del_story = txm_story_new_del_stmt(del_tuple, stmt);
			if (del_story == NULL)
				goto fail;
			del_story_created = true;
		}
	}
	if (new_tuple != NULL && del_story_created) {
		for (uint32_t i = 0; i < add_story->index_count; i++) {
			struct txm_story_link *link = &add_story->link[i];
			if (link->older.is_story)
				continue;
			if (link->older.tuple == del_tuple) {
				txm_story_unlink(add_story, i);
				txm_story_link_story(add_story, del_story, i);
			}
		}
	}
	if (del_story != NULL && del_story->del_stmt != stmt) {
		stmt->next_in_del_list = del_story->del_stmt;
		del_story->del_stmt = stmt;
		stmt->del_story = del_story;
	}

	while (collected_conflicts != NULL) {
		if (txm_cause_conflict(collected_conflicts->other_txn,
				       stmt->txn) != 0) {
			goto fail;
		}
		collected_conflicts = collected_conflicts->next;
	}
	region_truncate(region, region_svp);
	if (stmt->new_tuple != NULL)
		tuple_ref(stmt->new_tuple);
	if (*result != NULL)
		tuple_ref(*result);
	return 0;

fail:
	if (add_story != NULL) {
		while (add_story_linked > 0) {
			--add_story_linked;
			uint32_t i = add_story_linked;

			struct index *index = stmt->space->index[i];
			struct txm_story_link *link = &add_story->link[i];
			struct tuple *was = txm_story_older_tuple(link);
			struct tuple *unused;
			if (index_replace(index, new_tuple, was,
					  DUP_INSERT,
					  &unused) != 0) {
				diag_log();
				unreachable();
				panic("failed to rollback change");
			}

			txm_story_unlink(stmt->add_story, i);

		}
		txm_story_delete_add_stmt(stmt->add_story);
	}

	if (del_story != NULL && del_story->del_stmt == stmt) {
		del_story->del_stmt = stmt->next_in_del_list;
		stmt->next_in_del_list = NULL;
	}

	if (del_story_created)
		txm_story_delete_del_stmt(stmt->del_story);
	else
		stmt->del_story = NULL;

	region_truncate(region, region_svp);
	return -1;
}

void
txm_history_unlink_stmt(struct txn_stmt *stmt)
{
	if (stmt->add_story != NULL) {
		assert(stmt->add_story->tuple == stmt->new_tuple);
		struct txm_story *story = stmt->add_story;

		for (uint32_t i = 0; i < story->index_count; i++) {
			struct txm_story_link *link = &story->link[i];
			if (link->newer_story == NULL) {
				struct tuple *unused;
				struct index *index = stmt->space->index[i];
				struct tuple *was = txm_story_older_tuple(link);
				if (index_replace(index, story->tuple, was,
						  DUP_INSERT, &unused) != 0) {
					diag_log();
					unreachable();
					panic("failed to rollback change");
				}
			} else {
				struct txm_story *newer = link->newer_story;
				assert(newer->link[i].older.is_story);
				assert(newer->link[i].older.story == story);
				txm_story_unlink(newer, i);
				if (link->older.is_story) {
					struct txm_story *to = link->older.story;
					txm_story_link_story(newer,to, i);
				} else {
					struct tuple *to = link->older.tuple;
					txm_story_link_tuple(newer, to, i);
				}
			}
			txm_story_unlink(story, i);
		}
		stmt->add_story->add_stmt = NULL;
		stmt->add_story = NULL;
		tuple_unref(stmt->new_tuple);
	}

	if (stmt->del_story != NULL) {
		struct txm_story *story = stmt->del_story;

		struct txn_stmt **prev = &story->del_stmt;
		while (*prev != stmt) {
			prev = &(*prev)->next_in_del_list;
			assert(*prev != NULL);
		}
		*prev = stmt->next_in_del_list;
		stmt->next_in_del_list = NULL;

		stmt->del_story->del_stmt = NULL;
		stmt->del_story = NULL;
	}
}

void
txm_history_prepare_stmt(struct txn_stmt *stmt)
{
	assert(stmt->txn->psn != 0);

	/* Move story to the past to prepared stories. */

	struct txm_story *story = stmt->add_story;
	uint32_t index_count = story == NULL ? 0 : story->index_count;
	for (uint32_t i = 0; i < index_count; ) {
		if (!story->link[i].older.is_story) {
			/* tuple is old. */
			i++;
			continue;
		}
		struct txm_story *old_story =
			story->link[i].older.story;
		if (old_story->del_psn != 0) {
			/* is psn is set, the change is prepared. */
			i++;
			continue;
		}
		if (old_story->add_psn != 0) {
			/* is psn is set, the change is prepared. */
			i++;
			continue;
		}

		if (old_story->add_stmt != NULL) {
			/* ancient */
			i++;
			continue;
		}
		if (old_story->add_stmt->txn == stmt->txn) {
			/* added by us. */
			i++;
			continue;
		}

		if (old_story->add_stmt->preserve_old_tuple || i != 0)
			old_story->add_stmt->txn->status = TXN_CONFLICTED;

		/* Swap story and old story. */
		struct txm_story_link *link = &story->link[i];
		if (link->newer_story == NULL) {
			/* we have to replace the tuple in index. */
			struct tuple *unused;
			struct index *index = stmt->space->index[i];
			if (index_replace(index, story->tuple,
			                  old_story->tuple,
			                  DUP_INSERT, &unused) != 0) {
				diag_log();
				unreachable();
				panic("failed to rollback change");
			}
		} else {
			struct txm_story *newer = link->newer_story;
			assert(newer->link[i].older.is_story);
			assert(newer->link[i].older.story == story);
			txm_story_unlink(newer, i);
			txm_story_link_story(newer, old_story, i);
		}

		txm_story_unlink(story, i);

		txm_story_unlink(story, i);
		if (old_story->link[i].older.is_story) {
			struct txm_story *to =
				old_story->link[i].older.story;
			txm_story_unlink(old_story, i);
			txm_story_link_story(story, to, i);
		} else {
			struct tuple *to =
				old_story->link[i].older.tuple;
			txm_story_unlink(old_story, i);
			txm_story_link_tuple(story, to, i);
		}

		txm_story_link_story(old_story, story, i);

		if (i == 0) {
			assert(stmt->del_story == old_story);
			assert(story->link[0].older.is_story ||
			       story->link[0].older.tuple == NULL);

			struct txn_stmt *dels = old_story->del_stmt;
			assert(dels != NULL);
			do {
				if (dels->txn != stmt->txn)
					dels->txn->status = TXN_CONFLICTED;
				dels->del_story = NULL;
				struct txn_stmt *next = dels->next_in_del_list;
				dels->next_in_del_list = NULL;
				dels = next;
			} while (dels != NULL);
			old_story->del_stmt = NULL;

			if (story->link[0].older.is_story) {
				struct txm_story *oldest_story =
					story->link[0].older.story;
				dels = oldest_story->del_stmt;
				while (dels != NULL) {
					assert(dels->txn != stmt->txn);
					dels->del_story = NULL;
					struct txn_stmt *next =
						dels->next_in_del_list;
					dels->next_in_del_list = NULL;
					dels = next;
				}
				oldest_story->del_stmt = stmt;
				stmt->del_story = oldest_story;
			}
		}
	}
	if (stmt->add_story != NULL)
		stmt->add_story->add_psn = stmt->txn->psn;

	if (stmt->del_story != NULL)
		stmt->del_story->del_psn = stmt->txn->psn;
}

ssize_t
txm_history_release_stmt(struct txn_stmt *stmt)
{
	size_t res = 0;
	if (stmt->add_story != NULL) {
		assert(stmt->add_story->add_stmt == stmt);
		res += stmt->add_story->tuple->bsize;
		stmt->add_story->add_stmt = NULL;
		stmt->add_story = NULL;
	}
	if (stmt->del_story != NULL) {
		assert(stmt->del_story->del_stmt == stmt);
		assert(stmt->next_in_del_list == NULL);
		res -= stmt->del_story->tuple->bsize;
		tuple_unref(stmt->del_story->tuple);
		stmt->del_story->del_stmt = NULL;
		stmt->del_story = NULL;
	}
	return res;
}

struct tuple *
txm_tuple_clarify_slow(struct txn* txn, struct tuple* tuple, uint32_t index,
                       uint32_t mk_index, bool prepared_ok)
{
	assert(tuple->is_dirty);
	struct txm_story *story = txm_story_get(tuple);
	bool own_change = false;
	struct tuple *result = NULL;

	while (true) {
		if (txm_story_is_visible(story, txn, &result,
					 prepared_ok, &own_change)) {
			break;
		}
		if (story->link[index].older.is_story) {
			story = story->link[index].older.story;
		} else {
			result = story->link[index].older.tuple;
			break;
		}
	}
	(void)own_change; /* TODO: add conflict */
	(void)mk_index; /* TODO: multiindex */
	return result;
}

static void
txm_story_delete(struct txm_story *story)
{
	assert(story->add_stmt == NULL);
	assert(story->del_stmt == NULL);

	if (txm.traverse_all_stories == &story->in_all_stories)
		txm.traverse_all_stories = rlist_next(txm.traverse_all_stories);
	rlist_del(&story->in_all_stories);

	mh_int_t pos = mh_history_find(txm.history, story->tuple, 0);
	assert(pos != mh_end(txm.history));
	mh_history_del(txm.history, pos, 0);

	story->tuple->is_dirty = false;
	tuple_unref(story->tuple);

#ifndef NDEBUG
	/* Expecting to delete fully unlinked story. */
	for (uint32_t i = 0; i < story->index_count; i++) {
		assert(story->link[i].newer_story == NULL);
		assert(story->link[i].older.is_story == false);
		assert(story->link[i].older.tuple == NULL);
	}
#endif

	struct mempool *pool = &txm.txm_story_pool[story->index_count];
	mempool_free(pool, story);
}
