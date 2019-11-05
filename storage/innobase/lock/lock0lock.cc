/*****************************************************************************

Copyright (c) 1996, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2014, 2019, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file lock/lock0lock.cc
The transaction lock system

Created 5/7/1996 Heikki Tuuri
*******************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "univ.i"

#include <mysql/service_thd_error_context.h>
#include <sql_class.h>

#include "lock0lock.h"
#include "lock0priv.h"
#include "dict0mem.h"
#include "trx0purge.h"
#include "trx0sys.h"
#include "ut0vec.h"
#include "btr0cur.h"
#include "row0sel.h"
#include "row0mysql.h"
#include "row0vers.h"
#include "pars0pars.h"

#include <set>

#ifdef WITH_WSREP
#include <mysql/service_wsrep.h>
#endif /* WITH_WSREP */

/** Lock scheduling algorithm */
ulong innodb_lock_schedule_algorithm;

/** The value of innodb_deadlock_detect */
my_bool	innobase_deadlock_detect;

/*********************************************************************//**
Checks if a waiting record lock request still has to wait in a queue.
@return lock that is causing the wait */
static
const lock_t*
lock_rec_has_to_wait_in_queue(
/*==========================*/
	const lock_t*	wait_lock);	/*!< in: waiting record lock */

/** Grant a lock to a waiting lock request and release the waiting transaction
after lock_reset_lock_and_trx_wait() has been called. */
static void lock_grant_after_reset(lock_t* lock);

extern "C" void thd_rpl_deadlock_check(MYSQL_THD thd, MYSQL_THD other_thd);
extern "C" int thd_need_wait_reports(const MYSQL_THD thd);
extern "C" int thd_need_ordering_with(const MYSQL_THD thd, const MYSQL_THD other_thd);

extern "C" int thd_deadlock_victim_preference(const MYSQL_THD thd1, const MYSQL_THD thd2);

/** Pretty-print a table lock.
@param[in,out]	file	output stream
@param[in]	lock	table lock */
static void lock_table_print(FILE* file, const lock_t* lock);

/** Pretty-print a record lock.
@param[in,out]	file	output stream
@param[in]	lock	record lock
@param[in,out]	mtr	mini-transaction for accessing the record */
static void lock_rec_print(FILE* file, const lock_t* lock, mtr_t& mtr);

/** Deadlock checker. */
class DeadlockChecker {
public:
	/** Check if a joining lock request results in a deadlock.
	If a deadlock is found, we will resolve the deadlock by
	choosing a victim transaction and rolling it back.
	We will attempt to resolve all deadlocks.

	@param[in]	lock	the lock request
	@param[in,out]	trx	transaction requesting the lock

	@return trx if it was chosen as victim
	@retval	NULL if another victim was chosen,
	or there is no deadlock (any more) */
	static const trx_t* check_and_resolve(const lock_t* lock, trx_t* trx);

private:
	/** Do a shallow copy. Default destructor OK.
	@param trx the start transaction (start node)
	@param wait_lock lock that a transaction wants
	@param mark_start visited node counter
	@param report_waiters whether to call thd_rpl_deadlock_check() */
	DeadlockChecker(
		const trx_t*	trx,
		const lock_t*	wait_lock,
		ib_uint64_t	mark_start,
		bool report_waiters)
		:
		m_cost(),
		m_start(trx),
		m_too_deep(),
		m_wait_lock(wait_lock),
		m_mark_start(mark_start),
		m_n_elems(),
		m_report_waiters(report_waiters)
	{
	}

	/** Check if the search is too deep. */
	bool is_too_deep() const
	{
		return(m_n_elems > LOCK_MAX_DEPTH_IN_DEADLOCK_CHECK
		       || m_cost > LOCK_MAX_N_STEPS_IN_DEADLOCK_CHECK);
	}

	/** Save current state.
	@param lock lock to push on the stack.
	@param heap_no the heap number to push on the stack.
	@return false if stack is full. */
	bool push(const lock_t*	lock, ulint heap_no)
	{
		ut_ad((lock_get_type_low(lock) & LOCK_REC)
		      || (lock_get_type_low(lock) & LOCK_TABLE));

		ut_ad(((lock_get_type_low(lock) & LOCK_TABLE) != 0)
		      == (heap_no == ULINT_UNDEFINED));

		/* Ensure that the stack is bounded. */
		if (m_n_elems >= UT_ARR_SIZE(s_states)) {
			return(false);
		}

		state_t&	state = s_states[m_n_elems++];

		state.m_lock = lock;
		state.m_wait_lock = m_wait_lock;
		state.m_heap_no =heap_no;

		return(true);
	}

	/** Restore state.
	@param[out] lock current lock
	@param[out] heap_no current heap_no */
	void pop(const lock_t*& lock, ulint& heap_no)
	{
		ut_a(m_n_elems > 0);

		const state_t&	state = s_states[--m_n_elems];

		lock = state.m_lock;
		heap_no = state.m_heap_no;
		m_wait_lock = state.m_wait_lock;
	}

	/** Check whether the node has been visited.
	@param lock lock to check
	@return true if the node has been visited */
	bool is_visited(const lock_t* lock) const
	{
		return(lock->trx->lock.deadlock_mark > m_mark_start);
	}

	/** Get the next lock in the queue that is owned by a transaction
	whose sub-tree has not already been searched.
	Note: "next" here means PREV for table locks.
	@param lock Lock in queue
	@param heap_no heap_no if lock is a record lock else ULINT_UNDEFINED
	@return next lock or NULL if at end of queue */
	const lock_t* get_next_lock(const lock_t* lock, ulint heap_no) const;

	/** Get the first lock to search. The search starts from the current
	wait_lock. What we are really interested in is an edge from the
	current wait_lock's owning transaction to another transaction that has
	a lock ahead in the queue. We skip locks where the owning transaction's
	sub-tree has already been searched.

	Note: The record locks are traversed from the oldest lock to the
	latest. For table locks we go from latest to oldest.

	For record locks, we first position the iterator on first lock on
	the page and then reposition on the actual heap_no. This is required
	due to the way the record lock has is implemented.

	@param[out] heap_no if rec lock, else ULINT_UNDEFINED.

	@return first lock or NULL */
	const lock_t* get_first_lock(ulint* heap_no) const;

	/** Notify that a deadlock has been detected and print the conflicting
	transaction info.
	@param lock lock causing deadlock */
	void notify(const lock_t* lock) const;

	/** Select the victim transaction that should be rolledback.
	@return victim transaction */
	const trx_t* select_victim() const;

	/** Rollback transaction selected as the victim. */
	void trx_rollback();

	/** Looks iteratively for a deadlock. Note: the joining transaction
	may have been granted its lock by the deadlock checks.

	@return 0 if no deadlock else the victim transaction.*/
	const trx_t* search();

	/** Print transaction data to the deadlock file and possibly to stderr.
	@param trx transaction
	@param max_query_len max query length to print */
	static void print(const trx_t* trx, ulint max_query_len);

	/** rewind(3) the file used for storing the latest detected deadlock
	and print a heading message to stderr if printing of all deadlocks to
	stderr is enabled. */
	static void start_print();

	/** Print lock data to the deadlock file and possibly to stderr.
	@param lock record or table type lock */
	static void print(const lock_t* lock);

	/** Print a message to the deadlock file and possibly to stderr.
	@param msg message to print */
	static void print(const char* msg);

	/** Print info about transaction that was rolled back.
	@param trx transaction rolled back
	@param lock lock trx wants */
	static void rollback_print(const trx_t* trx, const lock_t* lock);

private:
	/** DFS state information, used during deadlock checking. */
	struct state_t {
		const lock_t*	m_lock;		/*!< Current lock */
		const lock_t*	m_wait_lock;	/*!< Waiting for lock */
		ulint		m_heap_no;	/*!< heap number if rec lock */
	};

	/** Used in deadlock tracking. Protected by lock_sys->mutex. */
	static ib_uint64_t	s_lock_mark_counter;

	/** Calculation steps thus far. It is the count of the nodes visited. */
	ulint			m_cost;

	/** Joining transaction that is requesting a lock in an
	incompatible mode */
	const trx_t*		m_start;

	/** TRUE if search was too deep and was aborted */
	bool			m_too_deep;

	/** Lock that trx wants */
	const lock_t*		m_wait_lock;

	/**  Value of lock_mark_count at the start of the deadlock check. */
	ib_uint64_t		m_mark_start;

	/** Number of states pushed onto the stack */
	size_t			m_n_elems;

	/** This is to avoid malloc/free calls. */
	static state_t		s_states[MAX_STACK_SIZE];

	/** Set if thd_rpl_deadlock_check() should be called for waits. */
	const bool m_report_waiters;
};

/** Counter to mark visited nodes during deadlock search. */
ib_uint64_t	DeadlockChecker::s_lock_mark_counter = 0;

/** The stack used for deadlock searches. */
DeadlockChecker::state_t	DeadlockChecker::s_states[MAX_STACK_SIZE];

#ifdef UNIV_DEBUG
/*********************************************************************//**
Validates the lock system.
@return TRUE if ok */
static
bool
lock_validate();
/*============*/

/*********************************************************************//**
Validates the record lock queues on a page.
@return TRUE if ok */
static
ibool
lock_rec_validate_page(
/*===================*/
	const buf_block_t*	block)	/*!< in: buffer block */
	MY_ATTRIBUTE((warn_unused_result));
#endif /* UNIV_DEBUG */

/* The lock system */
lock_sys_t*	lock_sys	= NULL;

/** We store info on the latest deadlock error to this buffer. InnoDB
Monitor will then fetch it and print */
static bool	lock_deadlock_found = false;

/** Only created if !srv_read_only_mode */
static FILE*		lock_latest_err_file;

/*********************************************************************//**
Reports that a transaction id is insensible, i.e., in the future. */
void
lock_report_trx_id_insanity(
/*========================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const offset_t*	offsets,	/*!< in: rec_get_offsets(rec, index) */
	trx_id_t	max_trx_id)	/*!< in: trx_sys_get_max_trx_id() */
{
	ib::error()
		<< "Transaction id " << trx_id
		<< " associated with record" << rec_offsets_print(rec, offsets)
		<< " in index " << index->name
		<< " of table " << index->table->name
		<< " is greater than the global counter " << max_trx_id
		<< "! The table is corrupted.";
}

/*********************************************************************//**
Checks that a transaction id is sensible, i.e., not in the future.
@return true if ok */
#ifdef UNIV_DEBUG

#else
static MY_ATTRIBUTE((warn_unused_result))
#endif
bool
lock_check_trx_id_sanity(
/*=====================*/
	trx_id_t	trx_id,		/*!< in: trx id */
	const rec_t*	rec,		/*!< in: user record */
	dict_index_t*	index,		/*!< in: index */
	const offset_t*	offsets)	/*!< in: rec_get_offsets(rec, index) */
{
	ut_ad(rec_offs_validate(rec, index, offsets));

	trx_id_t	max_trx_id = trx_sys_get_max_trx_id();
	bool		is_ok = trx_id < max_trx_id;

	if (!is_ok) {
		lock_report_trx_id_insanity(
			trx_id, rec, index, offsets, max_trx_id);
	}

	return(is_ok);
}

/*********************************************************************//**
Checks that a record is seen in a consistent read.
@return true if sees, or false if an earlier version of the record
should be retrieved */
bool
lock_clust_rec_cons_read_sees(
/*==========================*/
	const rec_t*	rec,	/*!< in: user record which should be read or
				passed over by a read cursor */
	dict_index_t*	index,	/*!< in: clustered index */
	const offset_t*	offsets,/*!< in: rec_get_offsets(rec, index) */
	ReadView*	view)	/*!< in: consistent read view */
{
	ut_ad(dict_index_is_clust(index));
	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));

	/* Temp-tables are not shared across connections and multiple
	transactions from different connections cannot simultaneously
	operate on same temp-table and so read of temp-table is
	always consistent read. */
	if (srv_read_only_mode || dict_table_is_temporary(index->table)) {
		ut_ad(view == 0 || dict_table_is_temporary(index->table));
		return(true);
	}

	/* NOTE that we call this function while holding the search
	system latch. */

	trx_id_t	trx_id = row_get_rec_trx_id(rec, index, offsets);

	return(view->changes_visible(trx_id, index->table->name));
}

/*********************************************************************//**
Checks that a non-clustered index record is seen in a consistent read.

NOTE that a non-clustered index page contains so little information on
its modifications that also in the case false, the present version of
rec may be the right, but we must check this from the clustered index
record.

@return true if certainly sees, or false if an earlier version of the
clustered index record might be needed */
bool
lock_sec_rec_cons_read_sees(
/*========================*/
	const rec_t*		rec,	/*!< in: user record which
					should be read or passed over
					by a read cursor */
	const dict_index_t*	index,	/*!< in: index */
	const ReadView*	view)	/*!< in: consistent read view */
{
	ut_ad(page_rec_is_user_rec(rec));

	/* NOTE that we might call this function while holding the search
	system latch. */

	if (recv_recovery_is_on()) {

		return(false);

	} else if (dict_table_is_temporary(index->table)) {

		/* Temp-tables are not shared across connections and multiple
		transactions from different connections cannot simultaneously
		operate on same temp-table and so read of temp-table is
		always consistent read. */

		return(true);
	}

	trx_id_t	max_trx_id = page_get_max_trx_id(page_align(rec));

	ut_ad(max_trx_id > 0);

	return(view->sees(max_trx_id));
}

/*********************************************************************//**
Creates the lock system at database start. */
void
lock_sys_create(
/*============*/
	ulint	n_cells)	/*!< in: number of slots in lock hash table */
{
	ulint	lock_sys_sz;

	lock_sys_sz = sizeof(*lock_sys) + OS_THREAD_MAX_N * sizeof(srv_slot_t);

	lock_sys = static_cast<lock_sys_t*>(ut_zalloc_nokey(lock_sys_sz));

	void*	ptr = &lock_sys[1];

	lock_sys->waiting_threads = static_cast<srv_slot_t*>(ptr);

	lock_sys->last_slot = lock_sys->waiting_threads;

	mutex_create(LATCH_ID_LOCK_SYS, &lock_sys->mutex);

	mutex_create(LATCH_ID_LOCK_SYS_WAIT, &lock_sys->wait_mutex);

	lock_sys->timeout_event = os_event_create(0);

	lock_sys->rec_hash = hash_create(n_cells);
	lock_sys->prdt_hash = hash_create(n_cells);
	lock_sys->prdt_page_hash = hash_create(n_cells);

	if (!srv_read_only_mode) {
		lock_latest_err_file = os_file_create_tmpfile(NULL);
		ut_a(lock_latest_err_file);
	}
}

/** Calculates the fold value of a lock: used in migrating the hash table.
@param[in]	lock	record lock object
@return	folded value */
static
ulint
lock_rec_lock_fold(
	const lock_t*	lock)
{
	return(lock_rec_fold(lock->un_member.rec_lock.space,
			     lock->un_member.rec_lock.page_no));
}

/** Resize the lock hash tables.
@param[in]	n_cells	number of slots in lock hash table */
void
lock_sys_resize(
	ulint	n_cells)
{
	hash_table_t*	old_hash;

	lock_mutex_enter();

	old_hash = lock_sys->rec_hash;
	lock_sys->rec_hash = hash_create(n_cells);
	HASH_MIGRATE(old_hash, lock_sys->rec_hash, lock_t, hash,
		     lock_rec_lock_fold);
	hash_table_free(old_hash);

	old_hash = lock_sys->prdt_hash;
	lock_sys->prdt_hash = hash_create(n_cells);
	HASH_MIGRATE(old_hash, lock_sys->prdt_hash, lock_t, hash,
		     lock_rec_lock_fold);
	hash_table_free(old_hash);

	old_hash = lock_sys->prdt_page_hash;
	lock_sys->prdt_page_hash = hash_create(n_cells);
	HASH_MIGRATE(old_hash, lock_sys->prdt_page_hash, lock_t, hash,
		     lock_rec_lock_fold);
	hash_table_free(old_hash);

	/* need to update block->lock_hash_val */
	for (ulint i = 0; i < srv_buf_pool_instances; ++i) {
		buf_pool_t*	buf_pool = buf_pool_from_array(i);

		buf_pool_mutex_enter(buf_pool);
		buf_page_t*	bpage;
		bpage = UT_LIST_GET_FIRST(buf_pool->LRU);

		while (bpage != NULL) {
			if (buf_page_get_state(bpage)
			    == BUF_BLOCK_FILE_PAGE) {
				buf_block_t*	block;
				block = reinterpret_cast<buf_block_t*>(
					bpage);

				block->lock_hash_val
					= lock_rec_hash(
						bpage->id.space(),
						bpage->id.page_no());
			}
			bpage = UT_LIST_GET_NEXT(LRU, bpage);
		}
		buf_pool_mutex_exit(buf_pool);
	}

	lock_mutex_exit();
}

/*********************************************************************//**
Closes the lock system at database shutdown. */
void
lock_sys_close(void)
/*================*/
{
	if (lock_latest_err_file != NULL) {
		fclose(lock_latest_err_file);
		lock_latest_err_file = NULL;
	}

	hash_table_free(lock_sys->rec_hash);
	hash_table_free(lock_sys->prdt_hash);
	hash_table_free(lock_sys->prdt_page_hash);

	os_event_destroy(lock_sys->timeout_event);

	mutex_destroy(&lock_sys->mutex);
	mutex_destroy(&lock_sys->wait_mutex);

	srv_slot_t*	slot = lock_sys->waiting_threads;

	for (ulint i = 0; i < OS_THREAD_MAX_N; i++, ++slot) {
		if (slot->event != NULL) {
			os_event_destroy(slot->event);
		}
	}

	ut_free(lock_sys);

	lock_sys = NULL;
}

/*********************************************************************//**
Gets the size of a lock struct.
@return size in bytes */
ulint
lock_get_size(void)
/*===============*/
{
	return((ulint) sizeof(lock_t));
}

static inline void lock_grant_have_trx_mutex(lock_t* lock)
{
	lock_reset_lock_and_trx_wait(lock);
	lock_grant_after_reset(lock);
}

/*********************************************************************//**
Gets the gap flag of a record lock.
@return LOCK_GAP or 0 */
UNIV_INLINE
ulint
lock_rec_get_gap(
/*=============*/
	const lock_t*	lock)	/*!< in: record lock */
{
	ut_ad(lock);
	ut_ad(lock_get_type_low(lock) == LOCK_REC);

	return(lock->type_mode & LOCK_GAP);
}

/*********************************************************************//**
Gets the LOCK_REC_NOT_GAP flag of a record lock.
@return LOCK_REC_NOT_GAP or 0 */
UNIV_INLINE
ulint
lock_rec_get_rec_not_gap(
/*=====================*/
	const lock_t*	lock)	/*!< in: record lock */
{
	ut_ad(lock);
	ut_ad(lock_get_type_low(lock) == LOCK_REC);

	return(lock->type_mode & LOCK_REC_NOT_GAP);
}

/*********************************************************************//**
Gets the waiting insert flag of a record lock.
@return LOCK_INSERT_INTENTION or 0 */
UNIV_INLINE
ulint
lock_rec_get_insert_intention(
/*==========================*/
	const lock_t*	lock)	/*!< in: record lock */
{
	ut_ad(lock);
	ut_ad(lock_get_type_low(lock) == LOCK_REC);

	return(lock->type_mode & LOCK_INSERT_INTENTION);
}

/*********************************************************************//**
Checks if a lock request for a new lock has to wait for request lock2.
@return TRUE if new lock has to wait for lock2 to be removed */
UNIV_INLINE
ibool
lock_rec_has_to_wait(
/*=================*/
	bool		for_locking,
				/*!< in is called locking or releasing */
	const trx_t*	trx,	/*!< in: trx of new lock */
	ulint		type_mode,/*!< in: precise mode of the new lock
				to set: LOCK_S or LOCK_X, possibly
				ORed to LOCK_GAP or LOCK_REC_NOT_GAP,
				LOCK_INSERT_INTENTION */
	const lock_t*	lock2,	/*!< in: another record lock; NOTE that
				it is assumed that this has a lock bit
				set on the same record as in the new
				lock we are setting */
	bool		lock_is_on_supremum)
				/*!< in: TRUE if we are setting the
				lock on the 'supremum' record of an
				index page: we know then that the lock
				request is really for a 'gap' type lock */
{
	ut_ad(trx && lock2);
	ut_ad(lock_get_type_low(lock2) == LOCK_REC);

	if (trx != lock2->trx
	    && !lock_mode_compatible(static_cast<lock_mode>(
			             LOCK_MODE_MASK & type_mode),
				     lock_get_mode(lock2))) {

		/* We have somewhat complex rules when gap type record locks
		cause waits */

		if ((lock_is_on_supremum || (type_mode & LOCK_GAP))
		    && !(type_mode & LOCK_INSERT_INTENTION)) {

			/* Gap type locks without LOCK_INSERT_INTENTION flag
			do not need to wait for anything. This is because
			different users can have conflicting lock types
			on gaps. */

			return(FALSE);
		}

		if (!(type_mode & LOCK_INSERT_INTENTION)
		    && lock_rec_get_gap(lock2)) {

			/* Record lock (LOCK_ORDINARY or LOCK_REC_NOT_GAP
			does not need to wait for a gap type lock */

			return(FALSE);
		}

		if ((type_mode & LOCK_GAP)
		    && lock_rec_get_rec_not_gap(lock2)) {

			/* Lock on gap does not need to wait for
			a LOCK_REC_NOT_GAP type lock */

			return(FALSE);
		}

		if (lock_rec_get_insert_intention(lock2)) {

			/* No lock request needs to wait for an insert
			intention lock to be removed. This is ok since our
			rules allow conflicting locks on gaps. This eliminates
			a spurious deadlock caused by a next-key lock waiting
			for an insert intention lock; when the insert
			intention lock was granted, the insert deadlocked on
			the waiting next-key lock.

			Also, insert intention locks do not disturb each
			other. */

			return(FALSE);
		}

		if ((type_mode & LOCK_GAP || lock_rec_get_gap(lock2)) &&
		    !thd_need_ordering_with(trx->mysql_thd,
					    lock2->trx->mysql_thd)) {
			/* If the upper server layer has already decided on the
			commit order between the transaction requesting the
			lock and the transaction owning the lock, we do not
			need to wait for gap locks. Such ordeering by the upper
			server layer happens in parallel replication, where the
			commit order is fixed to match the original order on the
			master.

			Such gap locks are mainly needed to get serialisability
			between transactions so that they will be binlogged in
			the correct order so that statement-based replication
			will give the correct results. Since the right order
			was already determined on the master, we do not need
			to enforce it again here.

			Skipping the locks is not essential for correctness,
			since in case of deadlock we will just kill the later
			transaction and retry it. But it can save some
			unnecessary rollbacks and retries. */

			return (FALSE);
		}

#ifdef WITH_WSREP
		/* if BF thread is locking and has conflict with another BF
		   thread, we need to look at trx ordering and lock types */
		if (wsrep_thd_is_BF(trx->mysql_thd, FALSE)         &&
		    wsrep_thd_is_BF(lock2->trx->mysql_thd, TRUE)) {
			mtr_t mtr;

			if (wsrep_debug) {
				ib::info() <<
					"BF-BF lock conflict, locking: " << for_locking;
				lock_rec_print(stderr, lock2, mtr);
				ib::info() << " SQL1: "
					   << wsrep_thd_query(trx->mysql_thd);
				ib::info() << " SQL2: "
					   << wsrep_thd_query(lock2->trx->mysql_thd);
			}

			if (wsrep_trx_order_before(trx->mysql_thd,
						   lock2->trx->mysql_thd) &&
			    (type_mode & LOCK_MODE_MASK) == LOCK_X        &&
			    (lock2->type_mode & LOCK_MODE_MASK) == LOCK_X) {
				if (for_locking || wsrep_debug) {
					/* exclusive lock conflicts are not
					   accepted */
					ib::info() <<
						"BF-BF X lock conflict,"
						"mode: " << type_mode <<
						" supremum: " << lock_is_on_supremum;
					ib::info() <<
						"conflicts states: my "
						   << wsrep_thd_conflict_state(trx->mysql_thd, FALSE)
						   << " locked "
						   << wsrep_thd_conflict_state(lock2->trx->mysql_thd, FALSE);
					lock_rec_print(stderr, lock2, mtr);
					ib::info() << " SQL1: "
						   << wsrep_thd_query(trx->mysql_thd);
					ib::info() << " SQL2: "
						   << wsrep_thd_query(lock2->trx->mysql_thd);

					if (for_locking) {
						return FALSE;
					}
				}
			} else {
				/* if lock2->index->n_uniq <=
				   lock2->index->n_user_defined_cols
				   operation is on uniq index
				*/
				if (wsrep_debug) {
					ib::info() <<
						"BF conflict, modes: "
						   << type_mode << ":" << lock2->type_mode
						   << " idx: " << lock2->index->name()
						   << " table: " << lock2->index->table->name.m_name
						   << " n_uniq: " << lock2->index->n_uniq
						   << " n_user: " << lock2->index->n_user_defined_cols;
					ib::info() << " SQL1: "
						   << wsrep_thd_query(trx->mysql_thd);
					ib::info() << " SQL2: "
						   << wsrep_thd_query(lock2->trx->mysql_thd);
				}
				return FALSE;
			}
		}
#endif /* WITH_WSREP */

		return(TRUE);
	}

	return(FALSE);
}

/*********************************************************************//**
Checks if a lock request lock1 has to wait for request lock2.
@return TRUE if lock1 has to wait for lock2 to be removed */
ibool
lock_has_to_wait(
/*=============*/
	const lock_t*	lock1,	/*!< in: waiting lock */
	const lock_t*	lock2)	/*!< in: another lock; NOTE that it is
				assumed that this has a lock bit set
				on the same record as in lock1 if the
				locks are record locks */
{
	ut_ad(lock1 && lock2);

	if (lock1->trx != lock2->trx
	    && !lock_mode_compatible(lock_get_mode(lock1),
				     lock_get_mode(lock2))) {
		if (lock_get_type_low(lock1) == LOCK_REC) {
			ut_ad(lock_get_type_low(lock2) == LOCK_REC);

			/* If this lock request is for a supremum record
			then the second bit on the lock bitmap is set */

			if (lock1->type_mode
			    & (LOCK_PREDICATE | LOCK_PRDT_PAGE)) {
				return(lock_prdt_has_to_wait(
					lock1->trx, lock1->type_mode,
					lock_get_prdt_from_lock(lock1),
					lock2));
			} else {
				return(lock_rec_has_to_wait(false,
					lock1->trx, lock1->type_mode, lock2,
					lock_rec_get_nth_bit(lock1, true)));
			}
		}

		return(TRUE);
	}

	return(FALSE);
}

/*============== RECORD LOCK BASIC FUNCTIONS ============================*/

/**********************************************************************//**
Looks for a set bit in a record lock bitmap. Returns ULINT_UNDEFINED,
if none found.
@return bit index == heap number of the record, or ULINT_UNDEFINED if
none found */
ulint
lock_rec_find_set_bit(
/*==================*/
	const lock_t*	lock)	/*!< in: record lock with at least one bit set */
{
	for (ulint i = 0; i < lock_rec_get_n_bits(lock); ++i) {

		if (lock_rec_get_nth_bit(lock, i)) {

			return(i);
		}
	}

	return(ULINT_UNDEFINED);
}

