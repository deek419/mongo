/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_txnid_cmp --
 *	Compare transaction IDs for sorting / searching.
 */
int WT_CDECL
__wt_txnid_cmp(const void *v1, const void *v2)
{
	uint64_t id1, id2;

	id1 = *(uint64_t *)v1;
	id2 = *(uint64_t *)v2;

	return ((id1 == id2) ? 0 : WT_TXNID_LT(id1, id2) ? -1 : 1);
}

/*
 * __txn_sort_snapshot --
 *	Sort a snapshot for faster searching and set the min/max bounds.
 */
static void
__txn_sort_snapshot(WT_SESSION_IMPL *session, uint32_t n, uint64_t snap_max)
{
	WT_TXN *txn;

	txn = &session->txn;

	if (n <= 10)
		WT_INSERTION_SORT(txn->snapshot, n, uint64_t, WT_TXNID_LT);
	else
		qsort(txn->snapshot, n, sizeof(uint64_t), __wt_txnid_cmp);

	txn->snapshot_count = n;
	txn->snap_max = snap_max;
	txn->snap_min = (n > 0 && WT_TXNID_LE(txn->snapshot[0], snap_max)) ?
	    txn->snapshot[0] : snap_max;
	F_SET(txn, WT_TXN_HAS_SNAPSHOT);
	WT_ASSERT(session, n == 0 || txn->snap_min != WT_TXN_NONE);
}

/*
 * __wt_txn_release_snapshot --
 *	Release the snapshot in the current transaction.
 */
void
__wt_txn_release_snapshot(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_state = &S2C(session)->txn_global.states[session->id];

	WT_ASSERT(session,
	    txn_state->snap_min == WT_TXN_NONE ||
	    session->txn.isolation == WT_ISO_READ_UNCOMMITTED ||
	    !__wt_txn_visible_all(session, txn_state->snap_min));

	txn_state->snap_min = WT_TXN_NONE;
	F_CLR(txn, WT_TXN_HAS_SNAPSHOT);
}

/*
 * __wt_txn_get_snapshot --
 *	Allocate a snapshot.
 */
void
__wt_txn_get_snapshot(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s, *txn_state;
	uint64_t ckpt_id, current_id, id;
	uint64_t prev_oldest_id, snap_min;
	uint32_t i, n, session_cnt;
	int32_t count;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];

	current_id = snap_min = txn_global->current;
	prev_oldest_id = txn_global->oldest_id;

	/* For pure read-only workloads, avoid scanning. */
	if (prev_oldest_id == current_id) {
		txn_state->snap_min = current_id;
		__txn_sort_snapshot(session, 0, current_id);

		/* Check that the oldest ID has not moved in the meantime. */
		if (prev_oldest_id == txn_global->oldest_id &&
		    txn_global->scan_count == 0)
			return;
	}

	/*
	 * We're going to scan.  Increment the count of scanners to prevent the
	 * oldest ID from moving forwards.  Spin if the count is negative,
	 * which indicates that some thread is moving the oldest ID forwards.
	 */
	do {
		if ((count = txn_global->scan_count) < 0)
			WT_PAUSE();
	} while (count < 0 ||
	    !WT_ATOMIC_CAS4(txn_global->scan_count, count, count + 1));

	/* The oldest ID cannot change until the scan count goes to zero. */
	prev_oldest_id = txn_global->oldest_id;
	current_id = snap_min = txn_global->current;

	/* Walk the array of concurrent transactions. */
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	ckpt_id = txn_global->checkpoint_id;
	for (i = n = 0, s = txn_global->states; i < session_cnt; i++, s++) {
		/* Skip the checkpoint transaction; it is never read from. */
		if (ckpt_id != WT_TXN_NONE && ckpt_id == s->id)
			continue;

		/*
		 * Build our snapshot of any concurrent transaction IDs.
		 *
		 * Ignore:
		 *  - Our own ID: we always read our own updates.
		 *  - The ID if it is older than the oldest ID we saw. This
		 *    can happen if we race with a thread that is allocating
		 *    an ID -- the ID will not be used because the thread will
		 *    keep spinning until it gets a valid one.
		 */
		if (s != txn_state &&
		    (id = s->id) != WT_TXN_NONE &&
		    WT_TXNID_LE(prev_oldest_id, id)) {
			txn->snapshot[n++] = id;
			if (WT_TXNID_LT(id, snap_min))
				snap_min = id;
		}
	}

	/*
	 * If we got a new snapshot, update the published snap_min for this
	 * session.
	 */
	WT_ASSERT(session, WT_TXNID_LE(prev_oldest_id, snap_min));
	WT_ASSERT(session, prev_oldest_id == txn_global->oldest_id);
	txn_state->snap_min = snap_min;

	/* Update the last running ID if we have a much newer value. */
	if (snap_min > txn_global->last_running + 100)
		txn_global->last_running = snap_min;

	WT_ASSERT(session, txn_global->scan_count > 0);
	(void)WT_ATOMIC_SUB4(txn_global->scan_count, 1);

	__txn_sort_snapshot(session, n, current_id);
}

