/*-------------------------------------------------------------------------
 *
 * xlogfuncs_gp.c
 *
 * GPDB-specific transaction log manager user interface functions
 *
 * This file contains WAL control and information functions.
 *
 * Portions Copyright (c) 2017-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/xlogfuncs_gp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "storage/lwlock.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "funcapi.h"
#include "libpq-fe.h"

#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "miscadmin.h"
#include "access/xlog.h"
#include "access/dr_served_snapshot.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "utils/guc.h"
#include "utils/faultinjector.h"
#include "utils/snapmgr.h"
#include "storage/proc.h"

/*
 * gp_create_restore_point: a distributed named point for cluster restore
 */
Datum
gp_create_restore_point(PG_FUNCTION_ARGS)
{

	typedef struct Context
	{
		CdbPgResults cdb_pgresults;
		XLogRecPtr	qd_restorepoint_lsn;
		int			index;
	}			Context;

	FuncCallContext *funcctx;
	Context    *context;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		text	   *restore_name = PG_GETARG_TEXT_P(0);
		char	   *restore_name_str;
		char	   *restore_command;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context for appropriate multiple function call */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* create tupdesc for result */
		tupdesc = CreateTemplateTupleDesc(2);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "gp_segment_id",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "restore_lsn",
						   LSNOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		context = (Context *) palloc(sizeof(Context));
		context->cdb_pgresults.pg_results = NULL;
		context->cdb_pgresults.numResults = 0;
		context->index = 0;
		funcctx->user_fctx = (void *) context;

		if (!IS_QUERY_DISPATCHER() || Gp_role != GP_ROLE_DISPATCH)
			elog(ERROR,
				 "cannot use gp_create_restore_point() when not in QD mode");

		restore_name_str = text_to_cstring(restore_name);
		restore_command =
			psprintf("SELECT pg_catalog.pg_create_restore_point(%s)", quote_literal_cstr(restore_name_str));

		/*
		 * Acquire TwophaseCommitLock in EXCLUSIVE mode. This is to ensure
		 * cluster-wide restore point consistency by blocking distributed
		 * commit prepared broadcasts from concurrent twophase transactions
		 * where a QE segment has written WAL.
		 */
		LWLockAcquire(TwophaseCommitLock, LW_EXCLUSIVE);

		SIMPLE_FAULT_INJECTOR("gp_create_restore_point_acquired_lock");

		CdbDispatchCommand(restore_command,
						   DF_NEED_TWO_PHASE | DF_CANCEL_ON_ERROR,
						   &context->cdb_pgresults);
		context->qd_restorepoint_lsn = DatumGetLSN(DirectFunctionCall1(pg_create_restore_point,
																	   PointerGetDatum(restore_name)));
		LWLockRelease(TwophaseCommitLock);

		pfree(restore_command);

		funcctx->user_fctx = (void *) context;
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Using SRF to return all the segment LSN information of the form
	 * {gp_segment_id, restore_lsn}
	 */
	funcctx = SRF_PERCALL_SETUP();
	context = (Context *) funcctx->user_fctx;

	while (context->index <= context->cdb_pgresults.numResults)
	{
		Datum		values[2];
		bool		nulls[2];
		HeapTuple	tuple;
		Datum		result;
		XLogRecPtr	restore_ptr;
		int			seg_index;
		uint32		hi;
		uint32		lo;

		if (context->index == 0)
		{
			/* Setting fields representing QD's restore point */
			seg_index = GpIdentity.segindex;
			restore_ptr = context->qd_restorepoint_lsn;
		}
		else
		{
			/* Setting fields representing QE's restore point */
			seg_index = context->index - 1;
			struct pg_result *pgresult = context->cdb_pgresults.pg_results[seg_index];
			ExecStatusType resultStatus = PQresultStatus(pgresult);

			if (resultStatus != PGRES_COMMAND_OK && resultStatus != PGRES_TUPLES_OK)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						 (errmsg("could not get the restore point from segment"),
						  errdetail("%s", PQresultErrorMessage(pgresult)))));
			Assert(PQntuples(pgresult) == 1);
			sscanf(PQgetvalue(pgresult, 0, 0), "%X/%X", &hi, &lo);
			restore_ptr = ((uint64) hi) << 32 | lo;
		}

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = Int16GetDatum(seg_index);
		values[1] = LSNGetDatum(restore_ptr);
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		context->index++;
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * gp_switch_wal: switch WAL on all segments and return meaningful info
 */