/*********************************************************************//**
Determines if there are explicit record locks on a page.
@return an explicit record lock on the page, or NULL if there are none */
lock_t*
lock_rec_expl_exist_on_page(
/*========================*/
	ulint	space,	/*!< in: space id */
	ulint	page_no)/*!< in: page number */
{
	lock_t*	lock;

	lock_mutex_enter();
	/* Only used in ibuf pages, so rec_hash is good enough */
	lock = lock_rec_get_first_on_page_addr(lock_sys->rec_hash,
					       space, page_no);
	lock_mutex_exit();

	return(lock);
}

/*********************************************************************//**
Resets the record lock bitmap to zero. NOTE: does not touch the wait_lock
pointer in the transaction! This function is used in lock object creation
and resetting. */
static
void
lock_rec_bitmap_reset(
/*==================*/
	lock_t*	lock)	/*!< in: record lock */
{
	ulint	n_bytes;

	ut_ad(lock_get_type_low(lock) == LOCK_REC);

	/* Reset to zero the bitmap which resides immediately after the lock
	struct */

	n_bytes = lock_rec_get_n_bits(lock) / 8;

	ut_ad((lock_rec_get_n_bits(lock) % 8) == 0);

	memset(&lock[1], 0, n_bytes);
}

/*********************************************************************//**
Copies a record lock to heap.
@return copy of lock */
static
lock_t*
lock_rec_copy(
/*==========*/
	const lock_t*	lock,	/*!< in: record lock */
	mem_heap_t*	heap)	/*!< in: memory heap */
{
	ulint	size;

	ut_ad(lock_get_type_low(lock) == LOCK_REC);

	size = sizeof(lock_t) + lock_rec_get_n_bits(lock) / 8;

	return(static_cast<lock_t*>(mem_heap_dup(heap, lock, size)));
}

/*********************************************************************//**
Gets the previous record lock set on a record.
@return previous lock on the same record, NULL if none exists */
const lock_t*
lock_rec_get_prev(
/*==============*/
	const lock_t*	in_lock,/*!< in: record lock */
	ulint		heap_no)/*!< in: heap number of the record */
{
	lock_t*		lock;
	ulint		space;
	ulint		page_no;
	lock_t*		found_lock	= NULL;
	hash_table_t*	hash;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_type_low(in_lock) == LOCK_REC);

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;

	hash = lock_hash_get(in_lock->type_mode);

	for (lock = lock_rec_get_first_on_page_addr(hash, space, page_no);
	     /* No op */;
	     lock = lock_rec_get_next_on_page(lock)) {

		ut_ad(lock);

		if (lock == in_lock) {

			return(found_lock);
		}

		if (lock_rec_get_nth_bit(lock, heap_no)) {

			found_lock = lock;
		}
	}
}

/*============= FUNCTIONS FOR ANALYZING RECORD LOCK QUEUE ================*/

/*********************************************************************//**
Checks if a transaction has a GRANTED explicit lock on rec stronger or equal
to precise_mode.
@return lock or NULL */
UNIV_INLINE
lock_t*
lock_rec_has_expl(
/*==============*/
	ulint			precise_mode,/*!< in: LOCK_S or LOCK_X
					possibly ORed to LOCK_GAP or
					LOCK_REC_NOT_GAP, for a
					supremum record we regard this
					always a gap type request */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: transaction */
{
	lock_t*	lock;

	ut_ad(lock_mutex_own());
	ut_ad((precise_mode & LOCK_MODE_MASK) == LOCK_S
	      || (precise_mode & LOCK_MODE_MASK) == LOCK_X);
	ut_ad(!(precise_mode & LOCK_INSERT_INTENTION));

	for (lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (lock->trx == trx
		    && !lock_rec_get_insert_intention(lock)
		    && lock_mode_stronger_or_eq(
			    lock_get_mode(lock),
			    static_cast<lock_mode>(
				    precise_mode & LOCK_MODE_MASK))
		    && !lock_get_wait(lock)
		    && (!lock_rec_get_rec_not_gap(lock)
			|| (precise_mode & LOCK_REC_NOT_GAP)
			|| heap_no == PAGE_HEAP_NO_SUPREMUM)
		    && (!lock_rec_get_gap(lock)
			|| (precise_mode & LOCK_GAP)
			|| heap_no == PAGE_HEAP_NO_SUPREMUM)) {

			return(lock);
		}
	}

	return(NULL);
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Checks if some other transaction has a lock request in the queue.
@return lock or NULL */
static
lock_t*
lock_rec_other_has_expl_req(
/*========================*/
	lock_mode		mode,	/*!< in: LOCK_S or LOCK_X */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	bool			wait,	/*!< in: whether also waiting locks
					are taken into account */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: transaction, or NULL if
					requests by all transactions
					are taken into account */
{

	ut_ad(lock_mutex_own());
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	/* Only GAP lock can be on SUPREMUM, and we are not looking for
	GAP lock */
	if (heap_no == PAGE_HEAP_NO_SUPREMUM) {
		return(NULL);
	}

	for (lock_t* lock = lock_rec_get_first(lock_sys->rec_hash,
						     block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (lock->trx != trx
		    && !lock_rec_get_gap(lock)
		    && (wait || !lock_get_wait(lock))
		    && lock_mode_stronger_or_eq(lock_get_mode(lock), mode)) {

			return(lock);
		}
	}

	return(NULL);
}
#endif /* UNIV_DEBUG */

#ifdef WITH_WSREP
static
void
wsrep_kill_victim(
/*==============*/
	const trx_t * const trx,
	const lock_t *lock)
{
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(lock->trx));

	/* quit for native mysql */
	if (!wsrep_on(trx->mysql_thd)) {
		return;
	}

	my_bool bf_this  = wsrep_thd_is_BF(trx->mysql_thd, FALSE);
	my_bool bf_other = wsrep_thd_is_BF(lock->trx->mysql_thd, TRUE);
	mtr_t mtr;

	if ((bf_this && !bf_other) ||
		(bf_this && bf_other && wsrep_trx_order_before(
			trx->mysql_thd, lock->trx->mysql_thd))) {

		if (lock->trx->lock.que_state == TRX_QUE_LOCK_WAIT) {
			if (wsrep_debug) {
				ib::info() << "WSREP: BF victim waiting\n";
			}
			/* cannot release lock, until our lock
			is in the queue*/
		} else if (lock->trx != trx) {
			if (wsrep_log_conflicts) {
				if (bf_this) {
					ib::info() << "*** Priority TRANSACTION:";
				} else {
					ib::info() << "*** Victim TRANSACTION:";
				}

				wsrep_trx_print_locking(stderr, trx, 3000);

				if (bf_other) {
					ib::info() << "*** Priority TRANSACTION:";
				} else {
					ib::info() << "*** Victim TRANSACTION:";
				}
                                wsrep_trx_print_locking(stderr, lock->trx, 3000);

				ib::info() << "*** WAITING FOR THIS LOCK TO BE GRANTED:";

				if (lock_get_type(lock) == LOCK_REC) {
					lock_rec_print(stderr, lock, mtr);
				} else {
					lock_table_print(stderr, lock);
				}

				ib::info() << " SQL1: "
					   << wsrep_thd_query(trx->mysql_thd);
				ib::info() << " SQL2: "
					   << wsrep_thd_query(lock->trx->mysql_thd);
			}

			wsrep_innobase_kill_one_trx(trx->mysql_thd,
						    trx, lock->trx, TRUE);
		}
	}
}
#endif /* WITH_WSREP */

/*********************************************************************//**
Checks if some other transaction has a conflicting explicit lock request
in the queue, so that we have to wait.
@return lock or NULL */
static
lock_t*
lock_rec_other_has_conflicting(
/*===========================*/
	ulint			mode,	/*!< in: LOCK_S or LOCK_X,
					possibly ORed to LOCK_GAP or
					LOC_REC_NOT_GAP,
					LOCK_INSERT_INTENTION */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of the record */
	const trx_t*		trx)	/*!< in: our transaction */
{
	lock_t*		lock;

	ut_ad(lock_mutex_own());

	bool	is_supremum = (heap_no == PAGE_HEAP_NO_SUPREMUM);

	for (lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (lock_rec_has_to_wait(true, trx, mode, lock, is_supremum)) {
#ifdef WITH_WSREP
			if (wsrep_on_trx(trx)) {
				trx_mutex_enter(lock->trx);
				/* Below function will roll back either trx
				or lock->trx depending on priority of the
				transaction. */
				wsrep_kill_victim(const_cast<trx_t*>(trx), lock);
				trx_mutex_exit(lock->trx);
			}
#endif /* WITH_WSREP */
			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Checks if some transaction has an implicit x-lock on a record in a secondary
index.
@return transaction id of the transaction which has the x-lock, or 0;
NOTE that this function can return false positives but never false
negatives. The caller must confirm all positive results by calling
trx_is_active(). */
static
trx_t*
lock_sec_rec_some_has_impl(
/*=======================*/
	const rec_t*	rec,	/*!< in: user record */
	dict_index_t*	index,	/*!< in: secondary index */
	const offset_t*	offsets)/*!< in: rec_get_offsets(rec, index) */
{
	trx_t*		trx;
	trx_id_t	max_trx_id;
	const page_t*	page = page_align(rec);

	ut_ad(!lock_mutex_own());
	ut_ad(!trx_sys_mutex_own());
	ut_ad(!dict_index_is_clust(index));
	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));

	max_trx_id = page_get_max_trx_id(page);

	/* Some transaction may have an implicit x-lock on the record only
	if the max trx id for the page >= min trx id for the trx list, or
	database recovery is running. */

	if (max_trx_id < trx_rw_min_trx_id() && !recv_recovery_is_on()) {

		trx = 0;

	} else if (!lock_check_trx_id_sanity(max_trx_id, rec, index, offsets)) {

		/* The page is corrupt: try to avoid a crash by returning 0 */
		trx = 0;

	/* In this case it is possible that some transaction has an implicit
	x-lock. We have to look in the clustered index. */

	} else {
		trx = row_vers_impl_x_locked(rec, index, offsets);
	}

	return(trx);
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Checks if some transaction, other than given trx_id, has an explicit
lock on the given rec, in the given precise_mode.
@return	the transaction, whose id is not equal to trx_id, that has an
explicit lock on the given rec, in the given precise_mode or NULL.*/
static
trx_t*
lock_rec_other_trx_holds_expl(
/*==========================*/
	ulint			precise_mode,	/*!< in: LOCK_S or LOCK_X
						possibly ORed to LOCK_GAP or
						LOCK_REC_NOT_GAP. */
	trx_t*			trx,		/*!< in: trx holding implicit
						lock on rec */
	const rec_t*		rec,		/*!< in: user record */
	const buf_block_t*	block)		/*!< in: buffer block
						containing the record */
{
	trx_t* holds = NULL;

	lock_mutex_enter();
	mutex_enter(&trx_sys->mutex);
	trx_mutex_enter(trx);

	ut_ad(!trx_state_eq(trx, TRX_STATE_NOT_STARTED));

	if (!trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY)) {
		const ulint heap_no = page_rec_get_heap_no(rec);
		for (trx_t* t = UT_LIST_GET_FIRST(trx_sys->rw_trx_list);
		     t != NULL;
		     t = UT_LIST_GET_NEXT(trx_list, t)) {

			lock_t* expl_lock = lock_rec_has_expl(
				precise_mode, block, heap_no, t);
			if (expl_lock && expl_lock->trx != trx) {
				/* An explicit lock is held by trx other than
				the trx holding the implicit lock. */
				holds = expl_lock->trx;
				break;
			}
		}
	}

	lock_mutex_exit();
	mutex_exit(&trx_sys->mutex);
	trx_mutex_exit(trx);

	return(holds);
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Return approximate number or record locks (bits set in the bitmap) for
this transaction. Since delete-marked records may be removed, the
record count will not be precise.
The caller must be holding lock_sys->mutex. */
ulint
lock_number_of_rows_locked(
/*=======================*/
	const trx_lock_t*	trx_lock)	/*!< in: transaction locks */
{
	ut_ad(lock_mutex_own());

	return(trx_lock->n_rec_locks);
}

/*********************************************************************//**
Return the number of table locks for a transaction.
The caller must be holding lock_sys->mutex. */
ulint
lock_number_of_tables_locked(
/*=========================*/
	const trx_lock_t*	trx_lock)	/*!< in: transaction locks */
{
	const lock_t*	lock;
	ulint		n_tables = 0;

	ut_ad(lock_mutex_own());

	for (lock = UT_LIST_GET_FIRST(trx_lock->trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		if (lock_get_type_low(lock) == LOCK_TABLE) {
			n_tables++;
		}
	}

	return(n_tables);
}

/*============== RECORD LOCK CREATION AND QUEUE MANAGEMENT =============*/

#ifdef WITH_WSREP
static
void
wsrep_print_wait_locks(
/*===================*/
	lock_t*		c_lock) /* conflicting lock to print */
{
	if (wsrep_debug &&  c_lock->trx->lock.wait_lock != c_lock) {
		mtr_t mtr;
		ib::info() << "WSREP: c_lock != wait lock";
		ib::info() << " SQL: "
			   << wsrep_thd_query(c_lock->trx->mysql_thd);

		if (lock_get_type_low(c_lock) & LOCK_TABLE) {
			lock_table_print(stderr, c_lock);
		} else {
			lock_rec_print(stderr, c_lock, mtr);
		}

		if (lock_get_type_low(c_lock->trx->lock.wait_lock) & LOCK_TABLE) {
			lock_table_print(stderr, c_lock->trx->lock.wait_lock);
		} else {
			lock_rec_print(stderr, c_lock->trx->lock.wait_lock,
				       mtr);
		}
	}
}
#endif /* WITH_WSREP */

/** Create a new record lock and inserts it to the lock queue,
without checking for deadlocks or conflicts.
@param[in]	type_mode	lock mode and wait flag; type will be replaced
				with LOCK_REC
@param[in]	space		tablespace id
@param[in]	page_no		index page number
@param[in]	page		R-tree index page, or NULL
@param[in]	heap_no		record heap number in the index page
@param[in]	index		the index tree
@param[in,out]	trx		transaction
@param[in]	holds_trx_mutex	whether the caller holds trx->mutex
@return created lock */
lock_t*
lock_rec_create_low(
#ifdef WITH_WSREP
	lock_t*		c_lock,	/*!< conflicting lock */
	que_thr_t*	thr,	/*!< thread owning trx */
#endif
	ulint		type_mode,
	ulint		space,
	ulint		page_no,
	const page_t*	page,
	ulint		heap_no,
	dict_index_t*	index,
	trx_t*		trx,
	bool		holds_trx_mutex)
{
	lock_t*		lock;
	ulint		n_bits;
	ulint		n_bytes;

	ut_ad(lock_mutex_own());
	ut_ad(holds_trx_mutex == trx_mutex_own(trx));
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

#ifdef UNIV_DEBUG
	/* Non-locking autocommit read-only transactions should not set
	any locks. See comment in trx_set_rw_mode explaining why this
	conditional check is required in debug code. */
	if (holds_trx_mutex) {
		check_trx_state(trx);
	}
#endif /* UNIV_DEBUG */

	/* If rec is the supremum record, then we reset the gap and
	LOCK_REC_NOT_GAP bits, as all locks on the supremum are
	automatically of the gap type */

	if (UNIV_UNLIKELY(heap_no == PAGE_HEAP_NO_SUPREMUM)) {
		ut_ad(!(type_mode & LOCK_REC_NOT_GAP));
		type_mode = type_mode & ~(LOCK_GAP | LOCK_REC_NOT_GAP);
	}

	if (UNIV_LIKELY(!(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)))) {
		/* Make lock bitmap bigger by a safety margin */
		n_bits = page_dir_get_n_heap(page) + LOCK_PAGE_BITMAP_MARGIN;
		n_bytes = 1 + n_bits / 8;
	} else {
		ut_ad(heap_no == PRDT_HEAPNO);

		/* The lock is always on PAGE_HEAP_NO_INFIMUM (0), so
		we only need 1 bit (which round up to 1 byte) for
		lock bit setting */
		n_bytes = 1;

		if (type_mode & LOCK_PREDICATE) {
			ulint	tmp = UNIV_WORD_SIZE - 1;

			/* We will attach predicate structure after lock.
			Make sure the memory is aligned on 8 bytes,
			the mem_heap_alloc will align it with
			MEM_SPACE_NEEDED anyway. */
			n_bytes = (n_bytes + sizeof(lock_prdt_t) + tmp) & ~tmp;
			ut_ad(n_bytes == sizeof(lock_prdt_t) + UNIV_WORD_SIZE);
		}
	}

	if (trx->lock.rec_cached >= UT_ARR_SIZE(trx->lock.rec_pool)
	    || sizeof *lock + n_bytes > sizeof *trx->lock.rec_pool) {
		lock = static_cast<lock_t*>(
			mem_heap_alloc(trx->lock.lock_heap,
				       sizeof *lock + n_bytes));
	} else {
		lock = &trx->lock.rec_pool[trx->lock.rec_cached++].lock;
	}

	lock->trx = trx;
	lock->type_mode = (type_mode & ~LOCK_TYPE_MASK) | LOCK_REC;
	lock->index = index;
	lock->un_member.rec_lock.space = uint32_t(space);
	lock->un_member.rec_lock.page_no = uint32_t(page_no);

	if (UNIV_LIKELY(!(type_mode & (LOCK_PREDICATE | LOCK_PRDT_PAGE)))) {
		lock->un_member.rec_lock.n_bits = uint32_t(n_bytes * 8);
	} else {
		/* Predicate lock always on INFIMUM (0) */
		lock->un_member.rec_lock.n_bits = 8;
 	}
	lock_rec_bitmap_reset(lock);
	lock_rec_set_nth_bit(lock, heap_no);
	index->table->n_rec_locks++;
	ut_ad(index->table->get_ref_count() > 0 || !index->table->can_be_evicted);

#ifdef WITH_WSREP
	if (c_lock && wsrep_on_trx(trx)
	    && wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {
		lock_t *hash	= (lock_t *)c_lock->hash;
		lock_t *prev	= NULL;

		while (hash && wsrep_thd_is_BF(hash->trx->mysql_thd, TRUE)
		       && wsrep_trx_order_before(hash->trx->mysql_thd,
						 trx->mysql_thd)) {
			prev = hash;
			hash = (lock_t *)hash->hash;
		}
		lock->hash = hash;
		if (prev) {
			prev->hash = lock;
		} else {
			c_lock->hash = lock;
		}
		/*
		 * delayed conflict resolution '...kill_one_trx' was not called,
		 * if victim was waiting for some other lock
		 */
		trx_mutex_enter(c_lock->trx);
		if (c_lock->trx->lock.que_state == TRX_QUE_LOCK_WAIT) {

			c_lock->trx->lock.was_chosen_as_deadlock_victim = TRUE;

			if (wsrep_debug) {
				wsrep_print_wait_locks(c_lock);
			}

			trx->lock.que_state = TRX_QUE_LOCK_WAIT;
			lock_set_lock_and_trx_wait(lock, trx);
			UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);

			trx->lock.wait_thr = thr;
			thr->state = QUE_THR_LOCK_WAIT;

			/* have to release trx mutex for the duration of
			   victim lock release. This will eventually call
			   lock_grant, which wants to grant trx mutex again
			*/
			if (holds_trx_mutex) {
				trx_mutex_exit(trx);
			}
			lock_cancel_waiting_and_release(
				c_lock->trx->lock.wait_lock);

			if (holds_trx_mutex) {
				trx_mutex_enter(trx);
			}

			trx_mutex_exit(c_lock->trx);

			if (wsrep_debug) {
				ib::info() << "WSREP: c_lock canceled "
					   << ib::hex(c_lock->trx->id)
					   << " SQL: "
					   << wsrep_thd_query(
						   c_lock->trx->mysql_thd);
			}

			/* have to bail out here to avoid lock_set_lock... */
			return(lock);
		}
		trx_mutex_exit(c_lock->trx);
	} else
#endif /* WITH_WSREP */
	if (!(type_mode & (LOCK_WAIT | LOCK_PREDICATE | LOCK_PRDT_PAGE))
	    && innodb_lock_schedule_algorithm
	    == INNODB_LOCK_SCHEDULE_ALGORITHM_VATS
	    && !thd_is_replication_slave_thread(trx->mysql_thd)) {
		HASH_PREPEND(lock_t, hash, lock_sys->rec_hash,
			     lock_rec_fold(space, page_no), lock);
	} else {
		HASH_INSERT(lock_t, hash, lock_hash_get(type_mode),
			    lock_rec_fold(space, page_no), lock);
	}

	if (!holds_trx_mutex) {
		trx_mutex_enter(trx);
	}
	ut_ad(trx_mutex_own(trx));
	if (type_mode & LOCK_WAIT) {
		lock_set_lock_and_trx_wait(lock, trx);
	}
	UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);
	if (!holds_trx_mutex) {
		trx_mutex_exit(trx);
	}
	MONITOR_INC(MONITOR_RECLOCK_CREATED);
	MONITOR_INC(MONITOR_NUM_RECLOCK);

	return lock;
}

/*********************************************************************//**
Check if lock1 has higher priority than lock2.
NULL has lowest priority.
If neither of them is wait lock, the first one has higher priority.
If only one of them is a wait lock, it has lower priority.
If either is a high priority transaction, the lock has higher priority.
Otherwise, the one with an older transaction has higher priority.
@returns true if lock1 has higher priority, false otherwise. */
static bool has_higher_priority(lock_t *lock1, lock_t *lock2)
{
	if (lock1 == NULL) {
		return false;
	} else if (lock2 == NULL) {
		return true;
	}
	// Granted locks has higher priority.
	if (!lock_get_wait(lock1)) {
		return true;
	} else if (!lock_get_wait(lock2)) {
		return false;
	}
	return lock1->trx->start_time_micro <= lock2->trx->start_time_micro;
}

/*********************************************************************//**
Insert a lock to the hash list according to the mode (whether it is a wait
lock) and the age of the transaction the it is associated with.
If the lock is not a wait lock, insert it to the head of the hash list.
Otherwise, insert it to the middle of the wait locks according to the age of
the transaciton. */
static
dberr_t
lock_rec_insert_by_trx_age(
	lock_t	*in_lock) /*!< in: lock to be insert */{
	ulint				space;
	ulint				page_no;
	ulint				rec_fold;
	lock_t*				node;
	lock_t*				next;
	hash_table_t*		hash;
	hash_cell_t*		cell;

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;
	rec_fold = lock_rec_fold(space, page_no);
	hash = lock_hash_get(in_lock->type_mode);
	cell = hash_get_nth_cell(hash,
				 hash_calc_hash(rec_fold, hash));

	node = (lock_t *) cell->node;
	// If in_lock is not a wait lock, we insert it to the head of the list.
	if (node == NULL || !lock_get_wait(in_lock) || has_higher_priority(in_lock, node)) {
		cell->node = in_lock;
		in_lock->hash = node;
		if (lock_get_wait(in_lock)) {
			lock_grant_have_trx_mutex(in_lock);
			return DB_SUCCESS_LOCKED_REC;
		}
		return DB_SUCCESS;
	}
	while (node != NULL && has_higher_priority((lock_t *) node->hash,
						   in_lock)) {
		node = (lock_t *) node->hash;
	}
	next = (lock_t *) node->hash;
	node->hash = in_lock;
	in_lock->hash = next;

	if (lock_get_wait(in_lock) && !lock_rec_has_to_wait_in_queue(in_lock)) {
		lock_grant_have_trx_mutex(in_lock);
		if (cell->node != in_lock) {
			// Move it to the front of the queue
			node->hash = in_lock->hash;
			next = (lock_t *) cell->node;
			cell->node = in_lock;
			in_lock->hash = next;
		}
		return DB_SUCCESS_LOCKED_REC;
	}

	return DB_SUCCESS;
}

#ifdef UNIV_DEBUG
static
bool
lock_queue_validate(
	const lock_t	*in_lock) /*!< in: lock whose hash list is to be validated */
{
	ulint				space;
	ulint				page_no;
	ulint				rec_fold;
	hash_table_t*		hash;
	hash_cell_t*		cell;
	lock_t*				next;
	bool				wait_lock = false;

	if (in_lock == NULL) {
		return true;
	}

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;
	rec_fold = lock_rec_fold(space, page_no);
	hash = lock_hash_get(in_lock->type_mode);
	cell = hash_get_nth_cell(hash,
			hash_calc_hash(rec_fold, hash));
	next = (lock_t *) cell->node;
	while (next != NULL) {
		// If this is a granted lock, check that there's no wait lock before it.
		if (!lock_get_wait(next)) {
			ut_ad(!wait_lock);
		} else {
			wait_lock = true;
		}
		next = next->hash;
	}
	return true;
}
#endif /* UNIV_DEBUG */

static
void
lock_rec_insert_to_head(
	lock_t *in_lock,   /*!< in: lock to be insert */
	ulint	rec_fold)  /*!< in: rec_fold of the page */
{
	hash_table_t*		hash;
	hash_cell_t*		cell;
	lock_t*				node;

	if (in_lock == NULL) {
		return;
	}

	hash = lock_hash_get(in_lock->type_mode);
	cell = hash_get_nth_cell(hash,
			hash_calc_hash(rec_fold, hash));
	node = (lock_t *) cell->node;
	if (node != in_lock) {
		cell->node = in_lock;
		in_lock->hash = node;
	}
}

/** Enqueue a waiting request for a lock which cannot be granted immediately.
Check for deadlocks.
@param[in]	type_mode	the requested lock mode (LOCK_S or LOCK_X)
				possibly ORed with LOCK_GAP or
				LOCK_REC_NOT_GAP, ORed with
				LOCK_INSERT_INTENTION if this
				waiting lock request is set
				when performing an insert of
				an index record
@param[in]	block		leaf page in the index
@param[in]	heap_no		record heap number in the block
@param[in]	index		index tree
@param[in,out]	thr		query thread
@param[in]	prdt		minimum bounding box (spatial index)
@retval	DB_LOCK_WAIT		if the waiting lock was enqueued
@retval	DB_DEADLOCK		if this transaction was chosen as the victim
@retval	DB_SUCCESS_LOCKED_REC	if the other transaction was chosen as a victim
				(or it happened to commit) */
dberr_t
lock_rec_enqueue_waiting(
#ifdef WITH_WSREP
	lock_t*			c_lock,	/*!< conflicting lock */
#endif
	ulint			type_mode,
	const buf_block_t*	block,
	ulint			heap_no,
	dict_index_t*		index,
	que_thr_t*		thr,
	lock_prdt_t*		prdt)
{
	ut_ad(lock_mutex_own());
	ut_ad(!srv_read_only_mode);
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

	trx_t* trx = thr_get_trx(thr);

	ut_ad(trx_mutex_own(trx));
	ut_a(!que_thr_stop(thr));

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		break;
	case TRX_DICT_OP_TABLE:
	case TRX_DICT_OP_INDEX:
		ib::error() << "A record lock wait happens in a dictionary"
			" operation. index "
			<< index->name
			<< " of table "
			<< index->table->name
			<< ". " << BUG_REPORT_MSG;
		ut_ad(0);
	}

	/* Enqueue the lock request that will wait to be granted, note that
	we already own the trx mutex. */
	lock_t* lock = lock_rec_create(
#ifdef WITH_WSREP
		c_lock, thr,
#endif
		type_mode | LOCK_WAIT, block, heap_no, index, trx, TRUE);

	if (prdt && type_mode & LOCK_PREDICATE) {
		lock_prdt_set_prdt(lock, prdt);
	}

	if (ut_d(const trx_t* victim =)
	    DeadlockChecker::check_and_resolve(lock, trx)) {
		ut_ad(victim == trx);
		lock_reset_lock_and_trx_wait(lock);
		lock_rec_reset_nth_bit(lock, heap_no);
		return DB_DEADLOCK;
	}

	if (!trx->lock.wait_lock) {
		/* If there was a deadlock but we chose another
		transaction as a victim, it is possible that we
		already have the lock now granted! */
#ifdef WITH_WSREP
		if (wsrep_debug) {
			ib::info() << "WSREP: BF thread got lock granted early, ID " << ib::hex(trx->id)
				   << " query: " << wsrep_thd_query(trx->mysql_thd);
		}
#endif
		return DB_SUCCESS_LOCKED_REC;
	}

	trx->lock.que_state = TRX_QUE_LOCK_WAIT;

	trx->lock.was_chosen_as_deadlock_victim = false;
	trx->lock.wait_started = time(NULL);

	ut_a(que_thr_stop(thr));

	DBUG_LOG("ib_lock", "trx " << ib::hex(trx->id)
		 << " waits for lock in index " << index->name
		 << " of table " << index->table->name);

	MONITOR_INC(MONITOR_LOCKREC_WAIT);

	if (innodb_lock_schedule_algorithm
	    == INNODB_LOCK_SCHEDULE_ALGORITHM_VATS
	    && !prdt
	    && !thd_is_replication_slave_thread(lock->trx->mysql_thd)) {
		HASH_DELETE(lock_t, hash, lock_sys->rec_hash,
			    lock_rec_lock_fold(lock), lock);
		dberr_t res = lock_rec_insert_by_trx_age(lock);
		if (res != DB_SUCCESS) {
			return res;
		}
	}

	return DB_LOCK_WAIT;
}

/*********************************************************************//**
Adds a record lock request in the record queue. The request is normally
added as the last in the queue, but if there are no waiting lock requests
on the record, and the request to be added is not a waiting request, we
can reuse a suitable record lock object already existing on the same page,
just setting the appropriate bit in its bitmap. This is a low-level function
which does NOT check for deadlocks or lock compatibility!
@return lock where the bit was set */
static
void
lock_rec_add_to_queue(
/*==================*/
	ulint			type_mode,/*!< in: lock mode, wait, gap
					etc. flags; type is ignored
					and replaced by LOCK_REC */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of the record */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in/out: transaction */
	bool			caller_owns_trx_mutex)
					/*!< in: TRUE if caller owns the
					transaction mutex */
{
#ifdef UNIV_DEBUG
	ut_ad(lock_mutex_own());
	ut_ad(caller_owns_trx_mutex == trx_mutex_own(trx));
	ut_ad(dict_index_is_clust(index)
	      || dict_index_get_online_status(index) != ONLINE_INDEX_CREATION);
	switch (type_mode & LOCK_MODE_MASK) {
	case LOCK_X:
	case LOCK_S:
		break;
	default:
		ut_error;
	}

	if (!(type_mode & (LOCK_WAIT | LOCK_GAP))) {
		lock_mode	mode = (type_mode & LOCK_MODE_MASK) == LOCK_S
			? LOCK_X
			: LOCK_S;
		const lock_t*	other_lock
			= lock_rec_other_has_expl_req(
				mode, block, false, heap_no, trx);
#ifdef WITH_WSREP
		//ut_a(!other_lock || (wsrep_thd_is_BF(trx->mysql_thd, FALSE) &&
                //                     wsrep_thd_is_BF(other_lock->trx->mysql_thd, TRUE)));
		if (other_lock &&
			wsrep_on(trx->mysql_thd) &&
			!wsrep_thd_is_BF(trx->mysql_thd, FALSE) &&
			!wsrep_thd_is_BF(other_lock->trx->mysql_thd, TRUE)) {

			ib::info() << "WSREP BF lock conflict for my lock:\n BF:" <<
				((wsrep_thd_is_BF(trx->mysql_thd, FALSE)) ? "BF" : "normal") << " exec: " <<
				wsrep_thd_exec_mode(trx->mysql_thd) << " conflict: " <<
				wsrep_thd_conflict_state(trx->mysql_thd, false) << " seqno: " <<
				wsrep_thd_trx_seqno(trx->mysql_thd) << " SQL: " <<
				wsrep_thd_query(trx->mysql_thd);
			trx_t* otrx = other_lock->trx;
			ib::info() << "WSREP other lock:\n BF:" <<
				((wsrep_thd_is_BF(otrx->mysql_thd, FALSE)) ? "BF" : "normal") << " exec: " <<
				wsrep_thd_exec_mode(otrx->mysql_thd) << " conflict: " <<
				wsrep_thd_conflict_state(otrx->mysql_thd, false) << " seqno: " <<
				wsrep_thd_trx_seqno(otrx->mysql_thd) << " SQL: " <<
				wsrep_thd_query(otrx->mysql_thd);
		}
#else
		ut_a(!other_lock);
#endif /* WITH_WSREP */
	}
#endif /* UNIV_DEBUG */

	type_mode |= LOCK_REC;

	/* If rec is the supremum record, then we can reset the gap bit, as
	all locks on the supremum are automatically of the gap type, and we
	try to avoid unnecessary memory consumption of a new record lock
	struct for a gap type lock */

	if (heap_no == PAGE_HEAP_NO_SUPREMUM) {
		ut_ad(!(type_mode & LOCK_REC_NOT_GAP));

		/* There should never be LOCK_REC_NOT_GAP on a supremum
		record, but let us play safe */

		type_mode &= ~(LOCK_GAP | LOCK_REC_NOT_GAP);
	}

	lock_t*		lock;
	lock_t*		first_lock;
	hash_table_t*	hash = lock_hash_get(type_mode);

	/* Look for a waiting lock request on the same record or on a gap */

	for (first_lock = lock = lock_rec_get_first_on_page(hash, block);
	     lock != NULL;
	     lock = lock_rec_get_next_on_page(lock)) {

		if (lock_get_wait(lock)
		    && lock_rec_get_nth_bit(lock, heap_no)) {

			break;
		}
	}

	if (lock == NULL && !(type_mode & LOCK_WAIT)) {

		/* Look for a similar record lock on the same page:
		if one is found and there are no waiting lock requests,
		we can just set the bit */

		lock = lock_rec_find_similar_on_page(
			type_mode, heap_no, first_lock, trx);

		if (lock != NULL) {

			lock_rec_set_nth_bit(lock, heap_no);

			return;
		}
	}

	lock_rec_create(
#ifdef WITH_WSREP
		NULL, NULL,
#endif
		type_mode, block, heap_no, index, trx, caller_owns_trx_mutex);
}

/*********************************************************************//**
This is a fast routine for locking a record in the most common cases:
there are no explicit locks on the page, or there is just one lock, owned
by this transaction, and of the right type_mode. This is a low-level function
which does NOT look at implicit locks! Checks lock compatibility within
explicit locks. This function sets a normal next-key lock, or in the case of
a page supremum record, a gap type lock.
@return whether the locking succeeded */
UNIV_INLINE
lock_rec_req_status
lock_rec_lock_fast(
/*===============*/
	bool			impl,	/*!< in: if TRUE, no lock is set
					if no wait is necessary: we
					assume that the caller will
					set an implicit lock */
	ulint			mode,	/*!< in: lock mode: LOCK_X or
					LOCK_S possibly ORed to either
					LOCK_GAP or LOCK_REC_NOT_GAP */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of record */
	dict_index_t*		index,	/*!< in: index of record */
	que_thr_t*		thr)	/*!< in: query thread */
{
	ut_ad(lock_mutex_own());
	ut_ad(!srv_read_only_mode);
	ut_ad((LOCK_MODE_MASK & mode) != LOCK_S
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));
	ut_ad((LOCK_MODE_MASK & mode) != LOCK_X
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX)
	      || srv_read_only_mode);
	ut_ad((LOCK_MODE_MASK & mode) == LOCK_S
	      || (LOCK_MODE_MASK & mode) == LOCK_X);
	ut_ad(mode - (LOCK_MODE_MASK & mode) == LOCK_GAP
	      || mode - (LOCK_MODE_MASK & mode) == 0
	      || mode - (LOCK_MODE_MASK & mode) == LOCK_REC_NOT_GAP);
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

	DBUG_EXECUTE_IF("innodb_report_deadlock", return(LOCK_REC_FAIL););

	lock_t*	lock = lock_rec_get_first_on_page(lock_sys->rec_hash, block);

	trx_t*	trx = thr_get_trx(thr);

	lock_rec_req_status	status = LOCK_REC_SUCCESS;

	if (lock == NULL) {
		if (!impl) {
			/* Note that we don't own the trx mutex. */
			lock = lock_rec_create(
#ifdef WITH_WSREP
				NULL, NULL,
#endif
				mode, block, heap_no, index, trx, false);
		}

		status = LOCK_REC_SUCCESS_CREATED;
	} else {
		trx_mutex_enter(trx);

		if (lock_rec_get_next_on_page(lock)
		     || lock->trx != trx
		     || lock->type_mode != (mode | LOCK_REC)
		     || lock_rec_get_n_bits(lock) <= heap_no) {

			status = LOCK_REC_FAIL;
		} else if (!impl) {
			/* If the nth bit of the record lock is already set
			then we do not set a new lock bit, otherwise we do
			set */
			if (!lock_rec_get_nth_bit(lock, heap_no)) {
				lock_rec_set_nth_bit(lock, heap_no);
				status = LOCK_REC_SUCCESS_CREATED;
			}
		}

		trx_mutex_exit(trx);
	}

	return(status);
}

/*********************************************************************//**
This is the general, and slower, routine for locking a record. This is a
low-level function which does NOT look at implicit locks! Checks lock
compatibility within explicit locks. This function sets a normal next-key
lock, or in the case of a page supremum record, a gap type lock.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
static
dberr_t
lock_rec_lock_slow(
/*===============*/
	ibool			impl,	/*!< in: if TRUE, no lock is set
					if no wait is necessary: we
					assume that the caller will
					set an implicit lock */
	ulint			mode,	/*!< in: lock mode: LOCK_X or
					LOCK_S possibly ORed to either
					LOCK_GAP or LOCK_REC_NOT_GAP */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of record */
	dict_index_t*		index,	/*!< in: index of record */
	que_thr_t*		thr)	/*!< in: query thread */
{
	ut_ad(lock_mutex_own());
	ut_ad(!srv_read_only_mode);
	ut_ad((LOCK_MODE_MASK & mode) != LOCK_S
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));
	ut_ad((LOCK_MODE_MASK & mode) != LOCK_X
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	ut_ad((LOCK_MODE_MASK & mode) == LOCK_S
	      || (LOCK_MODE_MASK & mode) == LOCK_X);
	ut_ad(mode - (LOCK_MODE_MASK & mode) == LOCK_GAP
	      || mode - (LOCK_MODE_MASK & mode) == 0
	      || mode - (LOCK_MODE_MASK & mode) == LOCK_REC_NOT_GAP);
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

	DBUG_EXECUTE_IF("innodb_report_deadlock", return(DB_DEADLOCK););

	dberr_t	err;
	trx_t*	trx = thr_get_trx(thr);

	trx_mutex_enter(trx);

	if (lock_rec_has_expl(mode, block, heap_no, trx)) {
		/* The trx already has a strong enough lock: do nothing */
		err = DB_SUCCESS;
	} else if (
#ifdef WITH_WSREP
		   lock_t* c_lock =
#endif /* WITH_WSREP */
		   lock_rec_other_has_conflicting(
			   static_cast<enum lock_mode>(mode),
			   block, heap_no, trx)) {
		/* If another transaction has a non-gap conflicting
		request in the queue, as this transaction does not
		have a lock strong enough already granted on the
		record, we have to wait. */
		err = lock_rec_enqueue_waiting(
#ifdef WITH_WSREP
			c_lock,
#endif /* WITH_WSREP */
			mode, block, heap_no, index, thr, NULL);
	} else if (!impl) {
		/* Set the requested lock on the record, note that
		we already own the transaction mutex. */
		lock_rec_add_to_queue(
			LOCK_REC | mode, block, heap_no, index, trx, TRUE);
		err = DB_SUCCESS_LOCKED_REC;
	} else {
		err = DB_SUCCESS;
	}

	trx_mutex_exit(trx);

	return(err);
}