/*
 * __wt_txn_update_oldest --
 *	Sweep the running transactions to update the oldest ID required.
 * !!!
 * If a data-source is calling the WT_EXTENSION_API.transaction_oldest
 * method (for the oldest transaction ID not yet visible to a running
 * transaction), and then comparing that oldest ID against committed
 * transactions to see if updates for a committed transaction are still
 * visible to running transactions, the oldest transaction ID may be
 * the same as the last committed transaction ID, if the transaction
 * state wasn't refreshed after the last transaction committed.  Push
 * past the last committed transaction.
*/
void
__wt_txn_update_oldest(WT_SESSION_IMPL *session, int force)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *oldest_session;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	uint64_t ckpt_id, current_id, id, oldest_id, prev_oldest_id, snap_min;
	uint32_t i, session_cnt;
	int32_t count;
	int last_running_moved;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	current_id = snap_min = txn_global->current;
	oldest_session = NULL;
	prev_oldest_id = txn_global->oldest_id;

	/*
	 * For pure read-only workloads, or if the update isn't forced and the
	 * oldest ID isn't too far behind, avoid scanning.
	 */
	if (prev_oldest_id == current_id ||
	    (!force && WT_TXNID_LT(current_id, prev_oldest_id + 100)))
		return;

	/*
	 * We're going to scan.  Increment the count of scanners to prevent the
	 * oldest ID from moving forwards.  Spin if the count is negative,
	 * which indicates that some thread is moving the oldest ID forwards.
	 */
	do {
		if ((count = txn_global->scan_count) < 0)
			WT_PAUSE();
	} while (count < 0 ||
	    !WT_ATOMIC_CAS4(txn_global->scan_count, count, count + 1));

	/* The oldest ID cannot change until the scan count goes to zero. */
	prev_oldest_id = txn_global->oldest_id;
	current_id = oldest_id = snap_min = txn_global->current;

	/* Walk the array of concurrent transactions. */
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	ckpt_id = txn_global->checkpoint_id;
	for (i = 0, s = txn_global->states; i < session_cnt; i++, s++) {
		/* Skip the checkpoint transaction; it is never read from. */
		if (ckpt_id != WT_TXN_NONE && ckpt_id == s->id)
			continue;

		/*
		 * Update the oldest ID.
		 *
		 * Ignore: IDs older than the oldest ID we saw. This can happen
		 * if we race with a thread that is allocating an ID -- the ID
		 * will not be used because the thread will keep spinning until
		 * it gets a valid one.
		 */
		if ((id = s->id) != WT_TXN_NONE &&
		    WT_TXNID_LE(prev_oldest_id, id) &&
		    WT_TXNID_LT(id, snap_min))
			snap_min = id;

		/*
		 * !!!
		 * Note: Don't ignore snap_min values older than the previous
		 * oldest ID.  Read-uncommitted operations publish snap_min
		 * values without incrementing scan_count to protect the global
		 * table.  See the comment in __wt_txn_cursor_op for
		 * more details.
		 */
		if ((id = s->snap_min) != WT_TXN_NONE &&
		    WT_TXNID_LT(id, oldest_id)) {
			oldest_id = id;
			oldest_session = &conn->sessions[i];
		}
	}

	if (WT_TXNID_LT(snap_min, oldest_id))
		oldest_id = snap_min;

	/* The oldest ID can't move past any named snapshots. */
	if ((id = txn_global->nsnap_oldest_id) != WT_TXN_NONE &&
	    WT_TXNID_LT(id, oldest_id))
		oldest_id = id;

	/* Update the last running ID. */
	if (WT_TXNID_LT(txn_global->last_running, snap_min)) {
		txn_global->last_running = snap_min;
		last_running_moved = 1;
	} else
		last_running_moved = 0;

	/* Update the oldest ID. */
	if (WT_TXNID_LT(prev_oldest_id, oldest_id) &&
	    WT_ATOMIC_CAS4(txn_global->scan_count, 1, -1)) {
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		ckpt_id = txn_global->checkpoint_id;
		for (i = 0, s = txn_global->states; i < session_cnt; i++, s++) {
			/*
			 * Skip the checkpoint transaction; it is never read
			 * from.
			 */
			if (ckpt_id != WT_TXN_NONE && ckpt_id == s->id)
				continue;

			if ((id = s->id) != WT_TXN_NONE &&
			    WT_TXNID_LT(id, oldest_id))
				oldest_id = id;
			if ((id = s->snap_min) != WT_TXN_NONE &&
			    WT_TXNID_LT(id, oldest_id))
				oldest_id = id;
		}
		if (WT_TXNID_LT(txn_global->oldest_id, oldest_id))
			txn_global->oldest_id = oldest_id;
		txn_global->scan_count = 0;
	} else {
		if (WT_VERBOSE_ISSET(session, WT_VERB_TRANSACTION) &&
		    current_id - oldest_id > 10000 && last_running_moved &&
		    oldest_session != NULL) {
			(void)__wt_verbose(session, WT_VERB_TRANSACTION,
			    "old snapshot %" PRIu64
			    " pinned in session %d [%s]"
			    " with snap_min %" PRIu64 "\n",
			    oldest_id, oldest_session->id,
			    oldest_session->lastop,
			    oldest_session->txn.snap_min);
		}
		WT_ASSERT(session, txn_global->scan_count > 0);
		(void)WT_ATOMIC_SUB4(txn_global->scan_count, 1);
	}
}