Datum
gp_switch_wal(PG_FUNCTION_ARGS)
{
	typedef struct Context
	{
		CdbPgResults cdb_pgresults;
		Datum qd_switch_lsn;
		Datum qd_switch_walfilename;
		int index;
	} Context;

	FuncCallContext *funcctx;
	Context    *context;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext	oldcontext;
		TupleDesc		tupdesc;
		char			*switch_command;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* switch to memory context for appropriate multiple function call */
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		/* create tupdesc for result */
		tupdesc = CreateTemplateTupleDesc(3);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "segment_id",
						   INT2OID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "switch_lsn",
						   LSNOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 3, "switch_walfilename",
						   TEXTOID, -1, 0);

		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		context = (Context *) palloc(sizeof(Context));
		context->cdb_pgresults.pg_results = NULL;
		context->cdb_pgresults.numResults = 0;
		context->index = 0;
		funcctx->user_fctx = (void *) context;

		if (!IS_QUERY_DISPATCHER() || Gp_role != GP_ROLE_DISPATCH)
			elog(ERROR,
				 "cannot use gp_switch_wal() when not in QD mode");

		switch_command = psprintf("SELECT switch_lsn, pg_walfile_name(switch_lsn) FROM pg_catalog.pg_switch_wal() switch_lsn");
		CdbDispatchCommand(switch_command,
						   DF_NEED_TWO_PHASE | DF_CANCEL_ON_ERROR,
						   &context->cdb_pgresults);
		context->qd_switch_lsn = DatumGetLSN(DirectFunctionCall1(pg_switch_wal, PointerGetDatum(NULL)));
		context->qd_switch_walfilename = DirectFunctionCall1(pg_walfile_name, context->qd_switch_lsn);

		pfree(switch_command);

		funcctx->user_fctx = (void *) context;
		MemoryContextSwitchTo(oldcontext);
	}

	/*
	 * Using SRF to return all the segment LSN information of the form
	 * {segment_id, switch_lsn, switch_walfilename}
	 */
	funcctx = SRF_PERCALL_SETUP();
	context = (Context *) funcctx->user_fctx;

	while (context->index <= context->cdb_pgresults.numResults)
	{
		Datum		values[3];
		bool		nulls[3];
		HeapTuple	tuple;
		Datum		result;
		Datum		switch_lsn;
		Datum		switch_walfilename;
		int			seg_index;

		if (context->index == 0)
		{
			/* Setting fields representing QD's switch WAL */
			seg_index = GpIdentity.segindex;
			switch_lsn = context->qd_switch_lsn;
			switch_walfilename = context->qd_switch_walfilename;
		}
		else
		{
			struct pg_result	*pgresult;
			ExecStatusType		resultStatus;
			uint32				hi, lo;

			/* Setting fields representing QE's switch WAL */
			seg_index = context->index - 1;
			pgresult = context->cdb_pgresults.pg_results[seg_index];
			resultStatus = PQresultStatus(pgresult);

			if (resultStatus != PGRES_COMMAND_OK && resultStatus != PGRES_TUPLES_OK)
				ereport(ERROR,
						(errcode(ERRCODE_GP_INTERCONNECTION_ERROR),
						 (errmsg("could not switch wal from segment"),
						  errdetail("%s", PQresultErrorMessage(pgresult)))));
			Assert(PQntuples(pgresult) == 1);

			sscanf(PQgetvalue(pgresult, 0, 0), "%X/%X", &hi, &lo);
			switch_lsn = LSNGetDatum(((uint64) hi) << 32 | lo);
			switch_walfilename = CStringGetTextDatum(PQgetvalue(pgresult, 0, 1));
		}

		/*
		 * Form tuple with appropriate data.
		 */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = Int16GetDatum(seg_index);
		values[1] = switch_lsn;
		values[2] = switch_walfilename;
		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		result = HeapTupleGetDatum(tuple);

		context->index++;
		SRF_RETURN_NEXT(funcctx, result);
	}

	SRF_RETURN_DONE(funcctx);
}

/* ---------------------------------------------------------------------------
 * DR recovery control
 *
 * A DR replica recovers *up to a distributed restore point* and serves reads
 * there; advancing means re-pointing every node at the next restore point and
 * letting replay run to it.  Driving that from outside the cluster needs a
 * connection to every node, which is why it lived in the gg_recovery utility.
 * These functions do it from the coordinator instead: each dispatches to every
 * primary segment and applies the same step locally, so one SQL call moves the
 * whole cluster.
 *
 * Status is read from the gg_stat_dr_replica / gg_stat_dr_replica_summary views;
 * there is deliberately no gg_dr_stat() function duplicating them.
 * ---------------------------------------------------------------------------
 */