/*********************************************************************//**
Tries to lock the specified record in the mode requested. If not immediately
possible, enqueues a waiting lock request. This is a low-level function
which does NOT look at implicit locks! Checks lock compatibility within
explicit locks. This function sets a normal next-key lock, or in the case
of a page supremum record, a gap type lock.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
static
dberr_t
lock_rec_lock(
/*==========*/
	bool			impl,	/*!< in: if true, no lock is set
					if no wait is necessary: we
					assume that the caller will
					set an implicit lock */
	ulint			mode,	/*!< in: lock mode: LOCK_X or
					LOCK_S possibly ORed to either
					LOCK_GAP or LOCK_REC_NOT_GAP */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no,/*!< in: heap number of record */
	dict_index_t*		index,	/*!< in: index of record */
	que_thr_t*		thr)	/*!< in: query thread */
{
	ut_ad(lock_mutex_own());
	ut_ad(!srv_read_only_mode);
	ut_ad((LOCK_MODE_MASK & mode) != LOCK_S
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));
	ut_ad((LOCK_MODE_MASK & mode) != LOCK_X
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	ut_ad((LOCK_MODE_MASK & mode) == LOCK_S
	      || (LOCK_MODE_MASK & mode) == LOCK_X);
	ut_ad(mode - (LOCK_MODE_MASK & mode) == LOCK_GAP
	      || mode - (LOCK_MODE_MASK & mode) == LOCK_REC_NOT_GAP
	      || mode - (LOCK_MODE_MASK & mode) == 0);
	ut_ad(dict_index_is_clust(index) || !dict_index_is_online_ddl(index));

	/* We try a simplified and faster subroutine for the most
	common cases */
	switch (lock_rec_lock_fast(impl, mode, block, heap_no, index, thr)) {
	case LOCK_REC_SUCCESS:
		return(DB_SUCCESS);
	case LOCK_REC_SUCCESS_CREATED:
		return(DB_SUCCESS_LOCKED_REC);
	case LOCK_REC_FAIL:
		return(lock_rec_lock_slow(impl, mode, block,
					  heap_no, index, thr));
	}

	ut_error;
	return(DB_ERROR);
}

/*********************************************************************//**
Checks if a waiting record lock request still has to wait in a queue.
@return lock that is causing the wait */
static
const lock_t*
lock_rec_has_to_wait_in_queue(
/*==========================*/
	const lock_t*	wait_lock)	/*!< in: waiting record lock */
{
	const lock_t*	lock;
	ulint		space;
	ulint		page_no;
	ulint		heap_no;
	ulint		bit_mask;
	ulint		bit_offset;
	hash_table_t*	hash;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_wait(wait_lock));
	ut_ad(lock_get_type_low(wait_lock) == LOCK_REC);

	space = wait_lock->un_member.rec_lock.space;
	page_no = wait_lock->un_member.rec_lock.page_no;
	heap_no = lock_rec_find_set_bit(wait_lock);

	bit_offset = heap_no / 8;
	bit_mask = static_cast<ulint>(1) << (heap_no % 8);

	hash = lock_hash_get(wait_lock->type_mode);

	for (lock = lock_rec_get_first_on_page_addr(hash, space, page_no);
	     lock != wait_lock;
	     lock = lock_rec_get_next_on_page_const(lock)) {

		const byte*	p = (const byte*) &lock[1];

		if (heap_no < lock_rec_get_n_bits(lock)
		    && (p[bit_offset] & bit_mask)
		    && lock_has_to_wait(wait_lock, lock)) {
#ifdef WITH_WSREP
			if (wsrep_thd_is_BF(wait_lock->trx->mysql_thd, FALSE) &&
			    wsrep_thd_is_BF(lock->trx->mysql_thd, TRUE)) {
				if (wsrep_debug) {
					mtr_t mtr;
					ib::info() << "WSREP: waiting BF trx: " << ib::hex(wait_lock->trx->id)
						   << " query: " << wsrep_thd_query(wait_lock->trx->mysql_thd);
					lock_rec_print(stderr, wait_lock, mtr);
					ib::info() << "WSREP: do not wait another BF trx: " << ib::hex(lock->trx->id)
						   << " query: " << wsrep_thd_query(lock->trx->mysql_thd);
					lock_rec_print(stderr, lock, mtr);
				}
				/* don't wait for another BF lock */
				continue;
			}
#endif /* WITH_WSREP */

			return(lock);
		}
	}

	return(NULL);
}

/** Grant a lock to a waiting lock request and release the waiting transaction
after lock_reset_lock_and_trx_wait() has been called. */
static void lock_grant_after_reset(lock_t* lock)
{
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(lock->trx));

	if (lock_get_mode(lock) == LOCK_AUTO_INC) {
		dict_table_t*	table = lock->un_member.tab_lock.table;

		if (table->autoinc_trx == lock->trx) {
			ib::error() << "Transaction already had an"
				<< " AUTO-INC lock!";
		} else {
			table->autoinc_trx = lock->trx;

			ib_vector_push(lock->trx->autoinc_locks, &lock);
		}
	}

	DBUG_PRINT("ib_lock", ("wait for trx " TRX_ID_FMT " ends",
			       trx_get_id_for_print(lock->trx)));

	/* If we are resolving a deadlock by choosing another transaction
	as a victim, then our original transaction may not be in the
	TRX_QUE_LOCK_WAIT state, and there is no need to end the lock wait
	for it */

	if (lock->trx->lock.que_state == TRX_QUE_LOCK_WAIT) {
		que_thr_t*	thr;

		thr = que_thr_end_lock_wait(lock->trx);

		if (thr != NULL) {
			lock_wait_release_thread_if_suspended(thr);
		}
	}
}

/** Grant a lock to a waiting lock request and release the waiting transaction. */
static void lock_grant(lock_t* lock)
{
	lock_reset_lock_and_trx_wait(lock);
	trx_mutex_enter(lock->trx);
	lock_grant_after_reset(lock);
	trx_mutex_exit(lock->trx);
}

/*************************************************************//**
Cancels a waiting record lock request and releases the waiting transaction
that requested it. NOTE: does NOT check if waiting lock requests behind this
one can now be granted! */
static
void
lock_rec_cancel(
/*============*/
	lock_t*	lock)	/*!< in: waiting record lock request */
{
	que_thr_t*	thr;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_type_low(lock) == LOCK_REC);

	/* Reset the bit (there can be only one set bit) in the lock bitmap */
	lock_rec_reset_nth_bit(lock, lock_rec_find_set_bit(lock));

	/* Reset the wait flag and the back pointer to lock in trx */

	lock_reset_lock_and_trx_wait(lock);

	/* The following function releases the trx from lock wait */

	trx_mutex_enter(lock->trx);

	thr = que_thr_end_lock_wait(lock->trx);

	if (thr != NULL) {
		lock_wait_release_thread_if_suspended(thr);
	}

	trx_mutex_exit(lock->trx);
}

static
void
lock_grant_and_move_on_page(ulint rec_fold, ulint space, ulint page_no)
{
	lock_t*		lock;
	lock_t*		previous = static_cast<lock_t*>(
		hash_get_nth_cell(lock_sys->rec_hash,
				  hash_calc_hash(rec_fold, lock_sys->rec_hash))
		->node);
	if (previous == NULL) {
		return;
	}
	if (previous->un_member.rec_lock.space == space &&
		previous->un_member.rec_lock.page_no == page_no) {
		lock = previous;
	}
	else {
		while (previous->hash &&
				(previous->hash->un_member.rec_lock.space != space ||
				previous->hash->un_member.rec_lock.page_no != page_no)) {
					previous = previous->hash;
		}
		lock = previous->hash;
	}

	ut_ad(previous->hash == lock || previous == lock);
	/* Grant locks if there are no conflicting locks ahead.
	 Move granted locks to the head of the list. */
	while (lock) {
		/* If the lock is a wait lock on this page, and it does not need to wait. */
		if (lock_get_wait(lock)
		    && lock->un_member.rec_lock.space == space
		    && lock->un_member.rec_lock.page_no == page_no
		    && !lock_rec_has_to_wait_in_queue(lock)) {
			lock_grant(lock);

			if (previous != NULL) {
				/* Move the lock to the head of the list. */
				HASH_GET_NEXT(hash, previous) = HASH_GET_NEXT(hash, lock);
				lock_rec_insert_to_head(lock, rec_fold);
			} else {
				/* Already at the head of the list. */
				previous = lock;
			}
			/* Move on to the next lock. */
			lock = static_cast<lock_t *>(HASH_GET_NEXT(hash, previous));
		} else {
			previous = lock;
			lock = static_cast<lock_t *>(HASH_GET_NEXT(hash, lock));
		}
	}
}

/** Remove a record lock request, waiting or granted, from the queue and
grant locks to other transactions in the queue if they now are entitled
to a lock. NOTE: all record locks contained in in_lock are removed.
@param[in,out]	in_lock		record lock */
static void lock_rec_dequeue_from_page(lock_t* in_lock)
{
	ulint		space;
	ulint		page_no;
	hash_table_t*	lock_hash;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_type_low(in_lock) == LOCK_REC);
	/* We may or may not be holding in_lock->trx->mutex here. */

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;

	in_lock->index->table->n_rec_locks--;

	lock_hash = lock_hash_get(in_lock->type_mode);

	ulint rec_fold = lock_rec_fold(space, page_no);

	HASH_DELETE(lock_t, hash, lock_hash, rec_fold, in_lock);
	UT_LIST_REMOVE(in_lock->trx->lock.trx_locks, in_lock);

	MONITOR_INC(MONITOR_RECLOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_RECLOCK);

	if (innodb_lock_schedule_algorithm
	    == INNODB_LOCK_SCHEDULE_ALGORITHM_FCFS
	    || lock_hash != lock_sys->rec_hash
	    || thd_is_replication_slave_thread(in_lock->trx->mysql_thd)) {
		/* Check if waiting locks in the queue can now be granted:
		grant locks if there are no conflicting locks ahead. Stop at
		the first X lock that is waiting or has been granted. */

		for (lock_t* lock = lock_rec_get_first_on_page_addr(
			     lock_hash, space, page_no);
		     lock != NULL;
		     lock = lock_rec_get_next_on_page(lock)) {

			if (lock_get_wait(lock)
			    && !lock_rec_has_to_wait_in_queue(lock)) {
				/* Grant the lock */
				ut_ad(lock->trx != in_lock->trx);
				lock_grant(lock);
			}
		}
	} else {
		lock_grant_and_move_on_page(rec_fold, space, page_no);
	}
}

/*************************************************************//**
Removes a record lock request, waiting or granted, from the queue. */
void
lock_rec_discard(
/*=============*/
	lock_t*		in_lock)	/*!< in: record lock object: all
					record locks which are contained
					in this lock object are removed */
{
	ulint		space;
	ulint		page_no;
	trx_lock_t*	trx_lock;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_type_low(in_lock) == LOCK_REC);

	trx_lock = &in_lock->trx->lock;

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;

	in_lock->index->table->n_rec_locks--;

	HASH_DELETE(lock_t, hash, lock_hash_get(in_lock->type_mode),
			    lock_rec_fold(space, page_no), in_lock);

	UT_LIST_REMOVE(trx_lock->trx_locks, in_lock);

	MONITOR_INC(MONITOR_RECLOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_RECLOCK);
}