/*
 * __wt_txn_config --
 *	Configure a transaction.
 */
int
__wt_txn_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_TXN *txn;

	txn = &session->txn;

	WT_RET(__wt_config_gets_def(session, cfg, "isolation", 0, &cval));
	if (cval.len != 0)
		txn->isolation =
		    WT_STRING_MATCH("snapshot", cval.str, cval.len) ?
		    WT_ISO_SNAPSHOT :
		    WT_STRING_MATCH("read-committed", cval.str, cval.len) ?
		    WT_ISO_READ_COMMITTED : WT_ISO_READ_UNCOMMITTED;

	/*
	 * The default sync setting is inherited from the connection, but can
	 * be overridden by an explicit "sync" setting for this transaction.
	 *
	 * !!! This is an unusual use of the config code: the "default" value
	 * we pass in is inherited from the connection.  If flush is not set in
	 * the connection-wide flag and not overridden here, we end up clearing
	 * all flags.  We want to distinguish between inheriting implicitly
	 * and explicitly.
	 */
	F_CLR(txn, WT_TXN_SYNC_SET);
	WT_RET(__wt_config_gets_def(session, cfg, "sync", UINT_MAX, &cval));
	if (cval.val == 0 || cval.val == 1)
		/*
		 * This is an explicit setting of sync.  Set the flag so
		 * that we know not to overwrite it in commit_transaction.
		 */
		F_SET(txn, WT_TXN_SYNC_SET);
	if (cval.val == 0 || (!F_ISSET(txn, WT_TXN_SYNC_SET) &&
	    !FLD_ISSET(txn->txn_logsync, WT_LOG_FLUSH)))
		/*
		 * Only reset the value if the setting was turned off.
		 */
		txn->txn_logsync = 0;

	WT_RET(__wt_config_gets_def(session, cfg, "snapshot", 0, &cval));
	if (cval.len > 0)
		/*
		 * The layering here isn't ideal - the named snapshot get
		 * function does both validation and setup. Otherwise we'd
		 * need to walk the list of named snapshots twice during
		 * transaction open.
		 */
		WT_RET(__wt_txn_named_snapshot_get(session, &cval));

	return (0);
}

/*
 * __wt_txn_release --
 *	Release the resources associated with the current transaction.
 */