/*
 * Apply one recovery-control step to every primary segment and to this node.
 *
 * The remote leg is a dispatched SQL command; the local leg calls the same
 * builtin directly.  None of these steps writes WAL -- ALTER SYSTEM writes
 * postgresql.auto.conf and the replay-control builtins touch shared memory --
 * which is what makes them legal on a node in recovery.
 *
 * What is dispatched matters as much as what it does.  On a replica every
 * executor snapshot is the image frozen at the served restore point, whose xmin
 * is old by construction once replay resumes.  A statement holding one is
 * cancelled by the first replayed prune or VACUUM that removes a row version
 * behind that xmin (max_standby_archive_delay = 0), and is refused outright
 * while the node has nothing to serve.  So nothing here reaches the executor on
 * a segment: the per-node primitive is a procedure, dispatched as a CALL, which
 * drops the snapshot its portal pushed before doing anything; and the state poll
 * is a SHOW of a GUC computed from shared memory, which takes no snapshot at all
 * (PlannedStmtRequiresSnapshot exempts it).
 */
static void
dr_dispatch(const char *cmd)
{
	CdbPgResults results = {NULL, 0};

	CdbDispatchCommand((char *) cmd, DF_CANCEL_ON_ERROR, &results);
	cdbdisp_clearCdbPgResults(&results);
}

/*
 * Write the pause target into postgresql.auto.conf on THIS node.
 *
 * Calls AlterSystemSetConfigFile() directly instead of executing an ALTER SYSTEM
 * statement, because standard_ProcessUtility() gates AlterSystemStmt behind
 * PreventInTransactionBlock() and the caller may be inside one: CALL is legal in
 * a transaction block, and on a segment the dispatched statement may be
 * executing inside the dispatched transaction.  The underlying action is
 * identical -- it writes the file and nothing else.
 */
static void
dr_write_pause_target(const char *target)
{
	A_Const    *con = makeNode(A_Const);
	VariableSetStmt *vset = makeNode(VariableSetStmt);
	AlterSystemStmt *stmt = makeNode(AlterSystemStmt);

	con->val.type = T_String;
	con->val.val.str = pstrdup(target);
	con->location = -1;

	vset->kind = VAR_SET_VALUE;
	vset->name = pstrdup("gp_pause_on_restore_point_replay");
	vset->args = list_make1(con);
	vset->is_local = false;
	stmt->setstmt = vset;

	AlterSystemSetConfigFile(stmt);
}

/*
 * Hold no snapshot from here on.
 *
 * The portal pushed one for this statement, and parse analysis registered a
 * catalog snapshot.  On a replica both are the frozen image, so both publish
 * an xmin from the served restore point, and the first replayed prune or VACUUM
 * that removes a row version behind it cancels this backend.  A switch waits for
 * as long as production takes to create and archive the next point, so it was
 * cancelled by the first such record in the meantime: with any churn on
 * production the switch failed every time, with none it always worked.
 *
 * Only a procedure can do this.  A function runs inside its caller's executor,
 * whose registered snapshot cannot be released from underneath it;
 * PortalRunUtility tolerates a procedure popping the snapshot it pushed, which
 * is how procedures that COMMIT work.  Same problem, same shape as
 * pg_wal_replay_wait() in PostgreSQL 17.
 *
 * `strict` is for the wait: REPEATABLE READ and SERIALIZABLE register the
 * transaction snapshot for the whole transaction, and a snapshot from an
 * enclosing function outlives us too, so neither can be made safe -- refuse
 * rather than wait exposed.  The per-node primitive runs for microseconds and
 * merely drops what it can.
 */
static void
dr_release_snapshots(const char *fname, bool strict)
{
	if (ActiveSnapshotSet())
		PopActiveSnapshot();
	InvalidateCatalogSnapshot();

	if (strict && GetOldestSnapshot() != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s() must be called without an active or registered snapshot", fname),
				 errdetail("On a disaster-recovery replica a snapshot is the image frozen at the served restore point; a backend holding one while replay runs is cancelled by the first row version replay removes."),
				 errhint("CALL it directly in a READ COMMITTED transaction, not from a function and not inside a REPEATABLE READ or SERIALIZABLE transaction.")));
	Assert(!strict || !TransactionIdIsValid(MyPgXact->xmin));
}