/*************************************************************//**
Removes record lock objects set on an index page which is discarded. This
function does not move locks, or check for waiting locks, therefore the
lock bitmaps must already be reset when this function is called. */
static
void
lock_rec_free_all_from_discard_page_low(
/*====================================*/
	ulint		space,
	ulint		page_no,
	hash_table_t*	lock_hash)
{
	lock_t*	lock;
	lock_t*	next_lock;

	lock = lock_rec_get_first_on_page_addr(lock_hash, space, page_no);

	while (lock != NULL) {
		ut_ad(lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
		ut_ad(!lock_get_wait(lock));

		next_lock = lock_rec_get_next_on_page(lock);

		lock_rec_discard(lock);

		lock = next_lock;
	}
}

/*************************************************************//**
Removes record lock objects set on an index page which is discarded. This
function does not move locks, or check for waiting locks, therefore the
lock bitmaps must already be reset when this function is called. */
void
lock_rec_free_all_from_discard_page(
/*================================*/
	const buf_block_t*	block)	/*!< in: page to be discarded */
{
	ulint	space;
	ulint	page_no;

	ut_ad(lock_mutex_own());

	space = block->page.id.space();
	page_no = block->page.id.page_no();

	lock_rec_free_all_from_discard_page_low(
		space, page_no, lock_sys->rec_hash);
	lock_rec_free_all_from_discard_page_low(
		space, page_no, lock_sys->prdt_hash);
	lock_rec_free_all_from_discard_page_low(
		space, page_no, lock_sys->prdt_page_hash);
}

/*============= RECORD LOCK MOVING AND INHERITING ===================*/

/*************************************************************//**
Resets the lock bits for a single record. Releases transactions waiting for
lock requests here. */
static
void
lock_rec_reset_and_release_wait_low(
/*================================*/
	hash_table_t*		hash,	/*!< in: hash table */
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no)/*!< in: heap number of record */
{
	lock_t*	lock;

	ut_ad(lock_mutex_own());

	for (lock = lock_rec_get_first(hash, block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (lock_get_wait(lock)) {
			lock_rec_cancel(lock);
		} else {
			lock_rec_reset_nth_bit(lock, heap_no);
		}
	}
}

/*************************************************************//**
Resets the lock bits for a single record. Releases transactions waiting for
lock requests here. */
static
void
lock_rec_reset_and_release_wait(
/*============================*/
	const buf_block_t*	block,	/*!< in: buffer block containing
					the record */
	ulint			heap_no)/*!< in: heap number of record */
{
	lock_rec_reset_and_release_wait_low(
		lock_sys->rec_hash, block, heap_no);

	lock_rec_reset_and_release_wait_low(
		lock_sys->prdt_hash, block, PAGE_HEAP_NO_INFIMUM);
	lock_rec_reset_and_release_wait_low(
		lock_sys->prdt_page_hash, block, PAGE_HEAP_NO_INFIMUM);
}

/*************************************************************//**
Makes a record to inherit the locks (except LOCK_INSERT_INTENTION type)
of another record as gap type locks, but does not reset the lock bits of
the other record. Also waiting lock requests on rec are inherited as
GRANTED gap locks. */
static
void
lock_rec_inherit_to_gap(
/*====================*/
	const buf_block_t*	heir_block,	/*!< in: block containing the
						record which inherits */
	const buf_block_t*	block,		/*!< in: block containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no)	/*!< in: heap_no of the
						donating record */
{
	lock_t*	lock;

	ut_ad(lock_mutex_own());

	/* If srv_locks_unsafe_for_binlog is TRUE or session is using
	READ COMMITTED isolation level, we do not want locks set
	by an UPDATE or a DELETE to be inherited as gap type locks. But we
	DO want S-locks/X-locks(taken for replace) set by a consistency
	constraint to be inherited also then. */

	for (lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (!lock_rec_get_insert_intention(lock)
		    && !((srv_locks_unsafe_for_binlog
			  || lock->trx->isolation_level
			  <= TRX_ISO_READ_COMMITTED)
			 && lock_get_mode(lock) ==
			 (lock->trx->duplicates ? LOCK_S : LOCK_X))) {
			lock_rec_add_to_queue(
				LOCK_REC | LOCK_GAP | lock_get_mode(lock),
				heir_block, heir_heap_no, lock->index,
				lock->trx, FALSE);
		}
	}
}

/*************************************************************//**
Makes a record to inherit the gap locks (except LOCK_INSERT_INTENTION type)
of another record as gap type locks, but does not reset the lock bits of the
other record. Also waiting lock requests are inherited as GRANTED gap locks. */
static
void
lock_rec_inherit_to_gap_if_gap_lock(
/*================================*/
	const buf_block_t*	block,		/*!< in: buffer block */
	ulint			heir_heap_no,	/*!< in: heap_no of
						record which inherits */
	ulint			heap_no)	/*!< in: heap_no of record
						from which inherited;
						does NOT reset the locks
						on this record */
{
	lock_t*	lock;

	lock_mutex_enter();

	for (lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {

		if (!lock_rec_get_insert_intention(lock)
		    && (heap_no == PAGE_HEAP_NO_SUPREMUM
			|| !lock_rec_get_rec_not_gap(lock))) {

			lock_rec_add_to_queue(
				LOCK_REC | LOCK_GAP | lock_get_mode(lock),
				block, heir_heap_no, lock->index,
				lock->trx, FALSE);
		}
	}

	lock_mutex_exit();
}

/*************************************************************//**
Moves the locks of a record to another record and resets the lock bits of
the donating record. */
static
void
lock_rec_move_low(
/*==============*/
	hash_table_t*		lock_hash,	/*!< in: hash table to use */
	const buf_block_t*	receiver,	/*!< in: buffer block containing
						the receiving record */
	const buf_block_t*	donator,	/*!< in: buffer block containing
						the donating record */
	ulint			receiver_heap_no,/*!< in: heap_no of the record
						which gets the locks; there
						must be no lock requests
						on it! */
	ulint			donator_heap_no)/*!< in: heap_no of the record
						which gives the locks */
{
	lock_t*	lock;

	ut_ad(lock_mutex_own());

	/* If the lock is predicate lock, it resides on INFIMUM record */
	ut_ad(lock_rec_get_first(
		lock_hash, receiver, receiver_heap_no) == NULL
	      || lock_hash == lock_sys->prdt_hash
	      || lock_hash == lock_sys->prdt_page_hash);

	for (lock = lock_rec_get_first(lock_hash,
				       donator, donator_heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next(donator_heap_no, lock)) {

		const ulint	type_mode = lock->type_mode;

		lock_rec_reset_nth_bit(lock, donator_heap_no);

		if (type_mode & LOCK_WAIT) {
			lock_reset_lock_and_trx_wait(lock);
		}

		/* Note that we FIRST reset the bit, and then set the lock:
		the function works also if donator == receiver */

		lock_rec_add_to_queue(
			type_mode, receiver, receiver_heap_no,
			lock->index, lock->trx, FALSE);
	}

	ut_ad(lock_rec_get_first(lock_sys->rec_hash,
				 donator, donator_heap_no) == NULL);
}

/** Move all the granted locks to the front of the given lock list.
All the waiting locks will be at the end of the list.
@param[in,out]	lock_list	the given lock list.  */
static
void
lock_move_granted_locks_to_front(
	UT_LIST_BASE_NODE_T(lock_t)&	lock_list)
{
	lock_t*	lock;

	bool seen_waiting_lock = false;

	for (lock = UT_LIST_GET_FIRST(lock_list); lock;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		if (!seen_waiting_lock) {
			if (lock->is_waiting()) {
				seen_waiting_lock = true;
			}
			continue;
		}

		ut_ad(seen_waiting_lock);

		if (!lock->is_waiting()) {
			lock_t* prev = UT_LIST_GET_PREV(trx_locks, lock);
			ut_a(prev);
			ut_list_move_to_front(lock_list, lock);
			lock = prev;
		}
	}
}

/*************************************************************//**
Moves the locks of a record to another record and resets the lock bits of
the donating record. */
UNIV_INLINE
void
lock_rec_move(
/*==========*/
	const buf_block_t*	receiver,       /*!< in: buffer block containing
						the receiving record */
	const buf_block_t*	donator,        /*!< in: buffer block containing
						the donating record */
	ulint			receiver_heap_no,/*!< in: heap_no of the record
						which gets the locks; there
						must be no lock requests
						on it! */
	ulint			donator_heap_no)/*!< in: heap_no of the record
                                                which gives the locks */
{
	lock_rec_move_low(lock_sys->rec_hash, receiver, donator,
			  receiver_heap_no, donator_heap_no);
}

/*************************************************************//**
Updates the lock table when we have reorganized a page. NOTE: we copy
also the locks set on the infimum of the page; the infimum may carry
locks if an update of a record is occurring on the page, and its locks
were temporarily stored on the infimum. */
void
lock_move_reorganize_page(
/*======================*/
	const buf_block_t*	block,	/*!< in: old index page, now
					reorganized */
	const buf_block_t*	oblock)	/*!< in: copy of the old, not
					reorganized page */
{
	lock_t*		lock;
	UT_LIST_BASE_NODE_T(lock_t)	old_locks;
	mem_heap_t*	heap		= NULL;
	ulint		comp;

	lock_mutex_enter();

	/* FIXME: This needs to deal with predicate lock too */
	lock = lock_rec_get_first_on_page(lock_sys->rec_hash, block);

	if (lock == NULL) {
		lock_mutex_exit();

		return;
	}

	heap = mem_heap_create(256);

	/* Copy first all the locks on the page to heap and reset the
	bitmaps in the original locks; chain the copies of the locks
	using the trx_locks field in them. */

	UT_LIST_INIT(old_locks, &lock_t::trx_locks);

	do {
		/* Make a copy of the lock */
		lock_t*	old_lock = lock_rec_copy(lock, heap);

		UT_LIST_ADD_LAST(old_locks, old_lock);

		/* Reset bitmap of lock */
		lock_rec_bitmap_reset(lock);

		if (lock_get_wait(lock)) {

			lock_reset_lock_and_trx_wait(lock);
		}

		lock = lock_rec_get_next_on_page(lock);
	} while (lock != NULL);

	comp = page_is_comp(block->frame);
	ut_ad(comp == page_is_comp(oblock->frame));

	lock_move_granted_locks_to_front(old_locks);

	DBUG_EXECUTE_IF("do_lock_reverse_page_reorganize",
			ut_list_reverse(old_locks););

	for (lock = UT_LIST_GET_FIRST(old_locks); lock;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

		/* NOTE: we copy also the locks set on the infimum and
		supremum of the page; the infimum may carry locks if an
		update of a record is occurring on the page, and its locks
		were temporarily stored on the infimum */
		const rec_t*	rec1 = page_get_infimum_rec(
			buf_block_get_frame(block));
		const rec_t*	rec2 = page_get_infimum_rec(
			buf_block_get_frame(oblock));

		/* Set locks according to old locks */
		for (;;) {
			ulint	old_heap_no;
			ulint	new_heap_no;

			if (comp) {
				old_heap_no = rec_get_heap_no_new(rec2);
				new_heap_no = rec_get_heap_no_new(rec1);

				rec1 = page_rec_get_next_low(rec1, TRUE);
				rec2 = page_rec_get_next_low(rec2, TRUE);
			} else {
				old_heap_no = rec_get_heap_no_old(rec2);
				new_heap_no = rec_get_heap_no_old(rec1);
				ut_ad(!memcmp(rec1, rec2,
					      rec_get_data_size_old(rec2)));

				rec1 = page_rec_get_next_low(rec1, FALSE);
				rec2 = page_rec_get_next_low(rec2, FALSE);
			}

			/* Clear the bit in old_lock. */
			if (old_heap_no < lock->un_member.rec_lock.n_bits
			    && lock_rec_reset_nth_bit(lock, old_heap_no)) {
				/* NOTE that the old lock bitmap could be too
				small for the new heap number! */

				lock_rec_add_to_queue(
					lock->type_mode, block, new_heap_no,
					lock->index, lock->trx, FALSE);
			}

			if (new_heap_no == PAGE_HEAP_NO_SUPREMUM) {
				ut_ad(old_heap_no == PAGE_HEAP_NO_SUPREMUM);
				break;
			}
		}

		ut_ad(lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
	}

	lock_mutex_exit();

	mem_heap_free(heap);

#ifdef UNIV_DEBUG_LOCK_VALIDATE
	ut_ad(lock_rec_validate_page(block));
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list end is moved to another page. */
void
lock_move_rec_list_end(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec)		/*!< in: record on page: this
						is the first record moved */
{
	lock_t*		lock;
	const ulint	comp	= page_rec_is_comp(rec);

	ut_ad(buf_block_get_frame(block) == page_align(rec));
	ut_ad(comp == page_is_comp(buf_block_get_frame(new_block)));

	lock_mutex_enter();

	/* Note: when we move locks from record to record, waiting locks
	and possible granted gap type locks behind them are enqueued in
	the original order, because new elements are inserted to a hash
	table to the end of the hash chain, and lock_rec_add_to_queue
	does not reuse locks if there are waiters in the queue. */

	for (lock = lock_rec_get_first_on_page(lock_sys->rec_hash, block); lock;
	     lock = lock_rec_get_next_on_page(lock)) {
		const rec_t*	rec1	= rec;
		const rec_t*	rec2;
		const ulint	type_mode = lock->type_mode;

		if (comp) {
			if (page_offset(rec1) == PAGE_NEW_INFIMUM) {
				rec1 = page_rec_get_next_low(rec1, TRUE);
			}

			rec2 = page_rec_get_next_low(
				buf_block_get_frame(new_block)
				+ PAGE_NEW_INFIMUM, TRUE);
		} else {
			if (page_offset(rec1) == PAGE_OLD_INFIMUM) {
				rec1 = page_rec_get_next_low(rec1, FALSE);
			}

			rec2 = page_rec_get_next_low(
				buf_block_get_frame(new_block)
				+ PAGE_OLD_INFIMUM, FALSE);
		}

		/* Copy lock requests on user records to new page and
		reset the lock bits on the old */

		for (;;) {
			ulint	rec1_heap_no;
			ulint	rec2_heap_no;

			if (comp) {
				rec1_heap_no = rec_get_heap_no_new(rec1);

				if (rec1_heap_no == PAGE_HEAP_NO_SUPREMUM) {
					break;
				}

				rec2_heap_no = rec_get_heap_no_new(rec2);
				rec1 = page_rec_get_next_low(rec1, TRUE);
				rec2 = page_rec_get_next_low(rec2, TRUE);
			} else {
				rec1_heap_no = rec_get_heap_no_old(rec1);

				if (rec1_heap_no == PAGE_HEAP_NO_SUPREMUM) {
					break;
				}

				rec2_heap_no = rec_get_heap_no_old(rec2);

				ut_ad(!memcmp(rec1, rec2,
					      rec_get_data_size_old(rec2)));

				rec1 = page_rec_get_next_low(rec1, FALSE);
				rec2 = page_rec_get_next_low(rec2, FALSE);
			}

			if (rec1_heap_no < lock->un_member.rec_lock.n_bits
			    && lock_rec_reset_nth_bit(lock, rec1_heap_no)) {
				if (type_mode & LOCK_WAIT) {
					lock_reset_lock_and_trx_wait(lock);
				}

				lock_rec_add_to_queue(
					type_mode, new_block, rec2_heap_no,
					lock->index, lock->trx, FALSE);
			}
		}
	}

	lock_mutex_exit();

#ifdef UNIV_DEBUG_LOCK_VALIDATE
	ut_ad(lock_rec_validate_page(block));
	ut_ad(lock_rec_validate_page(new_block));
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
void
lock_move_rec_list_start(
/*=====================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	const rec_t*		rec,		/*!< in: record on page:
						this is the first
						record NOT copied */
	const rec_t*		old_end)	/*!< in: old
						previous-to-last
						record on new_page
						before the records
						were copied */
{
	lock_t*		lock;
	const ulint	comp	= page_rec_is_comp(rec);

	ut_ad(block->frame == page_align(rec));
	ut_ad(new_block->frame == page_align(old_end));
	ut_ad(comp == page_rec_is_comp(old_end));

	lock_mutex_enter();

	for (lock = lock_rec_get_first_on_page(lock_sys->rec_hash, block); lock;
	     lock = lock_rec_get_next_on_page(lock)) {
		const rec_t*	rec1;
		const rec_t*	rec2;
		const ulint	type_mode = lock->type_mode;

		if (comp) {
			rec1 = page_rec_get_next_low(
				buf_block_get_frame(block)
				+ PAGE_NEW_INFIMUM, TRUE);
			rec2 = page_rec_get_next_low(old_end, TRUE);
		} else {
			rec1 = page_rec_get_next_low(
				buf_block_get_frame(block)
				+ PAGE_OLD_INFIMUM, FALSE);
			rec2 = page_rec_get_next_low(old_end, FALSE);
		}

		/* Copy lock requests on user records to new page and
		reset the lock bits on the old */

		while (rec1 != rec) {
			ulint	rec1_heap_no;
			ulint	rec2_heap_no;

			if (comp) {
				rec1_heap_no = rec_get_heap_no_new(rec1);
				rec2_heap_no = rec_get_heap_no_new(rec2);

				rec1 = page_rec_get_next_low(rec1, TRUE);
				rec2 = page_rec_get_next_low(rec2, TRUE);
			} else {
				rec1_heap_no = rec_get_heap_no_old(rec1);
				rec2_heap_no = rec_get_heap_no_old(rec2);

				ut_ad(!memcmp(rec1, rec2,
					      rec_get_data_size_old(rec2)));

				rec1 = page_rec_get_next_low(rec1, FALSE);
				rec2 = page_rec_get_next_low(rec2, FALSE);
			}

			if (rec1_heap_no < lock->un_member.rec_lock.n_bits
			    && lock_rec_reset_nth_bit(lock, rec1_heap_no)) {
				if (type_mode & LOCK_WAIT) {
					lock_reset_lock_and_trx_wait(lock);
				}

				lock_rec_add_to_queue(
					type_mode, new_block, rec2_heap_no,
					lock->index, lock->trx, FALSE);
			}
		}

#ifdef UNIV_DEBUG
		if (page_rec_is_supremum(rec)) {
			ulint	i;

			for (i = PAGE_HEAP_NO_USER_LOW;
			     i < lock_rec_get_n_bits(lock); i++) {
				if (lock_rec_get_nth_bit(lock, i)) {
					ib::fatal()
						<< "lock_move_rec_list_start():"
						<< i << " not moved in "
						<<  (void*) lock;
				}
			}
		}
#endif /* UNIV_DEBUG */
	}

	lock_mutex_exit();

#ifdef UNIV_DEBUG_LOCK_VALIDATE
	ut_ad(lock_rec_validate_page(block));
#endif
}

/*************************************************************//**
Moves the explicit locks on user records to another page if a record
list start is moved to another page. */
void
lock_rtr_move_rec_list(
/*===================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						move to */
	const buf_block_t*	block,		/*!< in: index page */
	rtr_rec_move_t*		rec_move,       /*!< in: recording records
						moved */
	ulint			num_move)       /*!< in: num of rec to move */
{
	lock_t*		lock;
	ulint		comp;

	if (!num_move) {
		return;
	}

	comp = page_rec_is_comp(rec_move[0].old_rec);

	ut_ad(block->frame == page_align(rec_move[0].old_rec));
	ut_ad(new_block->frame == page_align(rec_move[0].new_rec));
	ut_ad(comp == page_rec_is_comp(rec_move[0].new_rec));

	lock_mutex_enter();

	for (lock = lock_rec_get_first_on_page(lock_sys->rec_hash, block); lock;
	     lock = lock_rec_get_next_on_page(lock)) {
		ulint		moved = 0;
		const rec_t*	rec1;
		const rec_t*	rec2;
		const ulint	type_mode = lock->type_mode;

		/* Copy lock requests on user records to new page and
		reset the lock bits on the old */

		while (moved < num_move) {
			ulint	rec1_heap_no;
			ulint	rec2_heap_no;

			rec1 = rec_move[moved].old_rec;
			rec2 = rec_move[moved].new_rec;

			if (comp) {
				rec1_heap_no = rec_get_heap_no_new(rec1);
				rec2_heap_no = rec_get_heap_no_new(rec2);

			} else {
				rec1_heap_no = rec_get_heap_no_old(rec1);
				rec2_heap_no = rec_get_heap_no_old(rec2);

				ut_ad(!memcmp(rec1, rec2,
					      rec_get_data_size_old(rec2)));
			}

			if (rec1_heap_no < lock->un_member.rec_lock.n_bits
			    && lock_rec_reset_nth_bit(lock, rec1_heap_no)) {
				if (type_mode & LOCK_WAIT) {
					lock_reset_lock_and_trx_wait(lock);
				}

				lock_rec_add_to_queue(
					type_mode, new_block, rec2_heap_no,
					lock->index, lock->trx, FALSE);

				rec_move[moved].moved = true;
			}

			moved++;
		}
	}

	lock_mutex_exit();

#ifdef UNIV_DEBUG_LOCK_VALIDATE
	ut_ad(lock_rec_validate_page(block));
#endif
}
/*************************************************************//**
Updates the lock table when a page is split to the right. */
void
lock_update_split_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block)	/*!< in: left page */
{
	ulint	heap_no = lock_get_min_heap_no(right_block);

	lock_mutex_enter();

	/* Move the locks on the supremum of the left page to the supremum
	of the right page */

	lock_rec_move(right_block, left_block,
		      PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);

	/* Inherit the locks to the supremum of left page from the successor
	of the infimum on right page */

	lock_rec_inherit_to_gap(left_block, right_block,
				PAGE_HEAP_NO_SUPREMUM, heap_no);

	lock_mutex_exit();
}

/*************************************************************//**
Updates the lock table when a page is merged to the right. */
void
lock_update_merge_right(
/*====================*/
	const buf_block_t*	right_block,	/*!< in: right page to
						which merged */
	const rec_t*		orig_succ,	/*!< in: original
						successor of infimum
						on the right page
						before merge */
	const buf_block_t*	left_block)	/*!< in: merged index
						page which will be
						discarded */
{
	lock_mutex_enter();

	/* Inherit the locks from the supremum of the left page to the
	original successor of infimum on the right page, to which the left
	page was merged */

	lock_rec_inherit_to_gap(right_block, left_block,
				page_rec_get_heap_no(orig_succ),
				PAGE_HEAP_NO_SUPREMUM);

	/* Reset the locks on the supremum of the left page, releasing
	waiting transactions */

	lock_rec_reset_and_release_wait_low(
		lock_sys->rec_hash, left_block, PAGE_HEAP_NO_SUPREMUM);

#ifdef UNIV_DEBUG
	/* there should exist no page lock on the left page,
	otherwise, it will be blocked from merge */
	ulint   space = left_block->page.id.space();
	ulint   page_no = left_block->page.id.page_no();
	ut_ad(lock_rec_get_first_on_page_addr(
                lock_sys->prdt_page_hash, space, page_no) == NULL);
#endif /* UNIV_DEBUG */

	lock_rec_free_all_from_discard_page(left_block);

	lock_mutex_exit();

}

/*************************************************************//**
Updates the lock table when the root page is copied to another in
btr_root_raise_and_insert. Note that we leave lock structs on the
root page, even though they do not make sense on other than leaf
pages: the reason is that in a pessimistic update the infimum record
of the root page will act as a dummy carrier of the locks of the record
to be updated. */
void
lock_update_root_raise(
/*===================*/
	const buf_block_t*	block,	/*!< in: index page to which copied */
	const buf_block_t*	root)	/*!< in: root page */
{
	lock_mutex_enter();

	/* Move the locks on the supremum of the root to the supremum
	of block */

	lock_rec_move(block, root,
		      PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
	lock_mutex_exit();
}

/*************************************************************//**
Updates the lock table when a page is copied to another and the original page
is removed from the chain of leaf pages, except if page is the root! */
void
lock_update_copy_and_discard(
/*=========================*/
	const buf_block_t*	new_block,	/*!< in: index page to
						which copied */
	const buf_block_t*	block)		/*!< in: index page;
						NOT the root! */
{
	lock_mutex_enter();

	/* Move the locks on the supremum of the old page to the supremum
	of new_page */

	lock_rec_move(new_block, block,
		      PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);
	lock_rec_free_all_from_discard_page(block);

	lock_mutex_exit();
}

/*************************************************************//**
Updates the lock table when a page is split to the left. */
void
lock_update_split_left(
/*===================*/
	const buf_block_t*	right_block,	/*!< in: right page */
	const buf_block_t*	left_block)	/*!< in: left page */
{
	ulint	heap_no = lock_get_min_heap_no(right_block);

	lock_mutex_enter();

	/* Inherit the locks to the supremum of the left page from the
	successor of the infimum on the right page */

	lock_rec_inherit_to_gap(left_block, right_block,
				PAGE_HEAP_NO_SUPREMUM, heap_no);

	lock_mutex_exit();
}

/*************************************************************//**
Updates the lock table when a page is merged to the left. */
void
lock_update_merge_left(
/*===================*/
	const buf_block_t*	left_block,	/*!< in: left page to
						which merged */
	const rec_t*		orig_pred,	/*!< in: original predecessor
						of supremum on the left page
						before merge */
	const buf_block_t*	right_block)	/*!< in: merged index page
						which will be discarded */
{
	const rec_t*	left_next_rec;

	ut_ad(left_block->frame == page_align(orig_pred));

	lock_mutex_enter();

	left_next_rec = page_rec_get_next_const(orig_pred);

	if (!page_rec_is_supremum(left_next_rec)) {

		/* Inherit the locks on the supremum of the left page to the
		first record which was moved from the right page */

		lock_rec_inherit_to_gap(left_block, left_block,
					page_rec_get_heap_no(left_next_rec),
					PAGE_HEAP_NO_SUPREMUM);

		/* Reset the locks on the supremum of the left page,
		releasing waiting transactions */

		lock_rec_reset_and_release_wait_low(
			lock_sys->rec_hash, left_block, PAGE_HEAP_NO_SUPREMUM);
	}

	/* Move the locks from the supremum of right page to the supremum
	of the left page */

	lock_rec_move(left_block, right_block,
		      PAGE_HEAP_NO_SUPREMUM, PAGE_HEAP_NO_SUPREMUM);

#ifdef UNIV_DEBUG
	/* there should exist no page lock on the right page,
	otherwise, it will be blocked from merge */
	ulint	space = right_block->page.id.space();
	ulint	page_no = right_block->page.id.page_no();
	lock_t*	lock_test = lock_rec_get_first_on_page_addr(
                lock_sys->prdt_page_hash, space, page_no);
	ut_ad(!lock_test);
#endif /* UNIV_DEBUG */

	lock_rec_free_all_from_discard_page(right_block);

	lock_mutex_exit();
}

/*************************************************************//**
Resets the original locks on heir and replaces them with gap type locks
inherited from rec. */
void
lock_rec_reset_and_inherit_gap_locks(
/*=================================*/
	const buf_block_t*	heir_block,	/*!< in: block containing the
						record which inherits */
	const buf_block_t*	block,		/*!< in: block containing the
						record from which inherited;
						does NOT reset the locks on
						this record */
	ulint			heir_heap_no,	/*!< in: heap_no of the
						inheriting record */
	ulint			heap_no)	/*!< in: heap_no of the
						donating record */
{
	lock_mutex_enter();

	lock_rec_reset_and_release_wait(heir_block, heir_heap_no);

	lock_rec_inherit_to_gap(heir_block, block, heir_heap_no, heap_no);

	lock_mutex_exit();
}

/*************************************************************//**
Updates the lock table when a page is discarded. */
void
lock_update_discard(
/*================*/
	const buf_block_t*	heir_block,	/*!< in: index page
						which will inherit the locks */
	ulint			heir_heap_no,	/*!< in: heap_no of the record
						which will inherit the locks */
	const buf_block_t*	block)		/*!< in: index page
						which will be discarded */
{
	const page_t*	page = block->frame;
	const rec_t*	rec;
	ulint		heap_no;

	lock_mutex_enter();

	if (lock_rec_get_first_on_page(lock_sys->rec_hash, block)) {
		ut_ad(!lock_rec_get_first_on_page(lock_sys->prdt_hash, block));
		ut_ad(!lock_rec_get_first_on_page(lock_sys->prdt_page_hash,
						  block));
		/* Inherit all the locks on the page to the record and
		reset all the locks on the page */

		if (page_is_comp(page)) {
			rec = page + PAGE_NEW_INFIMUM;

			do {
				heap_no = rec_get_heap_no_new(rec);

				lock_rec_inherit_to_gap(heir_block, block,
							heir_heap_no, heap_no);

				lock_rec_reset_and_release_wait(
					block, heap_no);

				rec = page + rec_get_next_offs(rec, TRUE);
			} while (heap_no != PAGE_HEAP_NO_SUPREMUM);
		} else {
			rec = page + PAGE_OLD_INFIMUM;

			do {
				heap_no = rec_get_heap_no_old(rec);

				lock_rec_inherit_to_gap(heir_block, block,
							heir_heap_no, heap_no);

				lock_rec_reset_and_release_wait(
					block, heap_no);

				rec = page + rec_get_next_offs(rec, FALSE);
			} while (heap_no != PAGE_HEAP_NO_SUPREMUM);
		}

		lock_rec_free_all_from_discard_page_low(
			block->page.id.space(), block->page.id.page_no(),
			lock_sys->rec_hash);
	} else {
		lock_rec_free_all_from_discard_page_low(
			block->page.id.space(), block->page.id.page_no(),
			lock_sys->prdt_hash);
		lock_rec_free_all_from_discard_page_low(
			block->page.id.space(), block->page.id.page_no(),
			lock_sys->prdt_page_hash);
	}

	lock_mutex_exit();
}

/*************************************************************//**
Updates the lock table when a new user record is inserted. */
void
lock_update_insert(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: the inserted record */
{
	ulint	receiver_heap_no;
	ulint	donator_heap_no;

	ut_ad(block->frame == page_align(rec));

	/* Inherit the gap-locking locks for rec, in gap mode, from the next
	record */

	if (page_rec_is_comp(rec)) {
		receiver_heap_no = rec_get_heap_no_new(rec);
		donator_heap_no = rec_get_heap_no_new(
			page_rec_get_next_low(rec, TRUE));
	} else {
		receiver_heap_no = rec_get_heap_no_old(rec);
		donator_heap_no = rec_get_heap_no_old(
			page_rec_get_next_low(rec, FALSE));
	}

	lock_rec_inherit_to_gap_if_gap_lock(
		block, receiver_heap_no, donator_heap_no);
}

/*************************************************************//**
Updates the lock table when a record is removed. */
void
lock_update_delete(
/*===============*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: the record to be removed */
{
	const page_t*	page = block->frame;
	ulint		heap_no;
	ulint		next_heap_no;

	ut_ad(page == page_align(rec));

	if (page_is_comp(page)) {
		heap_no = rec_get_heap_no_new(rec);
		next_heap_no = rec_get_heap_no_new(page
						   + rec_get_next_offs(rec,
								       TRUE));
	} else {
		heap_no = rec_get_heap_no_old(rec);
		next_heap_no = rec_get_heap_no_old(page
						   + rec_get_next_offs(rec,
								       FALSE));
	}

	lock_mutex_enter();

	/* Let the next record inherit the locks from rec, in gap mode */

	lock_rec_inherit_to_gap(block, block, next_heap_no, heap_no);

	/* Reset the lock bits on rec and release waiting transactions */

	lock_rec_reset_and_release_wait(block, heap_no);

	lock_mutex_exit();
}

/*********************************************************************//**
Stores on the page infimum record the explicit locks of another record.
This function is used to store the lock state of a record when it is
updated and the size of the record changes in the update. The record
is moved in such an update, perhaps to another page. The infimum record
acts as a dummy carrier record, taking care of lock releases while the
actual record is being moved. */
void
lock_rec_store_on_page_infimum(
/*===========================*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec)	/*!< in: record whose lock state
					is stored on the infimum
					record of the same page; lock
					bits are reset on the
					record */
{
	ulint	heap_no = page_rec_get_heap_no(rec);

	ut_ad(block->frame == page_align(rec));

	lock_mutex_enter();

	lock_rec_move(block, block, PAGE_HEAP_NO_INFIMUM, heap_no);

	lock_mutex_exit();
}

/*********************************************************************//**
Restores the state of explicit lock requests on a single record, where the
state was stored on the infimum of the page. */
void
lock_rec_restore_from_page_infimum(
/*===============================*/
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec,	/*!< in: record whose lock state
					is restored */
	const buf_block_t*	donator)/*!< in: page (rec is not
					necessarily on this page)
					whose infimum stored the lock
					state; lock bits are reset on
					the infimum */
{
	ulint	heap_no = page_rec_get_heap_no(rec);

	lock_mutex_enter();

	lock_rec_move(block, donator, heap_no, PAGE_HEAP_NO_INFIMUM);

	lock_mutex_exit();
}

/*========================= TABLE LOCKS ==============================*/

/** Functor for accessing the embedded node within a table lock. */
struct TableLockGetNode {
	ut_list_node<lock_t>& operator() (lock_t& elem)
	{
		return(elem.un_member.tab_lock.locks);
	}
};

/*********************************************************************//**
Creates a table lock object and adds it as the last in the lock queue
of the table. Does NOT check for deadlocks or lock compatibility.
@return own: new lock object */
UNIV_INLINE
lock_t*
lock_table_create(
/*==============*/
	dict_table_t*	table,	/*!< in/out: database table
				in dictionary cache */
	ulint		type_mode,/*!< in: lock mode possibly ORed with
				LOCK_WAIT */
	trx_t*		trx	/*!< in: trx */
#ifdef WITH_WSREP
	, lock_t*	c_lock = NULL	/*!< in: conflicting lock */
#endif
	)
{
	lock_t*		lock;

	ut_ad(table && trx);
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(trx));

	check_trx_state(trx);

	if ((type_mode & LOCK_MODE_MASK) == LOCK_AUTO_INC) {
		++table->n_waiting_or_granted_auto_inc_locks;
	}

	/* For AUTOINC locking we reuse the lock instance only if
	there is no wait involved else we allocate the waiting lock
	from the transaction lock heap. */
	if (type_mode == LOCK_AUTO_INC) {

		lock = table->autoinc_lock;

		table->autoinc_trx = trx;

		ib_vector_push(trx->autoinc_locks, &lock);

	} else if (trx->lock.table_cached
		   < UT_ARR_SIZE(trx->lock.table_pool)) {
		lock = &trx->lock.table_pool[trx->lock.table_cached++];
	} else {

		lock = static_cast<lock_t*>(
			mem_heap_alloc(trx->lock.lock_heap, sizeof(*lock)));

	}

	lock->type_mode = ib_uint32_t(type_mode | LOCK_TABLE);
	lock->trx = trx;

	lock->un_member.tab_lock.table = table;

	ut_ad(table->get_ref_count() > 0 || !table->can_be_evicted);

	UT_LIST_ADD_LAST(trx->lock.trx_locks, lock);

#ifdef WITH_WSREP
	if (c_lock && wsrep_on_trx(trx)) {
		if (wsrep_thd_is_BF(trx->mysql_thd, FALSE)) {
			ut_list_insert(table->locks, c_lock, lock,
				       TableLockGetNode());
			if (wsrep_debug) {
				ib::info() << "table lock BF conflict for "
					   << ib::hex(c_lock->trx->id)
					   << " SQL: "
					   << wsrep_thd_query(
						   c_lock->trx->mysql_thd);
			}
		} else {
			ut_list_append(table->locks, lock, TableLockGetNode());
		}

		trx_mutex_enter(c_lock->trx);

		if (c_lock->trx->lock.que_state == TRX_QUE_LOCK_WAIT) {
			c_lock->trx->lock.was_chosen_as_deadlock_victim = TRUE;

			if (wsrep_debug) {
				wsrep_print_wait_locks(c_lock);
			}

			/* The lock release will call lock_grant(),
			which would acquire trx->mutex again. */
			trx_mutex_exit(trx);
			lock_cancel_waiting_and_release(
				c_lock->trx->lock.wait_lock);
			trx_mutex_enter(trx);

			if (wsrep_debug) {
				ib::info() << "WSREP: c_lock canceled "
					   << ib::hex(c_lock->trx->id)
					   << " SQL: "
					   << wsrep_thd_query(
						   c_lock->trx->mysql_thd);
			}
		}

		trx_mutex_exit(c_lock->trx);
	} else
#endif /* WITH_WSREP */
	ut_list_append(table->locks, lock, TableLockGetNode());

	if (type_mode & LOCK_WAIT) {

		lock_set_lock_and_trx_wait(lock, trx);
	}

	lock->trx->lock.table_locks.push_back(lock);

	MONITOR_INC(MONITOR_TABLELOCK_CREATED);
	MONITOR_INC(MONITOR_NUM_TABLELOCK);

	return(lock);
}

/*************************************************************//**
Pops autoinc lock requests from the transaction's autoinc_locks. We
handle the case where there are gaps in the array and they need to
be popped off the stack. */
UNIV_INLINE
void
lock_table_pop_autoinc_locks(
/*=========================*/
	trx_t*	trx)	/*!< in/out: transaction that owns the AUTOINC locks */
{
	ut_ad(lock_mutex_own());
	ut_ad(!ib_vector_is_empty(trx->autoinc_locks));

	/* Skip any gaps, gaps are NULL lock entries in the
	trx->autoinc_locks vector. */

	do {
		ib_vector_pop(trx->autoinc_locks);

		if (ib_vector_is_empty(trx->autoinc_locks)) {
			return;
		}

	} while (*(lock_t**) ib_vector_get_last(trx->autoinc_locks) == NULL);
}

/*************************************************************//**
Removes an autoinc lock request from the transaction's autoinc_locks. */
UNIV_INLINE
void
lock_table_remove_autoinc_lock(
/*===========================*/
	lock_t*	lock,	/*!< in: table lock */
	trx_t*	trx)	/*!< in/out: transaction that owns the lock */
{
	lock_t*	autoinc_lock;
	lint	i = ib_vector_size(trx->autoinc_locks) - 1;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_mode(lock) == LOCK_AUTO_INC);
	ut_ad(lock_get_type_low(lock) & LOCK_TABLE);
	ut_ad(!ib_vector_is_empty(trx->autoinc_locks));

	/* With stored functions and procedures the user may drop
	a table within the same "statement". This special case has
	to be handled by deleting only those AUTOINC locks that were
	held by the table being dropped. */

	autoinc_lock = *static_cast<lock_t**>(
		ib_vector_get(trx->autoinc_locks, i));

	/* This is the default fast case. */

	if (autoinc_lock == lock) {
		lock_table_pop_autoinc_locks(trx);
	} else {
		/* The last element should never be NULL */
		ut_a(autoinc_lock != NULL);

		/* Handle freeing the locks from within the stack. */

		while (--i >= 0) {
			autoinc_lock = *static_cast<lock_t**>(
				ib_vector_get(trx->autoinc_locks, i));

			if (autoinc_lock == lock) {
				void*	null_var = NULL;
				ib_vector_set(trx->autoinc_locks, i, &null_var);
				return;
			}
		}

		/* Must find the autoinc lock. */
		ut_error;
	}
}

/*************************************************************//**
Removes a table lock request from the queue and the trx list of locks;
this is a low-level function which does NOT check if waiting requests
can now be granted. */
UNIV_INLINE
void
lock_table_remove_low(
/*==================*/
	lock_t*	lock)	/*!< in/out: table lock */
{
	trx_t*		trx;
	dict_table_t*	table;

	ut_ad(lock_mutex_own());

	trx = lock->trx;
	table = lock->un_member.tab_lock.table;

	/* Remove the table from the transaction's AUTOINC vector, if
	the lock that is being released is an AUTOINC lock. */
	if (lock_get_mode(lock) == LOCK_AUTO_INC) {

		/* The table's AUTOINC lock can get transferred to
		another transaction before we get here. */
		if (table->autoinc_trx == trx) {
			table->autoinc_trx = NULL;
		}

		/* The locks must be freed in the reverse order from
		the one in which they were acquired. This is to avoid
		traversing the AUTOINC lock vector unnecessarily.

		We only store locks that were granted in the
		trx->autoinc_locks vector (see lock_table_create()
		and lock_grant()). Therefore it can be empty and we
		need to check for that. */

		if (!lock_get_wait(lock)
		    && !ib_vector_is_empty(trx->autoinc_locks)) {

			lock_table_remove_autoinc_lock(lock, trx);
		}

		ut_a(table->n_waiting_or_granted_auto_inc_locks > 0);
		table->n_waiting_or_granted_auto_inc_locks--;
	}

	UT_LIST_REMOVE(trx->lock.trx_locks, lock);
	ut_list_remove(table->locks, lock, TableLockGetNode());

	MONITOR_INC(MONITOR_TABLELOCK_REMOVED);
	MONITOR_DEC(MONITOR_NUM_TABLELOCK);
}

/*********************************************************************//**
Enqueues a waiting request for a table lock which cannot be granted
immediately. Checks for deadlocks.
@retval	DB_LOCK_WAIT	if the waiting lock was enqueued
@retval	DB_DEADLOCK	if this transaction was chosen as the victim
@retval	DB_SUCCESS	if the other transaction committed or aborted */
static
dberr_t
lock_table_enqueue_waiting(
/*=======================*/
	ulint		mode,	/*!< in: lock mode this transaction is
				requesting */
	dict_table_t*	table,	/*!< in/out: table */
	que_thr_t*	thr	/*!< in: query thread */
#ifdef WITH_WSREP
	, lock_t*	c_lock	/*!< in: conflicting lock or NULL */
#endif
)
{
	trx_t*		trx;
	lock_t*		lock;

	ut_ad(lock_mutex_own());
	ut_ad(!srv_read_only_mode);

	trx = thr_get_trx(thr);
	ut_ad(trx_mutex_own(trx));
	ut_a(!que_thr_stop(thr));

	switch (trx_get_dict_operation(trx)) {
	case TRX_DICT_OP_NONE:
		break;
	case TRX_DICT_OP_TABLE:
	case TRX_DICT_OP_INDEX:
		ib::error() << "A table lock wait happens in a dictionary"
			" operation. Table " << table->name
			<< ". " << BUG_REPORT_MSG;
		ut_ad(0);
	}

#ifdef WITH_WSREP
	if (trx->lock.was_chosen_as_deadlock_victim && wsrep_on_trx(trx)) {
		return(DB_DEADLOCK);
	}
#endif /* WITH_WSREP */

	/* Enqueue the lock request that will wait to be granted */
	lock = lock_table_create(table, mode | LOCK_WAIT, trx
#ifdef WITH_WSREP
				 , c_lock
#endif
				 );

	const trx_t*	victim_trx =
		DeadlockChecker::check_and_resolve(lock, trx);

	if (victim_trx != 0) {
		ut_ad(victim_trx == trx);

		/* The order here is important, we don't want to
		lose the state of the lock before calling remove. */
		lock_table_remove_low(lock);
		lock_reset_lock_and_trx_wait(lock);

		return(DB_DEADLOCK);

	} else if (trx->lock.wait_lock == NULL) {
		/* Deadlock resolution chose another transaction as a victim,
		and we accidentally got our lock granted! */

		return(DB_SUCCESS);
	}

	trx->lock.que_state = TRX_QUE_LOCK_WAIT;

	trx->lock.wait_started = time(NULL);
	trx->lock.was_chosen_as_deadlock_victim = false;

	ut_a(que_thr_stop(thr));

	MONITOR_INC(MONITOR_TABLELOCK_WAIT);

	return(DB_LOCK_WAIT);
}

/*********************************************************************//**
Checks if other transactions have an incompatible mode lock request in
the lock queue.
@return lock or NULL */
UNIV_INLINE
lock_t*
lock_table_other_has_incompatible(
/*==============================*/
	const trx_t*		trx,	/*!< in: transaction, or NULL if all
					transactions should be included */
	ulint			wait,	/*!< in: LOCK_WAIT if also
					waiting locks are taken into
					account, or 0 if not */
	const dict_table_t*	table,	/*!< in: table */
	lock_mode		mode)	/*!< in: lock mode */
{
	lock_t*	lock;

	ut_ad(lock_mutex_own());

	for (lock = UT_LIST_GET_LAST(table->locks);
	     lock != NULL;
	     lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock)) {

		if (lock->trx != trx
		    && !lock_mode_compatible(lock_get_mode(lock), mode)
		    && (wait || !lock_get_wait(lock))) {

#ifdef WITH_WSREP
			if (wsrep_on(lock->trx->mysql_thd)) {
				if (wsrep_debug) {
					ib::info() << "WSREP: table lock abort for table:"
						   << table->name.m_name;
					ib::info() << " SQL: "
					   << wsrep_thd_query(lock->trx->mysql_thd);
				}
				trx_mutex_enter(lock->trx);
				wsrep_kill_victim((trx_t *)trx, (lock_t *)lock);
				trx_mutex_exit(lock->trx);
			}
#endif /* WITH_WSREP */

			return(lock);
		}
	}

	return(NULL);
}

/*********************************************************************//**
Locks the specified database table in the mode given. If the lock cannot
be granted immediately, the query thread is put to wait.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_table(
/*=======*/
	ulint		flags,	/*!< in: if BTR_NO_LOCKING_FLAG bit is set,
				does nothing */
	dict_table_t*	table,	/*!< in/out: database table
				in dictionary cache */
	lock_mode	mode,	/*!< in: lock mode */
	que_thr_t*	thr)	/*!< in: query thread */
{
	trx_t*		trx;
	dberr_t		err;
	lock_t*		wait_for;

	ut_ad(table && thr);

	/* Given limited visibility of temp-table we can avoid
	locking overhead */
	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || dict_table_is_temporary(table)) {

		return(DB_SUCCESS);
	}

	ut_a(flags == 0);

	trx = thr_get_trx(thr);

	/* Look for equal or stronger locks the same trx already
	has on the table. No need to acquire the lock mutex here
	because only this transacton can add/access table locks
	to/from trx_t::table_locks. */

	if (lock_table_has(trx, table, mode)) {

		return(DB_SUCCESS);
	}

	/* Read only transactions can write to temp tables, we don't want
	to promote them to RW transactions. Their updates cannot be visible
	to other transactions. Therefore we can keep them out
	of the read views. */

	if ((mode == LOCK_IX || mode == LOCK_X)
	    && !trx->read_only
	    && trx->rsegs.m_redo.rseg == 0) {

		trx_set_rw_mode(trx);
	}

	lock_mutex_enter();

	DBUG_EXECUTE_IF("fatal-semaphore-timeout",
		{ os_thread_sleep(3600000000LL); });

	/* We have to check if the new lock is compatible with any locks
	other transactions have in the table lock queue. */

	wait_for = lock_table_other_has_incompatible(
		trx, LOCK_WAIT, table, mode);

	trx_mutex_enter(trx);

	/* Another trx has a request on the table in an incompatible
	mode: this trx may have to wait */

	if (wait_for != NULL) {
		err = lock_table_enqueue_waiting(mode | flags, table, thr
#ifdef WITH_WSREP
						 , wait_for
#endif
						 );
	} else {
		lock_table_create(table, mode | flags, trx);

		ut_a(!flags || mode == LOCK_S || mode == LOCK_X);

		err = DB_SUCCESS;
	}

	lock_mutex_exit();

	trx_mutex_exit(trx);

	return(err);
}

/*********************************************************************//**
Creates a table IX lock object for a resurrected transaction. */
void
lock_table_ix_resurrect(
/*====================*/
	dict_table_t*	table,	/*!< in/out: table */
	trx_t*		trx)	/*!< in/out: transaction */
{
	ut_ad(trx->is_recovered);

	if (lock_table_has(trx, table, LOCK_IX)) {
		return;
	}

	lock_mutex_enter();

	/* We have to check if the new lock is compatible with any locks
	other transactions have in the table lock queue. */

	ut_ad(!lock_table_other_has_incompatible(
		      trx, LOCK_WAIT, table, LOCK_IX));

	trx_mutex_enter(trx);
	lock_table_create(table, LOCK_IX, trx);
	lock_mutex_exit();
	trx_mutex_exit(trx);
}

/*********************************************************************//**
Checks if a waiting table lock request still has to wait in a queue.
@return TRUE if still has to wait */
static
bool
lock_table_has_to_wait_in_queue(
/*============================*/
	const lock_t*	wait_lock)	/*!< in: waiting table lock */
{
	const dict_table_t*	table;
	const lock_t*		lock;

	ut_ad(lock_mutex_own());
	ut_ad(lock_get_wait(wait_lock));

	table = wait_lock->un_member.tab_lock.table;

	for (lock = UT_LIST_GET_FIRST(table->locks);
	     lock != wait_lock;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {

		if (lock_has_to_wait(wait_lock, lock)) {

			return(true);
		}
	}

	return(false);
}

/*************************************************************//**
Removes a table lock request, waiting or granted, from the queue and grants
locks to other transactions in the queue, if they now are entitled to a
lock. */
static
void
lock_table_dequeue(
/*===============*/
	lock_t*	in_lock)/*!< in/out: table lock object; transactions waiting
			behind will get their lock requests granted, if
			they are now qualified to it */
{
	ut_ad(lock_mutex_own());
	ut_a(lock_get_type_low(in_lock) == LOCK_TABLE);

	lock_t*	lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, in_lock);

	lock_table_remove_low(in_lock);

	/* Check if waiting locks in the queue can now be granted: grant
	locks if there are no conflicting locks ahead. */

	for (/* No op */;
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {

		if (lock_get_wait(lock)
		    && !lock_table_has_to_wait_in_queue(lock)) {

			/* Grant the lock */
			ut_ad(in_lock->trx != lock->trx);
			lock_grant(lock);
		}
	}
}

/** Sets a lock on a table based on the given mode.
@param[in]	table	table to lock
@param[in,out]	trx	transaction
@param[in]	mode	LOCK_X or LOCK_S
@return error code or DB_SUCCESS. */
dberr_t
lock_table_for_trx(
	dict_table_t*	table,
	trx_t*		trx,
	enum lock_mode	mode)
{
	mem_heap_t*	heap;
	que_thr_t*	thr;
	dberr_t		err;
	sel_node_t*	node;
	heap = mem_heap_create(512);

	node = sel_node_create(heap);
	thr = pars_complete_graph_for_exec(node, trx, heap, NULL);
	thr->graph->state = QUE_FORK_ACTIVE;

	/* We use the select query graph as the dummy graph needed
	in the lock module call */

	thr = static_cast<que_thr_t*>(
		que_fork_get_first_thr(
			static_cast<que_fork_t*>(que_node_get_parent(thr))));

	que_thr_move_to_run_state_for_mysql(thr, trx);

run_again:
	thr->run_node = thr;
	thr->prev_node = thr->common.parent;

	err = lock_table(0, table, mode, thr);

	trx->error_state = err;

	if (UNIV_LIKELY(err == DB_SUCCESS)) {
		que_thr_stop_for_mysql_no_error(thr, trx);
	} else {
		que_thr_stop_for_mysql(thr);

		if (row_mysql_handle_errors(&err, trx, thr, NULL)) {
			goto run_again;
		}
	}

	que_graph_free(thr->graph);
	trx->op_info = "";

	return(err);
}

/*=========================== LOCK RELEASE ==============================*/
static
void
lock_grant_and_move_on_rec(
	hash_table_t*	lock_hash,
	lock_t*			first_lock,
	ulint			heap_no)
{
	lock_t*		lock;
	lock_t*		previous;
	ulint		space;
	ulint		page_no;
	ulint		rec_fold;

	space = first_lock->un_member.rec_lock.space;
	page_no = first_lock->un_member.rec_lock.page_no;
	rec_fold = lock_rec_fold(space, page_no);

	previous = (lock_t *) hash_get_nth_cell(lock_hash,
							hash_calc_hash(rec_fold, lock_hash))->node;
	if (previous == NULL) {
		return;
	}
	if (previous == first_lock) {
		lock = previous;
	} else {
		while (previous->hash &&
				previous->hash != first_lock) {
			previous = previous->hash;
	    }
		lock = previous->hash;
	}
	/* Grant locks if there are no conflicting locks ahead.
	 Move granted locks to the head of the list. */
	for (;lock != NULL;) {

		/* If the lock is a wait lock on this page, and it does not need to wait. */
		if (lock->un_member.rec_lock.space == space
			&& lock->un_member.rec_lock.page_no == page_no
			&& lock_rec_get_nth_bit(lock, heap_no)
			&& lock_get_wait(lock)
			&& !lock_rec_has_to_wait_in_queue(lock)) {

			lock_grant(lock);

			if (previous != NULL) {
				/* Move the lock to the head of the list. */
				HASH_GET_NEXT(hash, previous) = HASH_GET_NEXT(hash, lock);
				lock_rec_insert_to_head(lock, rec_fold);
			} else {
				/* Already at the head of the list. */
				previous = lock;
			}
			/* Move on to the next lock. */
			lock = static_cast<lock_t *>(HASH_GET_NEXT(hash, previous));
		} else {
			previous = lock;
			lock = static_cast<lock_t *>(HASH_GET_NEXT(hash, lock));
		}
	}
}

/*************************************************************//**
Removes a granted record lock of a transaction from the queue and grants
locks to other transactions waiting in the queue if they now are entitled
to a lock. */
void
lock_rec_unlock(
/*============*/
	trx_t*			trx,	/*!< in/out: transaction that has
					set a record lock */
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec,	/*!< in: record */
	lock_mode		lock_mode)/*!< in: LOCK_S or LOCK_X */
{
	lock_t*		first_lock;
	lock_t*		lock;
	ulint		heap_no;

	ut_ad(trx);
	ut_ad(rec);
	ut_ad(block->frame == page_align(rec));
	ut_ad(!trx->lock.wait_lock);
	ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));

	heap_no = page_rec_get_heap_no(rec);

	lock_mutex_enter();
	trx_mutex_enter(trx);

	first_lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);

	/* Find the last lock with the same lock_mode and transaction
	on the record. */

	for (lock = first_lock; lock != NULL;
	     lock = lock_rec_get_next(heap_no, lock)) {
		if (lock->trx == trx && lock_get_mode(lock) == lock_mode) {
			goto released;
		}
	}

	lock_mutex_exit();
	trx_mutex_exit(trx);

	{
		ib::error	err;
		err << "Unlock row could not find a " << lock_mode
			<< " mode lock on the record. Current statement: ";
		size_t		stmt_len;
		if (const char* stmt = innobase_get_stmt_unsafe(
			    trx->mysql_thd, &stmt_len)) {
			err.write(stmt, stmt_len);
		}
	}

	return;

released:
	ut_a(!lock_get_wait(lock));
	lock_rec_reset_nth_bit(lock, heap_no);

	if (innodb_lock_schedule_algorithm
		== INNODB_LOCK_SCHEDULE_ALGORITHM_FCFS ||
		thd_is_replication_slave_thread(lock->trx->mysql_thd)) {

		/* Check if we can now grant waiting lock requests */

		for (lock = first_lock; lock != NULL;
			 lock = lock_rec_get_next(heap_no, lock)) {
			if (lock_get_wait(lock)
				&& !lock_rec_has_to_wait_in_queue(lock)) {

				/* Grant the lock */
				ut_ad(trx != lock->trx);
				lock_grant(lock);
			}
		}
	} else {
		lock_grant_and_move_on_rec(lock_sys->rec_hash, first_lock, heap_no);
	}

	lock_mutex_exit();
	trx_mutex_exit(trx);
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Check if a transaction that has X or IX locks has set the dict_op
code correctly. */
static
void
lock_check_dict_lock(
/*==================*/
	const lock_t*	lock)	/*!< in: lock to check */
{
	if (lock_get_type_low(lock) == LOCK_REC) {

		/* Check if the transcation locked a record
		in a system table in X mode. It should have set
		the dict_op code correctly if it did. */
		if (lock->index->table->id < DICT_HDR_FIRST_ID
		    && lock_get_mode(lock) == LOCK_X) {

			ut_ad(lock_get_mode(lock) != LOCK_IX);
			ut_ad(lock->trx->dict_operation != TRX_DICT_OP_NONE);
		}
	} else {
		ut_ad(lock_get_type_low(lock) & LOCK_TABLE);

		const dict_table_t*	table;

		table = lock->un_member.tab_lock.table;

		/* Check if the transcation locked a system table
		in IX mode. It should have set the dict_op code
		correctly if it did. */
		if (table->id < DICT_HDR_FIRST_ID
		    && (lock_get_mode(lock) == LOCK_X
			|| lock_get_mode(lock) == LOCK_IX)) {

			ut_ad(lock->trx->dict_operation != TRX_DICT_OP_NONE);
		}
	}
}
#endif /* UNIV_DEBUG */

/*********************************************************************//**
Releases transaction locks, and releases possible other transactions waiting
because of these locks. */
static
void
lock_release(
/*=========*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	lock_t*		lock;
	ulint		count = 0;
	trx_id_t	max_trx_id = trx_sys_get_max_trx_id();

	ut_ad(lock_mutex_own());
	ut_ad(!trx_mutex_own(trx));

	for (lock = UT_LIST_GET_LAST(trx->lock.trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_LAST(trx->lock.trx_locks)) {

		ut_d(lock_check_dict_lock(lock));

		if (lock_get_type_low(lock) == LOCK_REC) {

			lock_rec_dequeue_from_page(lock);
		} else {
			dict_table_t*	table;

			table = lock->un_member.tab_lock.table;

			if (lock_get_mode(lock) != LOCK_IS
			    && trx->undo_no != 0) {

				/* The trx may have modified the table. We
				block the use of the MySQL query cache for
				all currently active transactions. */

				table->query_cache_inv_trx_id = max_trx_id;
			}

			lock_table_dequeue(lock);
		}

		if (count == LOCK_RELEASE_INTERVAL) {
			/* Release the  mutex for a while, so that we
			do not monopolize it */

			lock_mutex_exit();

			lock_mutex_enter();

			count = 0;
		}

		++count;
	}
}

/* True if a lock mode is S or X */
#define IS_LOCK_S_OR_X(lock) \
	(lock_get_mode(lock) == LOCK_S \
	 || lock_get_mode(lock) == LOCK_X)

/*********************************************************************//**
Removes table locks of the transaction on a table to be dropped. */
static
void
lock_trx_table_locks_remove(
/*========================*/
	const lock_t*	lock_to_remove)		/*!< in: lock to remove */
{
	trx_t*		trx = lock_to_remove->trx;

	ut_ad(lock_mutex_own());

	/* It is safe to read this because we are holding the lock mutex */
	if (!trx->lock.cancel) {
		trx_mutex_enter(trx);
	} else {
		ut_ad(trx_mutex_own(trx));
	}

	for (lock_list::iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {
		const lock_t*	lock = *it;

		ut_ad(!lock || trx == lock->trx);
		ut_ad(!lock || lock_get_type_low(lock) & LOCK_TABLE);
		ut_ad(!lock || lock->un_member.tab_lock.table);

		if (lock == lock_to_remove) {
			*it = NULL;

			if (!trx->lock.cancel) {
				trx_mutex_exit(trx);
			}

			return;
		}
	}

	if (!trx->lock.cancel) {
		trx_mutex_exit(trx);
	}

	/* Lock must exist in the vector. */
	ut_error;
}

/*********************************************************************//**
Removes locks of a transaction on a table to be dropped.
If remove_also_table_sx_locks is TRUE then table-level S and X locks are
also removed in addition to other table-level and record-level locks.
No lock that is going to be removed is allowed to be a wait lock. */
static
void
lock_remove_all_on_table_for_trx(
/*=============================*/
	dict_table_t*	table,			/*!< in: table to be dropped */
	trx_t*		trx,			/*!< in: a transaction */
	ibool		remove_also_table_sx_locks)/*!< in: also removes
						table S and X locks */
{
	lock_t*		lock;
	lock_t*		prev_lock;

	ut_ad(lock_mutex_own());

	for (lock = UT_LIST_GET_LAST(trx->lock.trx_locks);
	     lock != NULL;
	     lock = prev_lock) {

		prev_lock = UT_LIST_GET_PREV(trx_locks, lock);

		if (lock_get_type_low(lock) == LOCK_REC
		    && lock->index->table == table) {
			ut_a(!lock_get_wait(lock));

			lock_rec_discard(lock);
		} else if (lock_get_type_low(lock) & LOCK_TABLE
			   && lock->un_member.tab_lock.table == table
			   && (remove_also_table_sx_locks
			       || !IS_LOCK_S_OR_X(lock))) {

			ut_a(!lock_get_wait(lock));

			lock_trx_table_locks_remove(lock);
			lock_table_remove_low(lock);
		}
	}
}

/*******************************************************************//**
Remove any explicit record locks held by recovering transactions on
the table.
@return number of recovered transactions examined */
static
ulint
lock_remove_recovered_trx_record_locks(
/*===================================*/
	dict_table_t*	table)	/*!< in: check if there are any locks
				held on records in this table or on the
				table itself */
{
	ut_a(table != NULL);
	ut_ad(lock_mutex_own());

	ulint		n_recovered_trx = 0;

	mutex_enter(&trx_sys->mutex);

	for (trx_t* trx = UT_LIST_GET_FIRST(trx_sys->rw_trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_NEXT(trx_list, trx)) {

		assert_trx_in_rw_list(trx);

		if (!trx->is_recovered) {
			continue;
		}

		/* Because we are holding the lock_sys->mutex,
		implicit locks cannot be converted to explicit ones
		while we are scanning the explicit locks. */

		lock_t*	next_lock;

		for (lock_t* lock = UT_LIST_GET_FIRST(trx->lock.trx_locks);
		     lock != NULL;
		     lock = next_lock) {

			ut_a(lock->trx == trx);

			/* Recovered transactions can't wait on a lock. */

			ut_a(!lock_get_wait(lock));

			next_lock = UT_LIST_GET_NEXT(trx_locks, lock);

			switch (lock_get_type_low(lock)) {
			default:
				ut_error;
			case LOCK_TABLE:
				if (lock->un_member.tab_lock.table == table) {
					lock_trx_table_locks_remove(lock);
					lock_table_remove_low(lock);
				}
				break;
			case LOCK_REC:
				if (lock->index->table == table) {
					lock_rec_discard(lock);
				}
			}
		}

		++n_recovered_trx;
	}

	mutex_exit(&trx_sys->mutex);

	return(n_recovered_trx);
}

/*********************************************************************//**
Removes locks on a table to be dropped or discarded.
If remove_also_table_sx_locks is TRUE then table-level S and X locks are
also removed in addition to other table-level and record-level locks.
No lock, that is going to be removed, is allowed to be a wait lock. */
void
lock_remove_all_on_table(
/*=====================*/
	dict_table_t*	table,			/*!< in: table to be dropped
						or discarded */
	ibool		remove_also_table_sx_locks)/*!< in: also removes
						table S and X locks */
{
	lock_t*		lock;

	lock_mutex_enter();

	for (lock = UT_LIST_GET_FIRST(table->locks);
	     lock != NULL;
	     /* No op */) {

		lock_t*	prev_lock;

		prev_lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock);

		/* If we should remove all locks (remove_also_table_sx_locks
		is TRUE), or if the lock is not table-level S or X lock,
		then check we are not going to remove a wait lock. */
		if (remove_also_table_sx_locks
		    || !(lock_get_type(lock) == LOCK_TABLE
			 && IS_LOCK_S_OR_X(lock))) {

			ut_a(!lock_get_wait(lock));
		}

		lock_remove_all_on_table_for_trx(
			table, lock->trx, remove_also_table_sx_locks);

		if (prev_lock == NULL) {
			if (lock == UT_LIST_GET_FIRST(table->locks)) {
				/* lock was not removed, pick its successor */
				lock = UT_LIST_GET_NEXT(
					un_member.tab_lock.locks, lock);
			} else {
				/* lock was removed, pick the first one */
				lock = UT_LIST_GET_FIRST(table->locks);
			}
		} else if (UT_LIST_GET_NEXT(un_member.tab_lock.locks,
					    prev_lock) != lock) {
			/* If lock was removed by
			lock_remove_all_on_table_for_trx() then pick the
			successor of prev_lock ... */
			lock = UT_LIST_GET_NEXT(
				un_member.tab_lock.locks, prev_lock);
		} else {
			/* ... otherwise pick the successor of lock. */
			lock = UT_LIST_GET_NEXT(
				un_member.tab_lock.locks, lock);
		}
	}

	/* Note: Recovered transactions don't have table level IX or IS locks
	but can have implicit record locks that have been converted to explicit
	record locks. Such record locks cannot be freed by traversing the
	transaction lock list in dict_table_t (as above). */

	if (!lock_sys->rollback_complete
	    && lock_remove_recovered_trx_record_locks(table) == 0) {

		lock_sys->rollback_complete = TRUE;
	}

	lock_mutex_exit();
}

/*===================== VALIDATION AND DEBUGGING ====================*/

/** Pretty-print a table lock.
@param[in,out]	file	output stream
@param[in]	lock	table lock */
static void lock_table_print(FILE* file, const lock_t* lock)
{
	ut_ad(lock_mutex_own());
	ut_a(lock_get_type_low(lock) == LOCK_TABLE);

	fputs("TABLE LOCK table ", file);
	ut_print_name(file, lock->trx,
		      lock->un_member.tab_lock.table->name.m_name);
	fprintf(file, " trx id " TRX_ID_FMT, trx_get_id_for_print(lock->trx));

	if (lock_get_mode(lock) == LOCK_S) {
		fputs(" lock mode S", file);
	} else if (lock_get_mode(lock) == LOCK_X) {
		ut_ad(lock->trx->id != 0);
		fputs(" lock mode X", file);
	} else if (lock_get_mode(lock) == LOCK_IS) {
		fputs(" lock mode IS", file);
	} else if (lock_get_mode(lock) == LOCK_IX) {
		ut_ad(lock->trx->id != 0);
		fputs(" lock mode IX", file);
	} else if (lock_get_mode(lock) == LOCK_AUTO_INC) {
		fputs(" lock mode AUTO-INC", file);
	} else {
		fprintf(file, " unknown lock mode %lu",
			(ulong) lock_get_mode(lock));
	}

	if (lock_get_wait(lock)) {
		fputs(" waiting", file);
	}

	putc('\n', file);
}

/** Pretty-print a record lock.
@param[in,out]	file	output stream
@param[in]	lock	record lock
@param[in,out]	mtr	mini-transaction for accessing the record */
static void lock_rec_print(FILE* file, const lock_t* lock, mtr_t& mtr)
{
	ulint			space;
	ulint			page_no;

	ut_ad(lock_mutex_own());
	ut_a(lock_get_type_low(lock) == LOCK_REC);

	space = lock->un_member.rec_lock.space;
	page_no = lock->un_member.rec_lock.page_no;

	fprintf(file, "RECORD LOCKS space id %lu page no %lu n bits %lu "
		"index %s of table ",
		(ulong) space, (ulong) page_no,
		(ulong) lock_rec_get_n_bits(lock),
		lock->index->name());
	ut_print_name(file, lock->trx, lock->index->table_name);
	fprintf(file, " trx id " TRX_ID_FMT, trx_get_id_for_print(lock->trx));

	if (lock_get_mode(lock) == LOCK_S) {
		fputs(" lock mode S", file);
	} else if (lock_get_mode(lock) == LOCK_X) {
		fputs(" lock_mode X", file);
	} else {
		ut_error;
	}

	if (lock_rec_get_gap(lock)) {
		fputs(" locks gap before rec", file);
	}

	if (lock_rec_get_rec_not_gap(lock)) {
		fputs(" locks rec but not gap", file);
	}

	if (lock_rec_get_insert_intention(lock)) {
		fputs(" insert intention", file);
	}

	if (lock_get_wait(lock)) {
		fputs(" waiting", file);
	}

	putc('\n', file);

	mem_heap_t*		heap		= NULL;
	offset_t		offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*		offsets		= offsets_;
	rec_offs_init(offsets_);

	mtr.start();
	const buf_block_t* block = buf_page_try_get(page_id_t(space, page_no),
						    &mtr);

	for (ulint i = 0; i < lock_rec_get_n_bits(lock); ++i) {

		if (!lock_rec_get_nth_bit(lock, i)) {
			continue;
		}

		fprintf(file, "Record lock, heap no %lu", (ulong) i);

		if (block) {
			ut_ad(page_is_leaf(block->frame));
			const rec_t*	rec;

			rec = page_find_rec_with_heap_no(
				buf_block_get_frame(block), i);

			offsets = rec_get_offsets(
				rec, lock->index, offsets, true,
				ULINT_UNDEFINED, &heap);

			putc(' ', file);
			rec_print_new(file, rec, offsets);
		}

		putc('\n', file);
	}

	mtr.commit();

	if (UNIV_LIKELY_NULL(heap)) {
		mem_heap_free(heap);
	}
}

#ifdef UNIV_DEBUG
/* Print the number of lock structs from lock_print_info_summary() only
in non-production builds for performance reasons, see
http://bugs.mysql.com/36942 */
#define PRINT_NUM_OF_LOCK_STRUCTS
#endif /* UNIV_DEBUG */

#ifdef PRINT_NUM_OF_LOCK_STRUCTS
/*********************************************************************//**
Calculates the number of record lock structs in the record lock hash table.
@return number of record locks */
static
ulint
lock_get_n_rec_locks(void)
/*======================*/
{
	ulint	n_locks	= 0;
	ulint	i;

	ut_ad(lock_mutex_own());

	for (i = 0; i < hash_get_n_cells(lock_sys->rec_hash); i++) {
		const lock_t*	lock;

		for (lock = static_cast<const lock_t*>(
				HASH_GET_FIRST(lock_sys->rec_hash, i));
		     lock != 0;
		     lock = static_cast<const lock_t*>(
				HASH_GET_NEXT(hash, lock))) {

			n_locks++;
		}
	}

	return(n_locks);
}
#endif /* PRINT_NUM_OF_LOCK_STRUCTS */

/*********************************************************************//**
Prints info of locks for all transactions.
@return FALSE if not able to obtain lock mutex
and exits without printing info */
ibool
lock_print_info_summary(
/*====================*/
	FILE*	file,	/*!< in: file where to print */
	ibool	nowait)	/*!< in: whether to wait for the lock mutex */
{
	/* if nowait is FALSE, wait on the lock mutex,
	otherwise return immediately if fail to obtain the
	mutex. */
	if (!nowait) {
		lock_mutex_enter();
	} else if (lock_mutex_enter_nowait()) {
		fputs("FAIL TO OBTAIN LOCK MUTEX,"
		      " SKIP LOCK INFO PRINTING\n", file);
		return(FALSE);
	}

	if (lock_deadlock_found) {
		fputs("------------------------\n"
		      "LATEST DETECTED DEADLOCK\n"
		      "------------------------\n", file);

		if (!srv_read_only_mode) {
			ut_copy_file(file, lock_latest_err_file);
		}
	}

	fputs("------------\n"
	      "TRANSACTIONS\n"
	      "------------\n", file);

	fprintf(file, "Trx id counter " TRX_ID_FMT "\n",
		trx_sys_get_max_trx_id());

	fprintf(file,
		"Purge done for trx's n:o < " TRX_ID_FMT
		" undo n:o < " TRX_ID_FMT " state: ",
		purge_sys->iter.trx_no,
		purge_sys->iter.undo_no);

	/* Note: We are reading the state without the latch. One because it
	will violate the latching order and two because we are merely querying
	the state of the variable for display. */

	switch (purge_sys->state){
	case PURGE_STATE_INIT:
		/* Should never be in this state while the system is running. */
		ut_error;

	case PURGE_STATE_EXIT:
		fprintf(file, "exited");
		break;

	case PURGE_STATE_DISABLED:
		fprintf(file, "disabled");
		break;

	case PURGE_STATE_RUN:
		fprintf(file, "running");
		/* Check if it is waiting for more data to arrive. */
		if (!purge_sys->running) {
			fprintf(file, " but idle");
		}
		break;

	case PURGE_STATE_STOP:
		fprintf(file, "stopped");
		break;
	}

	fprintf(file, "\n");

	fprintf(file,
		"History list length %lu\n",
		(ulong) trx_sys->rseg_history_len);

#ifdef PRINT_NUM_OF_LOCK_STRUCTS
	fprintf(file,
		"Total number of lock structs in row lock hash table %lu\n",
		(ulong) lock_get_n_rec_locks());
#endif /* PRINT_NUM_OF_LOCK_STRUCTS */
	return(TRUE);
}

/** Prints transaction lock wait and MVCC state.
@param[in,out]	file	file where to print
@param[in]	trx	transaction
@param[in]	now	current time */
void
lock_trx_print_wait_and_mvcc_state(FILE* file, const trx_t* trx, time_t now)
{
	fprintf(file, "---");

	trx_print_latched(file, trx, 600);

	const ReadView*	read_view = trx_get_read_view(trx);

	if (read_view != NULL) {
		read_view->print_limits(file);
	}

	if (trx->lock.que_state == TRX_QUE_LOCK_WAIT) {

		fprintf(file,
			"------- TRX HAS BEEN WAITING %lu SEC"
			" FOR THIS LOCK TO BE GRANTED:\n",
			(ulong) difftime(now, trx->lock.wait_started));

		if (lock_get_type_low(trx->lock.wait_lock) == LOCK_REC) {
			mtr_t mtr;
			lock_rec_print(file, trx->lock.wait_lock, mtr);
		} else {
			lock_table_print(file, trx->lock.wait_lock);
		}

		fprintf(file, "------------------\n");
	}
}

/*********************************************************************//**
Prints info of locks for a transaction. */
static
void
lock_trx_print_locks(
/*=================*/
	FILE*		file,		/*!< in/out: File to write */
	const trx_t*	trx)		/*!< in: current transaction */
{
	mtr_t mtr;
	uint32_t i= 0;
	/* Iterate over the transaction's locks. */
	for (lock_t *lock = UT_LIST_GET_FIRST(trx->lock.trx_locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {
		if (lock_get_type_low(lock) == LOCK_REC) {

			lock_rec_print(file, lock, mtr);
		} else {
			ut_ad(lock_get_type_low(lock) & LOCK_TABLE);

			lock_table_print(file, lock);
		}

		if (++i == 10) {

			fprintf(file,
				"10 LOCKS PRINTED FOR THIS TRX:"
				" SUPPRESSING FURTHER PRINTS\n");

			break;
		}
	}
}

/** Functor to display all transactions (except recovered ones) */
struct lock_print_info
{
  lock_print_info(FILE* file, time_t now) : file(file), now(now) {}

  void operator()(const trx_t* trx) const
  {
    ut_ad(mutex_own(&trx_sys->mutex));
    ut_ad(trx->in_mysql_trx_list);
    lock_trx_print_wait_and_mvcc_state(file, trx, now);

    if (trx->will_lock && srv_print_innodb_lock_monitor)
      lock_trx_print_locks(file, trx);
  }

  FILE* const file;
  const time_t now;
};

/** Functor to display recovered read-write transactions */
struct lock_print_info_rw_recovered
{
  lock_print_info_rw_recovered(FILE* file, time_t now) : file(file),now(now) {}

  void operator()(const trx_t* trx) const
  {
    ut_ad(mutex_own(&trx_sys->mutex));
    ut_ad(trx->in_rw_trx_list);
    if (trx->mysql_thd)
      return;
    ut_ad(!trx->in_mysql_trx_list);

    lock_trx_print_wait_and_mvcc_state(file, trx, now);

    if (trx->will_lock && srv_print_innodb_lock_monitor)
      lock_trx_print_locks(file, trx);
  }

  FILE* const file;
  const time_t now;
};

/*********************************************************************//**
Prints info of locks for each transaction. This function assumes that the
caller holds the lock mutex and more importantly it will release the lock
mutex on behalf of the caller. (This should be fixed in the future). */
void
lock_print_info_all_transactions(
/*=============================*/
	FILE*		file)	/*!< in/out: file where to print */
{
	ut_ad(lock_mutex_own());

	fprintf(file, "LIST OF TRANSACTIONS FOR EACH SESSION:\n");
	const time_t now = time(NULL);

	mutex_enter(&trx_sys->mutex);
	ut_list_map(trx_sys->mysql_trx_list, lock_print_info(file, now));
	ut_list_map(trx_sys->rw_trx_list,
		    lock_print_info_rw_recovered(file, now));
	mutex_exit(&trx_sys->mutex);
	lock_mutex_exit();
	ut_ad(lock_validate());
}

#ifdef UNIV_DEBUG
/*********************************************************************//**
Find the the lock in the trx_t::trx_lock_t::table_locks vector.
@return true if found */
static
bool
lock_trx_table_locks_find(
/*======================*/
	trx_t*		trx,		/*!< in: trx to validate */
	const lock_t*	find_lock)	/*!< in: lock to find */
{
	bool		found = false;

	ut_ad(trx_mutex_own(trx));

	for (lock_list::const_iterator it = trx->lock.table_locks.begin(),
             end = trx->lock.table_locks.end(); it != end; ++it) {

		const lock_t*	lock = *it;

		if (lock == NULL) {

			continue;

		} else if (lock == find_lock) {

			/* Can't be duplicates. */
			ut_a(!found);
			found = true;
		}

		ut_a(trx == lock->trx);
		ut_a(lock_get_type_low(lock) & LOCK_TABLE);
		ut_a(lock->un_member.tab_lock.table != NULL);
	}

	return(found);
}

/*********************************************************************//**
Validates the lock queue on a table.
@return TRUE if ok */
static
ibool
lock_table_queue_validate(
/*======================*/
	const dict_table_t*	table)	/*!< in: table */
{
	const lock_t*	lock;

	ut_ad(lock_mutex_own());
	ut_ad(trx_sys_mutex_own());

	for (lock = UT_LIST_GET_FIRST(table->locks);
	     lock != NULL;
	     lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock)) {

		/* lock->trx->state cannot change from or to NOT_STARTED
		while we are holding the trx_sys->mutex. It may change
		from ACTIVE or PREPARED to PREPARED or COMMITTED. */
		trx_mutex_enter(lock->trx);
		ut_ad(trx_assert_started(lock->trx));
		if (trx_state_eq(lock->trx, TRX_STATE_COMMITTED_IN_MEMORY)) {
		} else if (!lock_get_wait(lock)) {
			ut_a(!lock_table_other_has_incompatible(
				     lock->trx, 0, table,
				     lock_get_mode(lock)));
		} else {
			ut_a(lock_table_has_to_wait_in_queue(lock));
		}

		ut_a(lock_trx_table_locks_find(lock->trx, lock));
		trx_mutex_exit(lock->trx);
	}

	return(TRUE);
}

/*********************************************************************//**
Validates the lock queue on a single record.
@return TRUE if ok */
static
ibool
lock_rec_queue_validate(
/*====================*/
	ibool			locked_lock_trx_sys,
					/*!< in: if the caller holds
					both the lock mutex and
					trx_sys_t->lock. */
	const buf_block_t*	block,	/*!< in: buffer block containing rec */
	const rec_t*		rec,	/*!< in: record to look at */
	const dict_index_t*	index,	/*!< in: index, or NULL if not known */
	const offset_t*		offsets)/*!< in: rec_get_offsets(rec, index) */
{
	const lock_t*	lock;
	ulint		heap_no;

	ut_a(rec);
	ut_a(block->frame == page_align(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));
	ut_ad(lock_mutex_own() == locked_lock_trx_sys);
	ut_ad(!index || dict_index_is_clust(index)
	      || !dict_index_is_online_ddl(index));

	heap_no = page_rec_get_heap_no(rec);

	if (!locked_lock_trx_sys) {
		lock_mutex_enter();
		mutex_enter(&trx_sys->mutex);
	}

	if (!page_rec_is_user_rec(rec)) {

		for (lock = lock_rec_get_first(lock_sys->rec_hash,
					       block, heap_no);
		     lock != NULL;
		     lock = lock_rec_get_next_const(heap_no, lock)) {

			ut_ad(!index || lock->index == index);

			trx_mutex_enter(lock->trx);
			ut_ad(!trx_is_ac_nl_ro(lock->trx));
			ut_ad(trx_state_eq(lock->trx,
					   TRX_STATE_COMMITTED_IN_MEMORY)
			      || !lock_get_wait(lock)
			      || lock_rec_has_to_wait_in_queue(lock));
			trx_mutex_exit(lock->trx);
		}

		goto func_exit;
	}

	ut_ad(page_rec_is_leaf(rec));
	ut_ad(lock_mutex_own());

	if (!index || !index->is_primary()) {
		/* Nothing we can do */
	} else if (trx_t* impl_trx = trx_rw_is_active_low(
			   lock_clust_rec_some_has_impl(rec, index, offsets),
			   NULL)) {
		/* impl_trx could have been committed before we
		acquire its mutex, but not thereafter. */

		mutex_enter(&impl_trx->mutex);
		ut_ad(impl_trx->state != TRX_STATE_NOT_STARTED);
		if (impl_trx->state == TRX_STATE_COMMITTED_IN_MEMORY) {
		} else if (const lock_t* other_lock
			   = lock_rec_other_has_expl_req(
				   LOCK_S, block, true, heap_no,
				   impl_trx)) {
			/* The impl_trx is holding an implicit lock on the
			given record 'rec'. So there cannot be another
			explicit granted lock.  Also, there can be another
			explicit waiting lock only if the impl_trx has an
			explicit granted lock. */

#ifdef WITH_WSREP
			if (wsrep_on(other_lock->trx->mysql_thd)) {
				if (!lock_get_wait(other_lock) ) {
					ib::info() << "WSREP impl BF lock conflict for my impl lock:\n BF:" <<
						((wsrep_thd_is_BF(impl_trx->mysql_thd, FALSE)) ? "BF" : "normal") << " exec: " <<
						wsrep_thd_exec_mode(impl_trx->mysql_thd) << " conflict: " <<
						wsrep_thd_conflict_state(impl_trx->mysql_thd, false) << " seqno: " <<
						wsrep_thd_trx_seqno(impl_trx->mysql_thd) << " SQL: " <<
						wsrep_thd_query(impl_trx->mysql_thd);

					trx_t* otrx = other_lock->trx;

					ib::info() << "WSREP other lock:\n BF:" <<
						((wsrep_thd_is_BF(otrx->mysql_thd, FALSE)) ? "BF" : "normal")  << " exec: " <<
						wsrep_thd_exec_mode(otrx->mysql_thd) << " conflict: " <<
						wsrep_thd_conflict_state(otrx->mysql_thd, false) << " seqno: " <<
						wsrep_thd_trx_seqno(otrx->mysql_thd) << " SQL: " <<
						wsrep_thd_query(otrx->mysql_thd);
				}

				if (!lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
						       block, heap_no,
						       impl_trx)) {
					ib::info() << "WSREP impl BF lock conflict";
				}
			} else
#endif /* WITH_WSREP */
			ut_ad(lock_get_wait(other_lock));
			ut_ad(lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
						block, heap_no, impl_trx));
		}

		mutex_exit(&impl_trx->mutex);
	}

	for (lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);
	     lock != NULL;
	     lock = lock_rec_get_next_const(heap_no, lock)) {

		ut_ad(!trx_is_ac_nl_ro(lock->trx));

		if (index) {
			ut_a(lock->index == index);
		}

		if (!lock_rec_get_gap(lock) && !lock_get_wait(lock)) {

			lock_mode	mode;

			if (lock_get_mode(lock) == LOCK_S) {
				mode = LOCK_X;
			} else {
				mode = LOCK_S;
			}

			const lock_t*	other_lock
				= lock_rec_other_has_expl_req(
					mode, block, false, heap_no,
					lock->trx);
#ifdef WITH_WSREP
			ut_a(!other_lock
			     || wsrep_thd_is_BF(lock->trx->mysql_thd, FALSE)
			     || wsrep_thd_is_BF(other_lock->trx->mysql_thd, FALSE));

#else
			ut_a(!other_lock);
#endif /* WITH_WSREP */
		} else if (lock_get_wait(lock) && !lock_rec_get_gap(lock)) {

			ut_a(lock_rec_has_to_wait_in_queue(lock));
		}
	}

	ut_ad(innodb_lock_schedule_algorithm == INNODB_LOCK_SCHEDULE_ALGORITHM_FCFS ||
		  lock_queue_validate(lock));

func_exit:
	if (!locked_lock_trx_sys) {
		lock_mutex_exit();
		mutex_exit(&trx_sys->mutex);
	}

	return(TRUE);
}

/*********************************************************************//**
Validates the record lock queues on a page.
@return TRUE if ok */
static
ibool
lock_rec_validate_page(
/*===================*/
	const buf_block_t*	block)	/*!< in: buffer block */
{
	const lock_t*	lock;
	const rec_t*	rec;
	ulint		nth_lock	= 0;
	ulint		nth_bit		= 0;
	ulint		i;
	mem_heap_t*	heap		= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	rec_offs_init(offsets_);

	ut_ad(!lock_mutex_own());

	lock_mutex_enter();
	mutex_enter(&trx_sys->mutex);
loop:
	lock = lock_rec_get_first_on_page_addr(
		lock_sys->rec_hash,
		block->page.id.space(), block->page.id.page_no());

	if (!lock) {
		goto function_exit;
	}

	ut_ad(!block->page.file_page_was_freed);

	for (i = 0; i < nth_lock; i++) {

		lock = lock_rec_get_next_on_page_const(lock);

		if (!lock) {
			goto function_exit;
		}
	}

	ut_ad(!trx_is_ac_nl_ro(lock->trx));

	/* Only validate the record queues when this thread is not
	holding a space->latch. */
	if (!sync_check_find(SYNC_FSP))
	for (i = nth_bit; i < lock_rec_get_n_bits(lock); i++) {

		if (i == PAGE_HEAP_NO_SUPREMUM
		    || lock_rec_get_nth_bit(lock, i)) {

			rec = page_find_rec_with_heap_no(block->frame, i);
			ut_a(rec);
			ut_ad(!lock_rec_get_nth_bit(lock, i)
			      || page_rec_is_leaf(rec));
			offsets = rec_get_offsets(rec, lock->index, offsets,
						  true, ULINT_UNDEFINED,
						  &heap);

			/* If this thread is holding the file space
			latch (fil_space_t::latch), the following
			check WILL break the latching order and may
			cause a deadlock of threads. */

			lock_rec_queue_validate(
				TRUE, block, rec, lock->index, offsets);

			nth_bit = i + 1;

			goto loop;
		}
	}

	nth_bit = 0;
	nth_lock++;

	goto loop;

function_exit:
	lock_mutex_exit();
	mutex_exit(&trx_sys->mutex);

	if (heap != NULL) {
		mem_heap_free(heap);
	}
	return(TRUE);
}

/*********************************************************************//**
Validates the table locks.
@return TRUE if ok */
static
ibool
lock_validate_table_locks(
/*======================*/
	const trx_ut_list_t*	trx_list)	/*!< in: trx list */
{
	const trx_t*	trx;

	ut_ad(lock_mutex_own());
	ut_ad(trx_sys_mutex_own());

	ut_ad(trx_list == &trx_sys->rw_trx_list);

	for (trx = UT_LIST_GET_FIRST(*trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_NEXT(trx_list, trx)) {

		const lock_t*	lock;

		check_trx_state(trx);

		for (lock = UT_LIST_GET_FIRST(trx->lock.trx_locks);
		     lock != NULL;
		     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

			if (lock_get_type_low(lock) & LOCK_TABLE) {

				lock_table_queue_validate(
					lock->un_member.tab_lock.table);
			}
		}
	}

	return(TRUE);
}

/*********************************************************************//**
Validate record locks up to a limit.
@return lock at limit or NULL if no more locks in the hash bucket */
static MY_ATTRIBUTE((warn_unused_result))
const lock_t*
lock_rec_validate(
/*==============*/
	ulint		start,		/*!< in: lock_sys->rec_hash
					bucket */
	ib_uint64_t*	limit)		/*!< in/out: upper limit of
					(space, page_no) */
{
	ut_ad(lock_mutex_own());
	ut_ad(trx_sys_mutex_own());

	for (const lock_t* lock = static_cast<const lock_t*>(
			HASH_GET_FIRST(lock_sys->rec_hash, start));
	     lock != NULL;
	     lock = static_cast<const lock_t*>(HASH_GET_NEXT(hash, lock))) {

		ib_uint64_t	current;

		ut_ad(!trx_is_ac_nl_ro(lock->trx));
		ut_ad(lock_get_type(lock) == LOCK_REC);

		current = ut_ull_create(
			lock->un_member.rec_lock.space,
			lock->un_member.rec_lock.page_no);

		if (current > *limit) {
			*limit = current + 1;
			return(lock);
		}
	}

	return(0);
}

/*********************************************************************//**
Validate a record lock's block */
static
void
lock_rec_block_validate(
/*====================*/
	ulint		space_id,
	ulint		page_no)
{
	/* The lock and the block that it is referring to may be freed at
	this point. We pass BUF_GET_POSSIBLY_FREED to skip a debug check.
	If the lock exists in lock_rec_validate_page() we assert
	!block->page.file_page_was_freed. */

	buf_block_t*	block;
	mtr_t		mtr;

	/* Make sure that the tablespace is not deleted while we are
	trying to access the page. */
	if (fil_space_t* space = fil_space_acquire_silent(space_id)) {
		dberr_t err = DB_SUCCESS;
		mtr_start(&mtr);

		block = buf_page_get_gen(
			page_id_t(space_id, page_no),
			page_size_t(space->flags),
			RW_X_LATCH, NULL,
			BUF_GET_POSSIBLY_FREED,
			__FILE__, __LINE__, &mtr, &err);

		if (err != DB_SUCCESS) {
			ib::error() << "Lock rec block validate failed for tablespace "
				   << space->name
				   << " space_id " << space_id
				   << " page_no " << page_no << " err " << err;
		}

		if (block) {
			buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

			ut_ad(lock_rec_validate_page(block));
		}

		mtr_commit(&mtr);

		fil_space_release(space);
	}
}

/*********************************************************************//**
Validates the lock system.
@return TRUE if ok */
static
bool
lock_validate()
/*===========*/
{
	typedef	std::pair<ulint, ulint>		page_addr_t;
	typedef std::set<
		page_addr_t,
		std::less<page_addr_t>,
		ut_allocator<page_addr_t> >	page_addr_set;

	page_addr_set	pages;

	lock_mutex_enter();
	mutex_enter(&trx_sys->mutex);

	ut_a(lock_validate_table_locks(&trx_sys->rw_trx_list));

	/* Iterate over all the record locks and validate the locks. We
	don't want to hog the lock_sys_t::mutex and the trx_sys_t::mutex.
	Release both mutexes during the validation check. */

	for (ulint i = 0; i < hash_get_n_cells(lock_sys->rec_hash); i++) {
		ib_uint64_t	limit = 0;

		while (const lock_t* lock = lock_rec_validate(i, &limit)) {
			if (lock_rec_find_set_bit(lock) == ULINT_UNDEFINED) {
				/* The lock bitmap is empty; ignore it. */
				continue;
			}
			const lock_rec_t& l = lock->un_member.rec_lock;
			pages.insert(std::make_pair(l.space, l.page_no));
		}
	}

	mutex_exit(&trx_sys->mutex);
	lock_mutex_exit();

	for (page_addr_set::const_iterator it = pages.begin();
	     it != pages.end();
	     ++it) {
		lock_rec_block_validate((*it).first, (*it).second);
	}

	return(true);
}
#endif /* UNIV_DEBUG */
/*============ RECORD LOCK CHECKS FOR ROW OPERATIONS ====================*/

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate insert of
a record. If they do, first tests if the query thread should anyway
be suspended for some reason; if not, then puts the transaction and
the query thread to the lock wait state and inserts a waiting request
for a gap x-lock to the lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_rec_insert_check_and_lock(
/*===========================*/
	ulint		flags,	/*!< in: if BTR_NO_LOCKING_FLAG bit is
				set, does nothing */
	const rec_t*	rec,	/*!< in: record after which to insert */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	dict_index_t*	index,	/*!< in: index */
	que_thr_t*	thr,	/*!< in: query thread */
	mtr_t*		mtr,	/*!< in/out: mini-transaction */
	ibool*		inherit)/*!< out: set to TRUE if the new
				inserted record maybe should inherit
				LOCK_GAP type locks from the successor
				record */
{
	ut_ad(block->frame == page_align(rec));
	ut_ad(!dict_index_is_online_ddl(index)
	      || index->is_primary()
	      || (flags & BTR_CREATE_FLAG));
	ut_ad(mtr->is_named_space(index->space));

	if (flags & BTR_NO_LOCKING_FLAG) {

		return(DB_SUCCESS);
	}

	ut_ad(!index->table->is_temporary());
	ut_ad(page_is_leaf(block->frame));

	dberr_t		err;
	lock_t*		lock;
	ibool		inherit_in = *inherit;
	trx_t*		trx = thr_get_trx(thr);
	const rec_t*	next_rec = page_rec_get_next_const(rec);
	ulint		heap_no = page_rec_get_heap_no(next_rec);

	lock_mutex_enter();
	/* Because this code is invoked for a running transaction by
	the thread that is serving the transaction, it is not necessary
	to hold trx->mutex here. */

	/* When inserting a record into an index, the table must be at
	least IX-locked. When we are building an index, we would pass
	BTR_NO_LOCKING_FLAG and skip the locking altogether. */
	ut_ad(lock_table_has(trx, index->table, LOCK_IX));

	lock = lock_rec_get_first(lock_sys->rec_hash, block, heap_no);

	if (lock == NULL) {
		/* We optimize CPU time usage in the simplest case */

		lock_mutex_exit();

		if (inherit_in && !dict_index_is_clust(index)) {
			/* Update the page max trx id field */
			page_update_max_trx_id(block,
					       buf_block_get_page_zip(block),
					       trx->id, mtr);
		}

		*inherit = FALSE;

		return(DB_SUCCESS);
	}

	/* Spatial index does not use GAP lock protection. It uses
	"predicate lock" to protect the "range" */
	if (dict_index_is_spatial(index)) {
		return(DB_SUCCESS);
	}

	*inherit = TRUE;

	/* If another transaction has an explicit lock request which locks
	the gap, waiting or granted, on the successor, the insert has to wait.

	An exception is the case where the lock by the another transaction
	is a gap type lock which it placed to wait for its turn to insert. We
	do not consider that kind of a lock conflicting with our insert. This
	eliminates an unnecessary deadlock which resulted when 2 transactions
	had to wait for their insert. Both had waiting gap type lock requests
	on the successor, which produced an unnecessary deadlock. */

	const ulint	type_mode = LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION;

	if (
#ifdef WITH_WSREP
	    lock_t* c_lock =
#endif /* WITH_WSREP */
	    lock_rec_other_has_conflicting(type_mode, block, heap_no, trx)) {
		/* Note that we may get DB_SUCCESS also here! */
		trx_mutex_enter(trx);

		err = lock_rec_enqueue_waiting(
#ifdef WITH_WSREP
			c_lock,
#endif /* WITH_WSREP */
			type_mode, block, heap_no, index, thr, NULL);

		trx_mutex_exit(trx);
	} else {
		err = DB_SUCCESS;
	}

	lock_mutex_exit();

	switch (err) {
	case DB_SUCCESS_LOCKED_REC:
		err = DB_SUCCESS;
		/* fall through */
	case DB_SUCCESS:
		if (!inherit_in || dict_index_is_clust(index)) {
			break;
		}

		/* Update the page max trx id field */
		page_update_max_trx_id(
			block, buf_block_get_page_zip(block), trx->id, mtr);
	default:
		/* We only care about the two return values. */
		break;
	}

#ifdef UNIV_DEBUG
	{
		mem_heap_t*	heap		= NULL;
		offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
		const offset_t*	offsets;
		rec_offs_init(offsets_);

		offsets = rec_get_offsets(next_rec, index, offsets_, true,
					  ULINT_UNDEFINED, &heap);

		ut_ad(lock_rec_queue_validate(
				FALSE, block, next_rec, index, offsets));

		if (heap != NULL) {
			mem_heap_free(heap);
		}
	}
#endif /* UNIV_DEBUG */

	return(err);
}

/*********************************************************************//**
Creates an explicit record lock for a running transaction that currently only
has an implicit lock on the record. The transaction instance must have a
reference count > 0 so that it can't be committed and freed before this
function has completed. */
static
void
lock_rec_convert_impl_to_expl_for_trx(
/*==================================*/
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record on page */
	dict_index_t*		index,	/*!< in: index of record */
	trx_t*			trx,	/*!< in/out: active transaction */
	ulint			heap_no)/*!< in: rec heap number to lock */
{
	ut_ad(page_rec_is_leaf(rec));

	DEBUG_SYNC_C("before_lock_rec_convert_impl_to_expl_for_trx");
	lock_mutex_enter();
	trx_mutex_enter(trx);
	ut_ad(trx->is_referenced());
	ut_ad(!trx_state_eq(trx, TRX_STATE_NOT_STARTED));

	if (!trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY)
	    && !lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
				  block, heap_no, trx)) {
		lock_rec_add_to_queue(LOCK_REC | LOCK_X | LOCK_REC_NOT_GAP,
				      block, heap_no, index, trx, true);
	}

	lock_mutex_exit();
	trx_mutex_exit(trx);
	trx->release_reference();

	DEBUG_SYNC_C("after_lock_rec_convert_impl_to_expl_for_trx");
}