void
__wt_txn_release(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	WT_ASSERT(session, txn->mod_count == 0);
	txn->notify = NULL;

	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/* Clear the transaction's ID from the global table. */
	if (F_ISSET(txn, WT_TXN_HAS_ID)) {
		WT_ASSERT(session, txn_state->id != WT_TXN_NONE &&
		    txn->id != WT_TXN_NONE);
		WT_PUBLISH(txn_state->id, WT_TXN_NONE);
		txn->id = WT_TXN_NONE;
	}

	/* Free the scratch buffer allocated for logging. */
	__wt_logrec_free(session, &txn->logrec);

	/* Discard any memory from the session's split stash that we can. */
	WT_ASSERT(session, session->split_gen == 0);
	if (session->split_stash_cnt > 0)
		__wt_split_stash_discard(session);

	/*
	 * Reset the transaction state to not running and release the snapshot.
	 */
	__wt_txn_release_snapshot(session);
	txn->isolation = session->isolation;
	F_CLR(txn, WT_TXN_ERROR | WT_TXN_HAS_ID |
	    WT_TXN_NAMED_SNAPSHOT | WT_TXN_READONLY | WT_TXN_RUNNING);
}

/*
 * __wt_txn_commit --
 *	Commit the current transaction.
 */
int
__wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_OP *op;
	u_int i;

	txn = &session->txn;
	conn = S2C(session);
	WT_ASSERT(session, !F_ISSET(txn, WT_TXN_ERROR));

	if (!F_ISSET(txn, WT_TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/*
	 * The default sync setting is inherited from the connection, but can
	 * be overridden by an explicit "sync" setting for this transaction.
	 */
	WT_RET(__wt_config_gets_def(session, cfg, "sync", 0, &cval));
	/*
	 * If the user chose the default setting, check whether
	 * transaction_sync is enabled in the connection.  If it isn't,
	 * we want to clear the field.  Otherwise check for specific
	 * settings.  We don't need to check for "on" because that is
	 * the default inherited from the connection.  If the user set
	 * anything in begin_transaction, we only override with an
	 * explicit setting.
	 */
	if (cval.len == 0) {
		if (!FLD_ISSET(txn->txn_logsync, WT_LOG_FLUSH) &&
		    !F_ISSET(txn, WT_TXN_SYNC_SET))
			txn->txn_logsync = 0;
	} else {
		/*
		 * If the caller already set sync on begin_transaction then
		 * they should not be using sync on commit_transaction.
		 * Flag that as an error.
		 */
		if (F_ISSET(txn, WT_TXN_SYNC_SET))
			WT_RET_MSG(session, EINVAL,
			    "Sync already set during begin_transaction.");
		if (WT_STRING_MATCH("background", cval.str, cval.len))
			txn->txn_logsync = WT_LOG_BACKGROUND;
		else if (WT_STRING_MATCH("off", cval.str, cval.len))
			txn->txn_logsync = 0;
		/*
		 * We don't need to check for "on" here because that is the
		 * default to inherit from the connection setting.
		 */
	}

	/* Commit notification. */
	if (txn->notify != NULL)
		WT_TRET(txn->notify->notify(txn->notify,
		    (WT_SESSION *)session, txn->id, 1));

	/* If we are logging, write a commit log record. */
	if (ret == 0 && txn->mod_count > 0 &&
	    FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) &&
	    !F_ISSET(session, WT_SESSION_NO_LOGGING)) {
		/*
		 * We are about to block on I/O writing the log.
		 * Release our snapshot in case it is keeping data pinned.
		 * This is particularly important for checkpoints.
		 */
		__wt_txn_release_snapshot(session);
		ret = __wt_txn_log_commit(session, cfg);
	}

	/*
	 * If anything went wrong, roll back.
	 *
	 * !!!
	 * Nothing can fail after this point.
	 */
	if (ret != 0) {
		WT_TRET(__wt_txn_rollback(session, cfg));
		return (ret);
	}

	/* Free memory associated with updates. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++)
		__wt_txn_op_free(session, op);
	txn->mod_count = 0;

	/*
	 * We are about to release the snapshot: copy values into any
	 * positioned cursors so they don't point to updates that could be
	 * freed once we don't have a transaction ID pinned.
	 */
	if (session->ncursors > 0)
		WT_RET(__wt_session_copy_values(session));

	__wt_txn_release(session);
	return (0);
}

/*
 * __wt_txn_rollback --
 *	Roll back the current transaction.
 */