/*
 * The per-node arm: point this node's pause target at `target`, have the
 * startup process read it, and resume replay unless the node is already stopped
 * there.
 *
 * The order is what makes it safe, and it is local to the node: the target is
 * on disk and signalled before replay moves, so a node cannot free-run past a
 * point it has not been told about.  The startup process handles SIGHUP before
 * it leaves its pause loop and again before every record, so the reload lands
 * before the next restore-point record is compared.
 *
 * A node ALREADY stopped at `target` is left alone.  Resuming it would send it
 * past a restore point it has reached, with nothing ahead to stop at -- and the
 * coordinator's wait would then never see it there.  Nodes do get there ahead of
 * a switch: one that restarted replays to its armed target on its own, and an
 * operator can drive a node by hand.
 */
static void
dr_node_arm(const char *target)
{
	char		local[MAXFNAMELEN];

	dr_write_pause_target(target);
	DirectFunctionCall1(pg_reload_conf, (Datum) 0);

	GetPausedRestorePointName(local, sizeof(local));
	if (RecoveryInProgress() && strcmp(local, target) != 0)
		DirectFunctionCall1(pg_wal_replay_resume, (Datum) 0);
}

/*
 * Arm `target` on every node: the segments through the per-node procedure,
 * then this node directly.  Dispatched as a CALL, gg_dr_switch_node() runs on
 * the segment through ProcessUtility and never the executor, so it is neither
 * refused for want of a served image nor cancelled for holding one.
 */
static void
dr_arm_everywhere(const char *target)
{
	char	   *cmd;

	cmd = psprintf("CALL pg_catalog.gg_dr_switch_node(%s, false)",
				   quote_literal_cstr(target));
	dr_dispatch(cmd);
	pfree(cmd);

	dr_node_arm(target);
}

/*
 * Start serving the image frozen at `target` on every node: segments first,
 * then this node, so no node serves the new point while another still serves
 * the old.  Only the coordinator may decide this, and only once it has
 * established that every node has arrived (dr_all_paused_at): a node that
 * published on its own arrival would serve the new point while a lagging peer
 * was still short of it -- the torn read, one restore point later.  That is
 * why arming and publishing are two separate calls of the per-node procedure
 * rather than one call that publishes whenever it happens to have the image.
 */
static void
dr_publish_everywhere(const char *target)
{
	char	   *cmd;

	cmd = psprintf("CALL pg_catalog.gg_dr_switch_node(%s, true)",
				   quote_literal_cstr(target));
	dr_dispatch(cmd);
	pfree(cmd);

	DRServedSnapshotPublish(target);
}

/*
 * Is every node stopped at restore point `target`?
 *
 * The per-node answer is the restore point actually *reached* (from the replayed
 * WAL record), not the configured pause target, so a node that has not got there
 * yet reports its previous point -- or nothing at all once it has resumed, since
 * SetRecoveryPause(false) clears the name.
 *
 * The segments are asked with SHOW, not SELECT.  A SHOW is one of the utility
 * statements PlannedStmtRequiresSnapshot exempts, so it runs on a segment
 * without ever taking the frozen image, and gg_dr_paused_restore_point's show
 * hook reads the same shared memory this node reads directly below.  It reports
 * '' unless the node is paused, so "stopped, and stopped there" is one string
 * comparison.
 */
static bool
dr_all_paused_at(const char *target)
{
	CdbPgResults results = {NULL, 0};
	char		local[MAXFNAMELEN];
	bool		all = true;
	int			i;

	if (!RecoveryInProgress() || !RecoveryIsPaused())
		return false;

	GetPausedRestorePointName(local, sizeof(local));
	if (local[0] == '\0' || strcmp(local, target) != 0)
		return false;

	CdbDispatchCommand("SHOW gg_dr_paused_restore_point",
					   DF_CANCEL_ON_ERROR, &results);
	for (i = 0; i < results.numResults; i++)
	{
		struct pg_result *pgresult = results.pg_results[i];

		if (PQresultStatus(pgresult) != PGRES_TUPLES_OK || PQntuples(pgresult) != 1 ||
			strcmp(PQgetvalue(pgresult, 0, 0), target) != 0)
		{
			all = false;
			break;
		}
	}
	cdbdisp_clearCdbPgResults(&results);
	return all;
}