/*********************************************************************//**
If a transaction has an implicit x-lock on a record, but no explicit x-lock
set on the record, sets one for it. */
static
void
lock_rec_convert_impl_to_expl(
/*==========================*/
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record on page */
	dict_index_t*		index,	/*!< in: index of record */
	const offset_t*		offsets)/*!< in: rec_get_offsets(rec, index) */
{
	trx_t*		trx;

	ut_ad(!lock_mutex_own());
	ut_ad(page_rec_is_user_rec(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(!page_rec_is_comp(rec) == !rec_offs_comp(offsets));

	if (dict_index_is_clust(index)) {
		trx_id_t	trx_id;

		trx_id = lock_clust_rec_some_has_impl(rec, index, offsets);

		trx = trx_rw_is_active(trx_id, NULL, true);
	} else {
		ut_ad(!dict_index_is_online_ddl(index));

		trx = lock_sec_rec_some_has_impl(rec, index, offsets);

		ut_ad(!trx || !lock_rec_other_trx_holds_expl(
				LOCK_S | LOCK_REC_NOT_GAP, trx, rec, block));
	}

	if (trx != 0) {
		ulint	heap_no = page_rec_get_heap_no(rec);

		ut_ad(trx->is_referenced());

		/* If the transaction is still active and has no
		explicit x-lock set on the record, set one for it.
		trx cannot be committed until the ref count is zero. */

		lock_rec_convert_impl_to_expl_for_trx(
			block, rec, index, trx, heap_no);
	}
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (update,
delete mark, or delete unmark) of a clustered index record. If they do,
first tests if the query thread should anyway be suspended for some
reason; if not, then puts the transaction and the query thread to the
lock wait state and inserts a waiting request for a record x-lock to the
lock queue.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_modify_check_and_lock(
/*=================================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: record which should be
					modified */
	dict_index_t*		index,	/*!< in: clustered index */
	const offset_t*		offsets,/*!< in: rec_get_offsets(rec, index) */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(dict_index_is_clust(index));
	ut_ad(block->frame == page_align(rec));

	if (flags & BTR_NO_LOCKING_FLAG) {

		return(DB_SUCCESS);
	}
	ut_ad(!dict_table_is_temporary(index->table));

	heap_no = rec_offs_comp(offsets)
		? rec_get_heap_no_new(rec)
		: rec_get_heap_no_old(rec);

	/* If a transaction has no explicit x-lock set on the record, set one
	for it */

	lock_rec_convert_impl_to_expl(block, rec, index, offsets);

	lock_mutex_enter();

	ut_ad(lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));

	err = lock_rec_lock(TRUE, LOCK_X | LOCK_REC_NOT_GAP,
			    block, heap_no, index, thr);

	MONITOR_INC(MONITOR_NUM_RECLOCK_REQ);

	lock_mutex_exit();

	ut_ad(lock_rec_queue_validate(FALSE, block, rec, index, offsets));

	if (err == DB_SUCCESS_LOCKED_REC) {
		err = DB_SUCCESS;
	}

	return(err);
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate modify (delete
mark or delete unmark) of a secondary index record.
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_modify_check_and_lock(
/*===============================*/
	ulint		flags,	/*!< in: if BTR_NO_LOCKING_FLAG
				bit is set, does nothing */
	buf_block_t*	block,	/*!< in/out: buffer block of rec */
	const rec_t*	rec,	/*!< in: record which should be
				modified; NOTE: as this is a secondary
				index, we always have to modify the
				clustered index record first: see the
				comment below */
	dict_index_t*	index,	/*!< in: secondary index */
	que_thr_t*	thr,	/*!< in: query thread
				(can be NULL if BTR_NO_LOCKING_FLAG) */
	mtr_t*		mtr)	/*!< in/out: mini-transaction */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index) || (flags & BTR_CREATE_FLAG));
	ut_ad(block->frame == page_align(rec));
	ut_ad(mtr->is_named_space(index->space));

	if (flags & BTR_NO_LOCKING_FLAG) {

		return(DB_SUCCESS);
	}
	ut_ad(!dict_table_is_temporary(index->table));

	heap_no = page_rec_get_heap_no(rec);

	/* Another transaction cannot have an implicit lock on the record,
	because when we come here, we already have modified the clustered
	index record, and this would not have been possible if another active
	transaction had modified this secondary index record. */

	lock_mutex_enter();

	ut_ad(lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));

	err = lock_rec_lock(TRUE, LOCK_X | LOCK_REC_NOT_GAP,
			    block, heap_no, index, thr);

	MONITOR_INC(MONITOR_NUM_RECLOCK_REQ);

	lock_mutex_exit();