int
__wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_TXN *txn;
	WT_TXN_OP *op;
	u_int i;

	WT_UNUSED(cfg);

	txn = &session->txn;
	if (!F_ISSET(txn, WT_TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/* Rollback notification. */
	if (txn->notify != NULL)
		WT_TRET(txn->notify->notify(txn->notify, (WT_SESSION *)session,
		    txn->id, 0));

	/* Rollback updates. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
		/* Metadata updates are never rolled back. */
		if (op->fileid == WT_METAFILE_ID)
			continue;

		switch (op->type) {
		case WT_TXN_OP_BASIC:
		case WT_TXN_OP_INMEM:
			op->u.upd->txnid = WT_TXN_ABORTED;
			break;
		case WT_TXN_OP_REF:
			__wt_delete_page_rollback(session, op->u.ref);
			break;
		case WT_TXN_OP_TRUNCATE_COL:
		case WT_TXN_OP_TRUNCATE_ROW:
			/*
			 * Nothing to do: these operations are only logged for
			 * recovery.  The in-memory changes will be rolled back
			 * with a combination of WT_TXN_OP_REF and
			 * WT_TXN_OP_INMEM operations.
			 */
			break;
		}

		/* Free any memory allocated for the operation. */
		__wt_txn_op_free(session, op);
	}
	txn->mod_count = 0;

	__wt_txn_release(session);
	return (ret);
}

/*
 * __wt_txn_init --
 *	Initialize a session's transaction data.
 */
int
__wt_txn_init(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	txn->id = WT_TXN_NONE;

	WT_RET(__wt_calloc_def(session,
	    S2C(session)->session_size, &txn->snapshot));

#ifdef HAVE_DIAGNOSTIC
	if (S2C(session)->txn_global.states != NULL) {
		WT_TXN_STATE *txn_state;
		txn_state = &S2C(session)->txn_global.states[session->id];
		WT_ASSERT(session, txn_state->snap_min == WT_TXN_NONE);
	}
#endif

	/*
	 * Take care to clean these out in case we are reusing the transaction
	 * for eviction.
	 */
	txn->mod = NULL;

	txn->isolation = session->isolation;
	return (0);
}

/*
 * __wt_txn_stats_update --
 *	Update the transaction statistics for return to the application.
 */
void
__wt_txn_stats_update(WT_SESSION_IMPL *session)
{
	WT_TXN_GLOBAL *txn_global;
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS *stats;
	uint64_t checkpoint_snap_min;

	conn = S2C(session);
	txn_global = &conn->txn_global;
	stats = &conn->stats;
	checkpoint_snap_min = txn_global->checkpoint_snap_min;

	WT_STAT_SET(stats, txn_pinned_range,
	    txn_global->current - txn_global->oldest_id);

	WT_STAT_SET(stats, txn_pinned_checkpoint_range,
	    checkpoint_snap_min == WT_TXN_NONE ?
	    0 : txn_global->current - checkpoint_snap_min);
}

/*
 * __wt_txn_destroy --
 *	Destroy a session's transaction data.
 */
void
__wt_txn_destroy(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	__wt_free(session, txn->mod);
	__wt_free(session, txn->snapshot);
}

/*
 * __wt_txn_global_init --
 *	Initialize the global transaction state.
 */
int
__wt_txn_global_init(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	u_int i;

	WT_UNUSED(cfg);
	conn = S2C(session);

	txn_global = &conn->txn_global;
	txn_global->current = txn_global->last_running =
	    txn_global->oldest_id = WT_TXN_FIRST;

	WT_RET(__wt_rwlock_alloc(session,
	    &txn_global->nsnap_rwlock, "named snapshot lock"));
	txn_global->nsnap_oldest_id = WT_TXN_NONE;
	STAILQ_INIT(&txn_global->nsnaph);

	WT_RET(__wt_calloc_def(
	    session, conn->session_size, &txn_global->states));

	for (i = 0, s = txn_global->states; i < conn->session_size; i++, s++)
		s->id = s->snap_min = WT_TXN_NONE;

	return (0);
}

/*
 * __wt_txn_global_destroy --
 *	Destroy the global transaction state.
 */
int
__wt_txn_global_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	if (txn_global == NULL)
		return (0);

	WT_TRET(__wt_rwlock_destroy(session, &txn_global->nsnap_rwlock));
	__wt_free(session, txn_global->states);

	return (ret);
}