/* Refuse anything that is not a DR coordinator driving its own cluster. */
static void
dr_control_precheck(const char *fname)
{
	if (!IS_QUERY_DISPATCHER() || Gp_role != GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s() must be called on the coordinator in dispatch mode", fname)));
	if (!RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s() requires a cluster in recovery", fname),
				 errdetail("This cluster is not a DR replica; it is online read-write.")));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call %s()", fname)));
}

/*
 * gg_dr_switch_node(restore_point, publish) -- procedure
 *
 * The per-node half of gg_dr_switch(), which dispatches it to every segment.
 * With publish = false it arms `restore_point` as this node's pause target and
 * resumes replay; with publish = true it starts serving the image frozen there,
 * if this node has one.  Two calls rather than one because only the coordinator
 * knows when the second is safe (see dr_publish_everywhere).
 *
 * Refused on a coordinator in dispatch mode: there gg_dr_switch() is the right
 * call, and this one would arm a single node of a cluster whose other nodes
 * nobody told.  A node reached in utility mode is fine -- that is an operator
 * driving one node by hand, which is what `ggdr switch --content` does.
 */
Datum
gg_dr_switch_node(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));
	bool		publish = PG_GETARG_BOOL(1);

	if (Gp_role == GP_ROLE_DISPATCH)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gg_dr_switch_node() is the per-node half of gg_dr_switch()"),
				 errhint("CALL gg_dr_switch() on the coordinator to switch the whole cluster.")));
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call gg_dr_switch_node()")));
	if (target[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("restore point name cannot be empty")));

	dr_release_snapshots("gg_dr_switch_node", false);

	if (publish)
		DRServedSnapshotPublish(target);
	else
		dr_node_arm(target);

	PG_RETURN_VOID();
}

/*
 * gg_dr_switch(restore_point) -- procedure
 *
 * Stop-and-go: re-point every node's pause target at `restore_point`, resume
 * replay, wait until all of them are paused there -- a consistent serve point
 * -- and then start serving it everywhere.  Recovery only moves forward, so
 * `restore_point` must be one that production has created (or will create)
 * ahead of the current position; arming it before production creates it is
 * fine and is how you catch a point cleanly instead of overshooting it.
 *
 * Blocks until every node is paused there.  There is no timeout argument: see
 * the wait loop below.  Cancel it like any other statement.  It is a procedure
 * and not a function so that it can wait without holding a snapshot
 * (dr_release_snapshots), which is why it is CALLed rather than SELECTed.
 *
 * Calling it with the point the cluster is already stopped at is well defined
 * and cheap: it re-publishes the served image without resuming replay.  That is
 * what a node needs after a restart, since the image lives in shared memory.
 */
Datum
gg_dr_switch(PG_FUNCTION_ARGS)
{
	char	   *target = text_to_cstring(PG_GETARG_TEXT_PP(0));

	dr_control_precheck("gg_dr_switch");
	if (target[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("restore point name cannot be empty")));

	dr_release_snapshots("gg_dr_switch", true);

	/*
	 * Already stopped there?  Then this is a re-publish, not a switch: publish,
	 * done -- no arming, no resume, no round trip beyond the poll.
	 *
	 * Reaching this on purpose is how a restarted node gets its served image
	 * back: the frozen image lives in shared memory, which the postmaster does
	 * not outlive, while the pause target is on disk and survives.
	 */
	if (dr_all_paused_at(target))
	{
		dr_publish_everywhere(target);
		PG_RETURN_VOID();
	}

	dr_arm_everywhere(target);

	/*
	 * Wait until every node is paused there.  No timeout: how long this takes is
	 * a property of how much WAL lies between here and the target, which the
	 * caller cannot usefully guess -- and a timeout that fires leaves the target
	 * armed anyway, so it would report a failure that is not one.  The wait is
	 * interruptible, so pg_cancel_backend() (or Ctrl-C) is the way out.
	 *
	 * A poll dispatches, and a dispatch can look a catalog up -- creating a gang
	 * checks that the user is a superuser, for one -- which registers a catalog
	 * snapshot: the frozen image, on a replica.  Drop it before sleeping, so that
	 * nothing old is pinned across the second spent waiting.  The microseconds
	 * inside the dispatch are the residual exposure, and they are the same
	 * microseconds any CALL spends between its portal pushing a snapshot and the
	 * procedure popping it.
	 */
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (dr_all_paused_at(target))
		{
			dr_publish_everywhere(target);
			PG_RETURN_VOID();
		}
		InvalidateCatalogSnapshot();
		pg_usleep(1000000L);
	}
}