#ifdef UNIV_DEBUG
	{
		mem_heap_t*	heap		= NULL;
		offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
		const offset_t*	offsets;
		rec_offs_init(offsets_);

		offsets = rec_get_offsets(rec, index, offsets_, true,
					  ULINT_UNDEFINED, &heap);

		ut_ad(lock_rec_queue_validate(
			FALSE, block, rec, index, offsets));

		if (heap != NULL) {
			mem_heap_free(heap);
		}
	}
#endif /* UNIV_DEBUG */

	if (err == DB_SUCCESS || err == DB_SUCCESS_LOCKED_REC) {
		/* Update the page max trx id field */
		/* It might not be necessary to do this if
		err == DB_SUCCESS (no new lock created),
		but it should not cost too much performance. */
		page_update_max_trx_id(block,
				       buf_block_get_page_zip(block),
				       thr_get_trx(thr)->id, mtr);
		err = DB_SUCCESS;
	}

	return(err);
}

/*********************************************************************//**
Like lock_clust_rec_read_check_and_lock(), but reads a
secondary index record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_sec_rec_read_check_and_lock(
/*=============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: secondary index */
	const offset_t*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	ulint			gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(!dict_index_is_clust(index));
	ut_ad(!dict_index_is_online_ddl(index));
	ut_ad(block->frame == page_align(rec));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));
	ut_ad(rec_offs_validate(rec, index, offsets));
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || dict_table_is_temporary(index->table)) {

		return(DB_SUCCESS);
	}

	heap_no = page_rec_get_heap_no(rec);

	/* Some transaction may have an implicit x-lock on the record only
	if the max trx id for the page >= min trx id for the trx list or a
	database recovery is running. */

	if ((page_get_max_trx_id(block->frame) >= trx_rw_min_trx_id()
	     || recv_recovery_is_on())
	    && !page_rec_is_supremum(rec)) {

		lock_rec_convert_impl_to_expl(block, rec, index, offsets);
	}

	lock_mutex_enter();

	ut_ad(mode != LOCK_X
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	ut_ad(mode != LOCK_S
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));

	err = lock_rec_lock(FALSE, mode | gap_mode,
			    block, heap_no, index, thr);

	MONITOR_INC(MONITOR_NUM_RECLOCK_REQ);

	lock_mutex_exit();

	ut_ad(lock_rec_queue_validate(FALSE, block, rec, index, offsets));

	return(err);
}