/*
 * gg_dr_promote() -> bool
 *
 * Promote the whole DR cluster to an online read-write cluster, cutting every
 * node at the restore point it is currently paused at.  Irreversible: recovery
 * only moves forward.
 *
 * No arguments: a replica is promoted from where it *is*, and where it is, is a
 * restore point (that is the only state it serves from).  Naming the point again
 * would only let the caller assert something the function already validates --
 * that every node is paused, and at the same one -- so it validates and reports
 * the point instead of asking for it.  Use gg_dr_switch() first to choose a
 * different cut.
 *
 * Segments are promoted before the coordinator so that when the coordinator's
 * FTS starts probing, the segments it probes are already live.
 *
 * Unlike the utility's older two-phase promote there is no restart: DR
 * behaviour is now keyed on "in recovery with hot standby" (IsDRReplicaMode),
 * so it lifts by itself the moment recovery ends.
 */
Datum
gg_dr_promote(PG_FUNCTION_ARGS)
{
	char		local[MAXFNAMELEN];

	dr_control_precheck("gg_dr_promote");

	/*
	 * Precondition: one consistent cut.  Promoting from anywhere else -- an
	 * immediate pause, or mid-replay -- gives a cluster whose segments stopped
	 * at unrelated points, which is not a restorable image.
	 */
	GetPausedRestorePointName(local, sizeof(local));
	if (local[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("the cluster is not paused at a restore point"),
				 errhint("Use gg_dr_switch() to reach a consistent restore point first.")));
	if (!dr_all_paused_at(local))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("not every node is paused at restore point \"%s\"", local),
				 errhint("Check gg_stat_dr_replica_summary.consistent_paused_point -- a promotion cuts at the point replay has reached, not at the one being served.")));

	/*
	 * Promote first, then resume: promotion only arms the trigger, and the
	 * startup process is sitting in the restore-point pause, so it needs the
	 * resume to notice.  Segments before the coordinator, so the coordinator's
	 * FTS finds them live when it starts probing.  The resume is guarded on both
	 * legs because promotion may already have ended recovery by then.
	 */
	dr_dispatch("SELECT pg_catalog.pg_promote(false)");
	dr_dispatch("SELECT pg_catalog.pg_wal_replay_resume() WHERE pg_catalog.pg_is_in_recovery()");

	DirectFunctionCall2(pg_promote, BoolGetDatum(false), Int32GetDatum(60));
	if (RecoveryInProgress())
		DirectFunctionCall1(pg_wal_replay_resume, (Datum) 0);

	/*
	 * Wait for this node to leave recovery.
	 *
	 * Scope of the return value, stated precisely because it is easy to assume
	 * more: true means *this cluster* left recovery at the requested cut.  The
	 * segments were promoted first, but asynchronously (pg_promote(false)), so
	 * one may still be finishing when this returns.
	 *
	 * They deliberately are not waited on here.  A standby coordinator cannot
	 * poll them -- the QE-side protocol check refuses a standby QD talking to a
	 * promoted QE, and vice versa once this node is promoted -- and they cannot
	 * be promoted synchronously either, because recoveryPausesHere() does not
	 * watch for the promote trigger, so pg_promote(true) on a paused node would
	 * wait for a resume only another session could send.  Polling from outside,
	 * with a connection per node, is what gg_recovery promote does; use it when
	 * you need the stronger guarantee.  In practice the segments are up within a
	 * second or two, and the first distributed query will wait for FTS anyway.
	 */
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (!RecoveryInProgress())
		{
			/*
			 * gp_configuration_history is replicated like any other catalog, so
			 * this cluster is now carrying PRODUCTION's configuration history
			 * into a cluster that is about to write its own.  Nothing here can
			 * tidy it up: this function runs inside the promoting backend, and
			 * the segments may still be finishing.  Say so once, rather than
			 * leave it to be discovered.
			 */
			ereport(NOTICE,
					(errmsg("gp_configuration_history still holds the rows replayed from production"),
					 errhint("Run \"ggdr promote\" instead of this function to replace it with a single "
							 "promotion row, or clear it by hand.")));
			PG_RETURN_BOOL(true);
		}
		pg_usleep(1000000L);
	}
}