/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record.
@return DB_SUCCESS, DB_SUCCESS_LOCKED_REC, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock(
/*===============================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	const offset_t*		offsets,/*!< in: rec_get_offsets(rec, index) */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	ulint			gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	dberr_t	err;
	ulint	heap_no;

	ut_ad(dict_index_is_clust(index));
	ut_ad(block->frame == page_align(rec));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));
	ut_ad(gap_mode == LOCK_ORDINARY || gap_mode == LOCK_GAP
	      || gap_mode == LOCK_REC_NOT_GAP);
	ut_ad(rec_offs_validate(rec, index, offsets));

	if ((flags & BTR_NO_LOCKING_FLAG)
	    || srv_read_only_mode
	    || dict_table_is_temporary(index->table)) {

		return(DB_SUCCESS);
	}

	heap_no = page_rec_get_heap_no(rec);

	if (heap_no != PAGE_HEAP_NO_SUPREMUM) {

		lock_rec_convert_impl_to_expl(block, rec, index, offsets);
	}

	lock_mutex_enter();

	ut_ad(mode != LOCK_X
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	ut_ad(mode != LOCK_S
	      || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));

	err = lock_rec_lock(FALSE, mode | gap_mode, block, heap_no, index, thr);

	MONITOR_INC(MONITOR_NUM_RECLOCK_REQ);

	lock_mutex_exit();

	ut_ad(lock_rec_queue_validate(FALSE, block, rec, index, offsets));

	DEBUG_SYNC_C("after_lock_clust_rec_read_check_and_lock");

	return(err);
}
/*********************************************************************//**
Checks if locks of other transactions prevent an immediate read, or passing
over by a read cursor, of a clustered index record. If they do, first tests
if the query thread should anyway be suspended for some reason; if not, then
puts the transaction and the query thread to the lock wait state and inserts a
waiting request for a record lock to the lock queue. Sets the requested mode
lock on the record. This is an alternative version of
lock_clust_rec_read_check_and_lock() that does not require the parameter
"offsets".
@return DB_SUCCESS, DB_LOCK_WAIT, or DB_DEADLOCK */
dberr_t
lock_clust_rec_read_check_and_lock_alt(
/*===================================*/
	ulint			flags,	/*!< in: if BTR_NO_LOCKING_FLAG
					bit is set, does nothing */
	const buf_block_t*	block,	/*!< in: buffer block of rec */
	const rec_t*		rec,	/*!< in: user record or page
					supremum record which should
					be read or passed over by a
					read cursor */
	dict_index_t*		index,	/*!< in: clustered index */
	lock_mode		mode,	/*!< in: mode of the lock which
					the read cursor should set on
					records: LOCK_S or LOCK_X; the
					latter is possible in
					SELECT FOR UPDATE */
	ulint			gap_mode,/*!< in: LOCK_ORDINARY, LOCK_GAP, or
					LOCK_REC_NOT_GAP */
	que_thr_t*		thr)	/*!< in: query thread */
{
	mem_heap_t*	tmp_heap	= NULL;
	offset_t	offsets_[REC_OFFS_NORMAL_SIZE];
	offset_t*	offsets		= offsets_;
	dberr_t		err;
	rec_offs_init(offsets_);

	ut_ad(page_rec_is_leaf(rec));
	offsets = rec_get_offsets(rec, index, offsets, true,
				  ULINT_UNDEFINED, &tmp_heap);
	err = lock_clust_rec_read_check_and_lock(flags, block, rec, index,
						 offsets, mode, gap_mode, thr);
	if (tmp_heap) {
		mem_heap_free(tmp_heap);
	}

	if (err == DB_SUCCESS_LOCKED_REC) {
		err = DB_SUCCESS;
	}

	return(err);
}

/*******************************************************************//**
Release the last lock from the transaction's autoinc locks. */
UNIV_INLINE
void
lock_release_autoinc_last_lock(
/*===========================*/
	ib_vector_t*	autoinc_locks)	/*!< in/out: vector of AUTOINC locks */
{
	ulint		last;
	lock_t*		lock;

	ut_ad(lock_mutex_own());
	ut_a(!ib_vector_is_empty(autoinc_locks));

	/* The lock to be release must be the last lock acquired. */
	last = ib_vector_size(autoinc_locks) - 1;
	lock = *static_cast<lock_t**>(ib_vector_get(autoinc_locks, last));

	/* Should have only AUTOINC locks in the vector. */
	ut_a(lock_get_mode(lock) == LOCK_AUTO_INC);
	ut_a(lock_get_type(lock) == LOCK_TABLE);

	ut_a(lock->un_member.tab_lock.table != NULL);

	/* This will remove the lock from the trx autoinc_locks too. */
	lock_table_dequeue(lock);

	/* Remove from the table vector too. */
	lock_trx_table_locks_remove(lock);
}

/*******************************************************************//**
Check if a transaction holds any autoinc locks.
@return TRUE if the transaction holds any AUTOINC locks. */
static
ibool
lock_trx_holds_autoinc_locks(
/*=========================*/
	const trx_t*	trx)		/*!< in: transaction */
{
	ut_a(trx->autoinc_locks != NULL);

	return(!ib_vector_is_empty(trx->autoinc_locks));
}

/*******************************************************************//**
Release all the transaction's autoinc locks. */
static
void
lock_release_autoinc_locks(
/*=======================*/
	trx_t*		trx)		/*!< in/out: transaction */
{
	ut_ad(lock_mutex_own());
	/* If this is invoked for a running transaction by the thread
	that is serving the transaction, then it is not necessary to
	hold trx->mutex here. */

	ut_a(trx->autoinc_locks != NULL);

	/* We release the locks in the reverse order. This is to
	avoid searching the vector for the element to delete at
	the lower level. See (lock_table_remove_low()) for details. */
	while (!ib_vector_is_empty(trx->autoinc_locks)) {

		/* lock_table_remove_low() will also remove the lock from
		the transaction's autoinc_locks vector. */
		lock_release_autoinc_last_lock(trx->autoinc_locks);
	}

	/* Should release all locks. */
	ut_a(ib_vector_is_empty(trx->autoinc_locks));
}

/*******************************************************************//**
Gets the type of a lock. Non-inline version for using outside of the
lock module.
@return LOCK_TABLE or LOCK_REC */
ulint
lock_get_type(
/*==========*/
	const lock_t*	lock)	/*!< in: lock */
{
	return(lock_get_type_low(lock));
}

/*******************************************************************//**
Gets the id of the transaction owning a lock.
@return transaction id */
trx_id_t
lock_get_trx_id(
/*============*/
	const lock_t*	lock)	/*!< in: lock */
{
	return(trx_get_id_for_print(lock->trx));
}

/*******************************************************************//**
Gets the mode of a lock in a human readable string.
The string should not be free()'d or modified.
@return lock mode */
const char*
lock_get_mode_str(
/*==============*/
	const lock_t*	lock)	/*!< in: lock */
{
	ibool	is_gap_lock;

	is_gap_lock = lock_get_type_low(lock) == LOCK_REC
		&& lock_rec_get_gap(lock);

	switch (lock_get_mode(lock)) {
	case LOCK_S:
		if (is_gap_lock) {
			return("S,GAP");
		} else {
			return("S");
		}
	case LOCK_X:
		if (is_gap_lock) {
			return("X,GAP");
		} else {
			return("X");
		}
	case LOCK_IS:
		if (is_gap_lock) {
			return("IS,GAP");
		} else {
			return("IS");
		}
	case LOCK_IX:
		if (is_gap_lock) {
			return("IX,GAP");
		} else {
			return("IX");
		}
	case LOCK_AUTO_INC:
		return("AUTO_INC");
	default:
		return("UNKNOWN");
	}
}

/*******************************************************************//**
Gets the type of a lock in a human readable string.
The string should not be free()'d or modified.
@return lock type */
const char*
lock_get_type_str(
/*==============*/
	const lock_t*	lock)	/*!< in: lock */
{
	switch (lock_get_type_low(lock)) {
	case LOCK_REC:
		return("RECORD");
	case LOCK_TABLE:
		return("TABLE");
	default:
		return("UNKNOWN");
	}
}

/*******************************************************************//**
Gets the table on which the lock is.
@return table */
UNIV_INLINE
dict_table_t*
lock_get_table(
/*===========*/
	const lock_t*	lock)	/*!< in: lock */
{
	switch (lock_get_type_low(lock)) {
	case LOCK_REC:
		ut_ad(dict_index_is_clust(lock->index)
		      || !dict_index_is_online_ddl(lock->index));
		return(lock->index->table);
	case LOCK_TABLE:
		return(lock->un_member.tab_lock.table);
	default:
		ut_error;
		return(NULL);
	}
}

/*******************************************************************//**
Gets the id of the table on which the lock is.
@return id of the table */
table_id_t
lock_get_table_id(
/*==============*/
	const lock_t*	lock)	/*!< in: lock */
{
	dict_table_t*	table;

	table = lock_get_table(lock);

	return(table->id);
}

/** Determine which table a lock is associated with.
@param[in]	lock	the lock
@return name of the table */
const table_name_t&
lock_get_table_name(
	const lock_t*	lock)
{
	return(lock_get_table(lock)->name);
}

/*******************************************************************//**
For a record lock, gets the index on which the lock is.
@return index */
const dict_index_t*
lock_rec_get_index(
/*===============*/
	const lock_t*	lock)	/*!< in: lock */
{
	ut_a(lock_get_type_low(lock) == LOCK_REC);
	ut_ad(dict_index_is_clust(lock->index)
	      || !dict_index_is_online_ddl(lock->index));

	return(lock->index);
}

/*******************************************************************//**
For a record lock, gets the name of the index on which the lock is.
The string should not be free()'d or modified.
@return name of the index */
const char*
lock_rec_get_index_name(
/*====================*/
	const lock_t*	lock)	/*!< in: lock */
{
	ut_a(lock_get_type_low(lock) == LOCK_REC);
	ut_ad(dict_index_is_clust(lock->index)
	      || !dict_index_is_online_ddl(lock->index));

	return(lock->index->name);
}

/*******************************************************************//**
For a record lock, gets the tablespace number on which the lock is.
@return tablespace number */
ulint
lock_rec_get_space_id(
/*==================*/
	const lock_t*	lock)	/*!< in: lock */
{
	ut_a(lock_get_type_low(lock) == LOCK_REC);

	return(lock->un_member.rec_lock.space);
}

/*******************************************************************//**
For a record lock, gets the page number on which the lock is.
@return page number */
ulint
lock_rec_get_page_no(
/*=================*/
	const lock_t*	lock)	/*!< in: lock */
{
	ut_a(lock_get_type_low(lock) == LOCK_REC);

	return(lock->un_member.rec_lock.page_no);
}

/*********************************************************************//**
Cancels a waiting lock request and releases possible other transactions
waiting behind it. */
void
lock_cancel_waiting_and_release(
/*============================*/
	lock_t*	lock)	/*!< in/out: waiting lock request */
{
	que_thr_t*	thr;

	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(lock->trx));

	lock->trx->lock.cancel = true;

	if (lock_get_type_low(lock) == LOCK_REC) {

		lock_rec_dequeue_from_page(lock);
	} else {
		ut_ad(lock_get_type_low(lock) & LOCK_TABLE);

		if (lock->trx->autoinc_locks != NULL) {
			/* Release the transaction's AUTOINC locks. */
			lock_release_autoinc_locks(lock->trx);
		}

		lock_table_dequeue(lock);
		/* Remove the lock from table lock vector too. */
		lock_trx_table_locks_remove(lock);
	}

	/* Reset the wait flag and the back pointer to lock in trx. */

	lock_reset_lock_and_trx_wait(lock);

	/* The following function releases the trx from lock wait. */

	thr = que_thr_end_lock_wait(lock->trx);

	if (thr != NULL) {
		lock_wait_release_thread_if_suspended(thr);
	}

	lock->trx->lock.cancel = false;
}

/*********************************************************************//**
Unlocks AUTO_INC type locks that were possibly reserved by a trx. This
function should be called at the the end of an SQL statement, by the
connection thread that owns the transaction (trx->mysql_thd). */
void
lock_unlock_table_autoinc(
/*======================*/
	trx_t*	trx)	/*!< in/out: transaction */
{
	ut_ad(!lock_mutex_own());
	ut_ad(!trx_mutex_own(trx));
	ut_ad(!trx->lock.wait_lock);

	/* This can be invoked on NOT_STARTED, ACTIVE, PREPARED,
	but not COMMITTED transactions. */

	ut_ad(trx_state_eq(trx, TRX_STATE_NOT_STARTED)
	      || !trx_state_eq(trx, TRX_STATE_COMMITTED_IN_MEMORY));

	/* This function is invoked for a running transaction by the
	thread that is serving the transaction. Therefore it is not
	necessary to hold trx->mutex here. */

	if (lock_trx_holds_autoinc_locks(trx)) {
		lock_mutex_enter();

		lock_release_autoinc_locks(trx);

		lock_mutex_exit();
	}
}

/** Release the explicit locks of a committing transaction,
and release possible other transactions waiting because of these locks. */
void lock_trx_release_locks(trx_t* trx)
{
	ut_ad(UT_LIST_GET_LEN(trx->lock.trx_locks));

	lock_mutex_enter();
	lock_release(trx);
	trx->lock.n_rec_locks = 0;
	/* We don't remove the locks one by one from the vector for
	efficiency reasons. We simply reset it because we would have
	released all the locks anyway. */

	trx->lock.table_locks.clear();

	ut_ad(UT_LIST_GET_LEN(trx->lock.trx_locks) == 0);
	ut_ad(ib_vector_is_empty(trx->autoinc_locks));
	lock_mutex_exit();
	mem_heap_empty(trx->lock.lock_heap);
}

static inline dberr_t lock_trx_handle_wait_low(trx_t* trx)
{
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(trx));

	if (trx->lock.was_chosen_as_deadlock_victim) {
		return DB_DEADLOCK;
	}
	if (!trx->lock.wait_lock) {
		/* The lock was probably granted before we got here. */
		return DB_SUCCESS;
	}

	lock_cancel_waiting_and_release(trx->lock.wait_lock);
	return DB_LOCK_WAIT;
}

/*********************************************************************//**
Check whether the transaction has already been rolled back because it
was selected as a deadlock victim, or if it has to wait then cancel
the wait lock.
@return DB_DEADLOCK, DB_LOCK_WAIT or DB_SUCCESS */
dberr_t
lock_trx_handle_wait(
/*=================*/
	trx_t*	trx)	/*!< in/out: trx lock state */
{
	lock_mutex_enter();
	trx_mutex_enter(trx);
	dberr_t err = lock_trx_handle_wait_low(trx);
	lock_mutex_exit();
	trx_mutex_exit(trx);
	return err;
}

/*********************************************************************//**
Get the number of locks on a table.
@return number of locks */
ulint
lock_table_get_n_locks(
/*===================*/
	const dict_table_t*	table)	/*!< in: table */
{
	ulint		n_table_locks;

	lock_mutex_enter();

	n_table_locks = UT_LIST_GET_LEN(table->locks);

	lock_mutex_exit();

	return(n_table_locks);
}

#ifdef UNIV_DEBUG
/*******************************************************************//**
Do an exhaustive check for any locks (table or rec) against the table.
@return lock if found */
static
const lock_t*
lock_table_locks_lookup(
/*====================*/
	const dict_table_t*	table,		/*!< in: check if there are
						any locks held on records in
						this table or on the table
						itself */
	const trx_ut_list_t*	trx_list)	/*!< in: trx list to check */
{
	ut_a(table != NULL);
	ut_ad(lock_mutex_own());
	ut_ad(trx_sys_mutex_own());

	for (trx_t* trx = UT_LIST_GET_FIRST(*trx_list);
	     trx != NULL;
	     trx = UT_LIST_GET_NEXT(trx_list, trx)) {
		const lock_t* lock;

		trx_mutex_enter(trx);
		check_trx_state(trx);

		for (lock = UT_LIST_GET_FIRST(trx->lock.trx_locks);
		     lock != NULL;
		     lock = UT_LIST_GET_NEXT(trx_locks, lock)) {

			ut_a(lock->trx == trx);

			if (lock_get_type_low(lock) == LOCK_REC) {
				ut_ad(!dict_index_is_online_ddl(lock->index)
				      || dict_index_is_clust(lock->index));
				if (lock->index->table == table) {
					break;
				}
			} else if (lock->un_member.tab_lock.table == table) {
				break;
			}
		}

		trx_mutex_exit(trx);

		if (lock) {
			return lock;
		}
	}

	return NULL;
}
#endif /* UNIV_DEBUG */

/*******************************************************************//**
Check if there are any locks (table or rec) against table.
@return true if table has either table or record locks. */
bool
lock_table_has_locks(
/*=================*/
	const dict_table_t*	table)	/*!< in: check if there are any locks
					held on records in this table or on the
					table itself */
{
	ibool			has_locks;

	lock_mutex_enter();

	has_locks = UT_LIST_GET_LEN(table->locks) > 0 || table->n_rec_locks > 0;

#ifdef UNIV_DEBUG
	if (!has_locks) {
		mutex_enter(&trx_sys->mutex);

		ut_ad(!lock_table_locks_lookup(table, &trx_sys->rw_trx_list));

		mutex_exit(&trx_sys->mutex);
	}
#endif /* UNIV_DEBUG */

	lock_mutex_exit();

	return(has_locks);
}

/*******************************************************************//**
Initialise the table lock list. */
void
lock_table_lock_list_init(
/*======================*/
	table_lock_list_t*	lock_list)	/*!< List to initialise */
{
	UT_LIST_INIT(*lock_list, &lock_table_t::locks);
}

/*******************************************************************//**
Initialise the trx lock list. */
void
lock_trx_lock_list_init(
/*====================*/
	trx_lock_list_t*	lock_list)	/*!< List to initialise */
{
	UT_LIST_INIT(*lock_list, &lock_t::trx_locks);
}

/*******************************************************************//**
Set the lock system timeout event. */
void
lock_set_timeout_event()
/*====================*/
{
	os_event_set(lock_sys->timeout_event);
}

#ifdef UNIV_DEBUG
/*******************************************************************//**
Check if the transaction holds any locks on the sys tables
or its records.
@return the strongest lock found on any sys table or 0 for none */
const lock_t*
lock_trx_has_sys_table_locks(
/*=========================*/
	const trx_t*	trx)	/*!< in: transaction to check */
{
	const lock_t*	strongest_lock = 0;
	lock_mode	strongest = LOCK_NONE;

	lock_mutex_enter();

	const lock_list::const_iterator end = trx->lock.table_locks.end();
	lock_list::const_iterator it = trx->lock.table_locks.begin();

	/* Find a valid mode. Note: ib_vector_size() can be 0. */

	for (/* No op */; it != end; ++it) {
		const lock_t*	lock = *it;

		if (lock != NULL
		    && dict_is_sys_table(lock->un_member.tab_lock.table->id)) {

			strongest = lock_get_mode(lock);
			ut_ad(strongest != LOCK_NONE);
			strongest_lock = lock;
			break;
		}
	}

	if (strongest == LOCK_NONE) {
		lock_mutex_exit();
		return(NULL);
	}

	for (/* No op */; it != end; ++it) {
		const lock_t*	lock = *it;

		if (lock == NULL) {
			continue;
		}

		ut_ad(trx == lock->trx);
		ut_ad(lock_get_type_low(lock) & LOCK_TABLE);
		ut_ad(lock->un_member.tab_lock.table != NULL);

		lock_mode	mode = lock_get_mode(lock);

		if (dict_is_sys_table(lock->un_member.tab_lock.table->id)
		    && lock_mode_stronger_or_eq(mode, strongest)) {

			strongest = mode;
			strongest_lock = lock;
		}
	}

	lock_mutex_exit();

	return(strongest_lock);
}

/*******************************************************************//**
Check if the transaction holds an exclusive lock on a record.
@return whether the locks are held */
bool
lock_trx_has_rec_x_lock(
/*====================*/
	const trx_t*		trx,	/*!< in: transaction to check */
	const dict_table_t*	table,	/*!< in: table to check */
	const buf_block_t*	block,	/*!< in: buffer block of the record */
	ulint			heap_no)/*!< in: record heap number */
{
	ut_ad(heap_no > PAGE_HEAP_NO_SUPREMUM);

	lock_mutex_enter();
	ut_a(lock_table_has(trx, table, LOCK_IX)
	     || dict_table_is_temporary(table));
	ut_a(lock_rec_has_expl(LOCK_X | LOCK_REC_NOT_GAP,
			       block, heap_no, trx)
	     || dict_table_is_temporary(table));
	lock_mutex_exit();
	return(true);
}
#endif /* UNIV_DEBUG */

/** rewind(3) the file used for storing the latest detected deadlock and
print a heading message to stderr if printing of all deadlocks to stderr
is enabled. */
void
DeadlockChecker::start_print()
{
	ut_ad(lock_mutex_own());

	rewind(lock_latest_err_file);
	ut_print_timestamp(lock_latest_err_file);

	if (srv_print_all_deadlocks) {
		ib::info() << "Transactions deadlock detected, dumping"
			" detailed information.";
	}
}

/** Print a message to the deadlock file and possibly to stderr.
@param msg message to print */
void
DeadlockChecker::print(const char* msg)
{
	fputs(msg, lock_latest_err_file);

	if (srv_print_all_deadlocks) {
		ib::info() << msg;
	}
}

/** Print transaction data to the deadlock file and possibly to stderr.
@param trx transaction
@param max_query_len max query length to print */
void
DeadlockChecker::print(const trx_t* trx, ulint max_query_len)
{
	ut_ad(lock_mutex_own());

	ulint	n_rec_locks = lock_number_of_rows_locked(&trx->lock);
	ulint	n_trx_locks = UT_LIST_GET_LEN(trx->lock.trx_locks);
	ulint	heap_size = mem_heap_get_size(trx->lock.lock_heap);

	mutex_enter(&trx_sys->mutex);

	trx_print_low(lock_latest_err_file, trx, max_query_len,
		      n_rec_locks, n_trx_locks, heap_size);

	if (srv_print_all_deadlocks) {
		trx_print_low(stderr, trx, max_query_len,
			      n_rec_locks, n_trx_locks, heap_size);
	}

	mutex_exit(&trx_sys->mutex);
}

/** Print lock data to the deadlock file and possibly to stderr.
@param lock record or table type lock */
void
DeadlockChecker::print(const lock_t* lock)
{
	ut_ad(lock_mutex_own());

	if (lock_get_type_low(lock) == LOCK_REC) {
		mtr_t mtr;
		lock_rec_print(lock_latest_err_file, lock, mtr);

		if (srv_print_all_deadlocks) {
			lock_rec_print(stderr, lock, mtr);
		}
	} else {
		lock_table_print(lock_latest_err_file, lock);

		if (srv_print_all_deadlocks) {
			lock_table_print(stderr, lock);
		}
	}
}

/** Get the next lock in the queue that is owned by a transaction whose
sub-tree has not already been searched.
Note: "next" here means PREV for table locks.

@param lock Lock in queue
@param heap_no heap_no if lock is a record lock else ULINT_UNDEFINED

@return next lock or NULL if at end of queue */
const lock_t*
DeadlockChecker::get_next_lock(const lock_t* lock, ulint heap_no) const
{
	ut_ad(lock_mutex_own());

	do {
		if (lock_get_type_low(lock) == LOCK_REC) {
			ut_ad(heap_no != ULINT_UNDEFINED);
			lock = lock_rec_get_next_const(heap_no, lock);
		} else {
			ut_ad(heap_no == ULINT_UNDEFINED);
			ut_ad(lock_get_type_low(lock) == LOCK_TABLE);

			lock = UT_LIST_GET_NEXT(
				un_member.tab_lock.locks, lock);
		}

	} while (lock != NULL && is_visited(lock));

	ut_ad(lock == NULL
	      || lock_get_type_low(lock) == lock_get_type_low(m_wait_lock));

	return(lock);
}

/** Get the first lock to search. The search starts from the current
wait_lock. What we are really interested in is an edge from the
current wait_lock's owning transaction to another transaction that has
a lock ahead in the queue. We skip locks where the owning transaction's
sub-tree has already been searched.

Note: The record locks are traversed from the oldest lock to the
latest. For table locks we go from latest to oldest.

For record locks, we first position the "iterator" on the first lock on
the page and then reposition on the actual heap_no. This is required
due to the way the record lock has is implemented.

@param[out] heap_no if rec lock, else ULINT_UNDEFINED.
@return first lock or NULL */
const lock_t*
DeadlockChecker::get_first_lock(ulint* heap_no) const
{
	ut_ad(lock_mutex_own());

	const lock_t*	lock = m_wait_lock;

	if (lock_get_type_low(lock) == LOCK_REC) {
		hash_table_t*	lock_hash;

		lock_hash = lock->type_mode & LOCK_PREDICATE
			? lock_sys->prdt_hash
			: lock_sys->rec_hash;

		/* We are only interested in records that match the heap_no. */
		*heap_no = lock_rec_find_set_bit(lock);

		ut_ad(*heap_no <= 0xffff);
		ut_ad(*heap_no != ULINT_UNDEFINED);

		/* Find the locks on the page. */
		lock = lock_rec_get_first_on_page_addr(
			lock_hash,
			lock->un_member.rec_lock.space,
			lock->un_member.rec_lock.page_no);

		/* Position on the first lock on the physical record.*/
		if (!lock_rec_get_nth_bit(lock, *heap_no)) {
			lock = lock_rec_get_next_const(*heap_no, lock);
		}

		ut_a(!lock_get_wait(lock));
	} else {
		/* Table locks don't care about the heap_no. */
		*heap_no = ULINT_UNDEFINED;
		ut_ad(lock_get_type_low(lock) == LOCK_TABLE);
		dict_table_t*	table = lock->un_member.tab_lock.table;
		lock = UT_LIST_GET_FIRST(table->locks);
	}

	/* Must find at least two locks, otherwise there cannot be a
	waiting lock, secondly the first lock cannot be the wait_lock. */
	ut_a(lock != NULL);
	ut_a(lock != m_wait_lock ||
	     (innodb_lock_schedule_algorithm
	      	== INNODB_LOCK_SCHEDULE_ALGORITHM_VATS
	      && !thd_is_replication_slave_thread(lock->trx->mysql_thd)));

	/* Check that the lock type doesn't change. */
	ut_ad(lock_get_type_low(lock) == lock_get_type_low(m_wait_lock));

	return(lock);
}

/** Notify that a deadlock has been detected and print the conflicting
transaction info.
@param lock lock causing deadlock */
void
DeadlockChecker::notify(const lock_t* lock) const
{
	ut_ad(lock_mutex_own());

	start_print();

	print("\n*** (1) TRANSACTION:\n");

	print(m_wait_lock->trx, 3000);

	print("*** (1) WAITING FOR THIS LOCK TO BE GRANTED:\n");

	print(m_wait_lock);

	print("*** (2) TRANSACTION:\n");

	print(lock->trx, 3000);

	print("*** (2) HOLDS THE LOCK(S):\n");

	print(lock);

	/* It is possible that the joining transaction was granted its
	lock when we rolled back some other waiting transaction. */

	if (m_start->lock.wait_lock != 0) {
		print("*** (2) WAITING FOR THIS LOCK TO BE GRANTED:\n");

		print(m_start->lock.wait_lock);
	}

	DBUG_PRINT("ib_lock", ("deadlock detected"));
}

/** Select the victim transaction that should be rolledback.
@return victim transaction */
const trx_t*
DeadlockChecker::select_victim() const
{
	ut_ad(lock_mutex_own());
	ut_ad(m_start->lock.wait_lock != 0);
	ut_ad(m_wait_lock->trx != m_start);

	if (trx_weight_ge(m_wait_lock->trx, m_start)) {
		/* The joining transaction is 'smaller',
		choose it as the victim and roll it back. */
#ifdef WITH_WSREP
		if (wsrep_thd_is_BF(m_start->mysql_thd, TRUE)) {
			return(m_wait_lock->trx);
		}
#endif /* WITH_WSREP */
		return(m_start);
	}

#ifdef WITH_WSREP
	if (wsrep_thd_is_BF(m_wait_lock->trx->mysql_thd, TRUE)) {
		return(m_start);
	}
#endif /* WITH_WSREP */

	return(m_wait_lock->trx);
}

/** Looks iteratively for a deadlock. Note: the joining transaction may
have been granted its lock by the deadlock checks.
@return 0 if no deadlock else the victim transaction instance.*/
const trx_t*
DeadlockChecker::search()
{
	ut_ad(lock_mutex_own());
	ut_ad(!trx_mutex_own(m_start));

	ut_ad(m_start != NULL);
	ut_ad(m_wait_lock != NULL);
	check_trx_state(m_wait_lock->trx);
	ut_ad(m_mark_start <= s_lock_mark_counter);

	/* Look at the locks ahead of wait_lock in the lock queue. */
	ulint		heap_no;
	const lock_t*	lock = get_first_lock(&heap_no);

	for (;;) {
		/* We should never visit the same sub-tree more than once. */
		ut_ad(lock == NULL || !is_visited(lock));

		while (m_n_elems > 0 && lock == NULL) {

			/* Restore previous search state. */

			pop(lock, heap_no);

			lock = get_next_lock(lock, heap_no);
		}

		if (lock == NULL) {
			break;
		}

		if (lock == m_wait_lock) {

			/* We can mark this subtree as searched */
			ut_ad(lock->trx->lock.deadlock_mark <= m_mark_start);

			lock->trx->lock.deadlock_mark = ++s_lock_mark_counter;

			/* We are not prepared for an overflow. This 64-bit
			counter should never wrap around. At 10^9 increments
			per second, it would take 10^3 years of uptime. */

			ut_ad(s_lock_mark_counter > 0);

			/* Backtrack */
			lock = NULL;
			continue;
		}

		if (!lock_has_to_wait(m_wait_lock, lock)) {
			/* No conflict, next lock */
			lock = get_next_lock(lock, heap_no);
			continue;
		}

		if (lock->trx == m_start) {
			/* Found a cycle. */
			notify(lock);
			return select_victim();
		}

		if (is_too_deep()) {
			/* Search too deep to continue. */
			m_too_deep = true;
			return m_start;
		}

		/* We do not need to report autoinc locks to the upper
		layer. These locks are released before commit, so they
		can not cause deadlocks with binlog-fixed commit
		order. */
		if (m_report_waiters
		    && (lock_get_type_low(lock) != LOCK_TABLE
			|| lock_get_mode(lock) != LOCK_AUTO_INC)) {
			thd_rpl_deadlock_check(m_start->mysql_thd,
					       lock->trx->mysql_thd);
		}

		if (lock->trx->lock.que_state == TRX_QUE_LOCK_WAIT) {
			/* Another trx ahead has requested a lock in an
			incompatible mode, and is itself waiting for a lock. */

			++m_cost;

			if (!push(lock, heap_no)) {
				m_too_deep = true;
				return m_start;
			}

			m_wait_lock = lock->trx->lock.wait_lock;

			lock = get_first_lock(&heap_no);

			if (is_visited(lock)) {
				lock = get_next_lock(lock, heap_no);
			}
		} else {
			lock = get_next_lock(lock, heap_no);
		}
	}

	ut_a(lock == NULL && m_n_elems == 0);

	/* No deadlock found. */
	return(0);
}

/** Print info about transaction that was rolled back.
@param trx transaction rolled back
@param lock lock trx wants */
void
DeadlockChecker::rollback_print(const trx_t* trx, const lock_t* lock)
{
	ut_ad(lock_mutex_own());

	/* If the lock search exceeds the max step
	or the max depth, the current trx will be
	the victim. Print its information. */
	start_print();

	print("TOO DEEP OR LONG SEARCH IN THE LOCK TABLE"
	      " WAITS-FOR GRAPH, WE WILL ROLL BACK"
	      " FOLLOWING TRANSACTION \n\n"
	      "*** TRANSACTION:\n");

	print(trx, 3000);

	print("*** WAITING FOR THIS LOCK TO BE GRANTED:\n");

	print(lock);
}

/** Rollback transaction selected as the victim. */
void
DeadlockChecker::trx_rollback()
{
	ut_ad(lock_mutex_own());

	trx_t*	trx = m_wait_lock->trx;

	print("*** WE ROLL BACK TRANSACTION (1)\n");

	trx_mutex_enter(trx);

	trx->lock.was_chosen_as_deadlock_victim = true;

	lock_cancel_waiting_and_release(trx->lock.wait_lock);

	trx_mutex_exit(trx);
}

/** Check if a joining lock request results in a deadlock.
If a deadlock is found, we will resolve the deadlock by
choosing a victim transaction and rolling it back.
We will attempt to resolve all deadlocks.

@param[in]	lock	the lock request
@param[in,out]	trx	transaction requesting the lock

@return trx if it was chosen as victim
@retval	NULL if another victim was chosen,
or there is no deadlock (any more) */
const trx_t*
DeadlockChecker::check_and_resolve(const lock_t* lock, trx_t* trx)
{
	ut_ad(lock_mutex_own());
	ut_ad(trx_mutex_own(trx));
	check_trx_state(trx);
	ut_ad(!srv_read_only_mode);

	if (!innobase_deadlock_detect) {
		return(NULL);
	}

	/*  Release the mutex to obey the latching order.
	This is safe, because DeadlockChecker::check_and_resolve()
	is invoked when a lock wait is enqueued for the currently
	running transaction. Because m_trx is a running transaction
	(it is not currently suspended because of a lock wait),
	its state can only be changed by this thread, which is
	currently associated with the transaction. */

	trx_mutex_exit(trx);

	const trx_t*	victim_trx;
	const bool	report_waiters = trx->mysql_thd
		&& thd_need_wait_reports(trx->mysql_thd);

	/* Try and resolve as many deadlocks as possible. */
	do {
		DeadlockChecker	checker(trx, lock, s_lock_mark_counter,
					report_waiters);

		victim_trx = checker.search();

		/* Search too deep, we rollback the joining transaction only
		if it is possible to rollback. Otherwise we rollback the
		transaction that is holding the lock that the joining
		transaction wants. */
		if (checker.is_too_deep()) {

			ut_ad(trx == checker.m_start);
			ut_ad(trx == victim_trx);

			rollback_print(victim_trx, lock);

			MONITOR_INC(MONITOR_DEADLOCK);

			break;

		} else if (victim_trx != NULL && victim_trx != trx) {

			ut_ad(victim_trx == checker.m_wait_lock->trx);

			checker.trx_rollback();

			lock_deadlock_found = true;

			MONITOR_INC(MONITOR_DEADLOCK);
		}

	} while (victim_trx != NULL && victim_trx != trx);

	/* If the joining transaction was selected as the victim. */
	if (victim_trx != NULL) {

		print("*** WE ROLL BACK TRANSACTION (2)\n");

		lock_deadlock_found = true;
	}

	trx_mutex_enter(trx);

	return(victim_trx);
}

/*************************************************************//**
Updates the lock table when a page is split and merged to
two pages. */
UNIV_INTERN
void
lock_update_split_and_merge(
	const buf_block_t* left_block,	/*!< in: left page to which merged */
	const rec_t* orig_pred,		/*!< in: original predecessor of
					supremum on the left page before merge*/
	const buf_block_t* right_block)	/*!< in: right page from which merged */
{
	const rec_t* left_next_rec;

	ut_a(left_block && right_block);
	ut_a(orig_pred);

	lock_mutex_enter();

	left_next_rec = page_rec_get_next_const(orig_pred);

	/* Inherit the locks on the supremum of the left page to the
	first record which was moved from the right page */
	lock_rec_inherit_to_gap(
		left_block, left_block,
		page_rec_get_heap_no(left_next_rec),
		PAGE_HEAP_NO_SUPREMUM);

	/* Reset the locks on the supremum of the left page,
	releasing waiting transactions */
	lock_rec_reset_and_release_wait(left_block,
					PAGE_HEAP_NO_SUPREMUM);

	/* Inherit the locks to the supremum of the left page from the
	successor of the infimum on the right page */
	lock_rec_inherit_to_gap(left_block, right_block,
				PAGE_HEAP_NO_SUPREMUM,
				lock_get_min_heap_no(right_block));

	lock_mutex_exit();
}
