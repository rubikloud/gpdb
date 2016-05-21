
/*-------------------------------------------------------------------------
 *
 * cdbdisp_query.c
 *	  Functions to dispatch command string or plan to QExecutors.
 *
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "cdb/cdbconn.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbutil.h"
#include "cdb/cdbvars.h"
#include "cdb/cdbmutate.h"
#include "cdb/cdbrelsize.h"
#include "cdb/cdbsrlz.h"
#include "tcop/tcopprot.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "miscadmin.h"

#include "cdb/cdbdisp.h"
#include "cdb/cdbdisp_query.h"
#include "cdb/cdbdisp_thread.h" /* for CdbDispatchCmdThreads and DispatchCommandParms */
#include "cdb/cdbdisp_dtx.h" /* for qdSerializeDtxContextInfo() */
#include "cdb/cdbdispatchresult.h"

extern bool Test_print_direct_dispatch_info;

/*
 * We need an array describing the relationship between a slice and
 * the number of "child" slices which depend on it.
 */
typedef struct
{
	int			sliceIndex;
	int			children;
	Slice	   *slice;
} sliceVec;

/*
 * Parameter structure for Greenplum Database Queries
 */
typedef struct DispatchCommandQueryParms
{
	/*
	 * The SQL command
	 */
	const char *strCommand;
	int strCommandlen;
	char *serializedQuerytree;
	int serializedQuerytreelen;
	char *serializedPlantree;
	int serializedPlantreelen;
	char *serializedParams;
	int serializedParamslen;
	char *serializedSliceInfo;
	int serializedSliceInfolen;

	/*
	 * serialized DTX context string
	 */
	char *serializedDtxContextInfo;
	int serializedDtxContextInfolen;

	int rootIdx;

	/*
	 * the sequence server info.
	 */
	char *seqServerHost; /* If non-null, sequence server host name. */
	int seqServerHostlen;
	int seqServerPort; /* If seqServerHost non-null, sequence server port. */

	/*
	 * Used by dispatch agent if NOT using sliced execution
	 */
	int primary_gang_id;
} DispatchCommandQueryParms;

static void stripPlanBeforeDispatch(PlannedStmt *plan);

static void
CdbDispatchUtilityStatement_Internal(struct Node *stmt,
									 bool needTwoPhase,
									 char *debugCaller);

static void
cdbdisp_dispatchSetCommandToAllGangs(const char *strCommand,
									 char *serializedQuerytree,
									 int serializedQuerytreelen,
									 char *serializedPlantree,
									 int serializedPlantreelen,
									 bool cancelOnError,
									 bool needTwoPhase,
									 struct CdbDispatcherState *ds);

static int
fillSliceVector(SliceTable *sliceTable,
				int sliceIndex,
				sliceVec *sliceVector,
				int len);

static char *
PQbuildGpQueryString(MemoryContext cxt,
					 DispatchCommandParms * pParms,
					 DispatchCommandQueryParms * pQueryParms,
					 int *finalLen);

static void
cdbdisp_queryParmsInit(struct CdbDispatcherState *ds,
					   DispatchCommandQueryParms * pQueryParms);

static void
cdbdisp_dispatchX(DispatchCommandQueryParms *pQueryParms,
				  bool cancelOnError,
				  struct SliceTable *sliceTbl,
				  struct CdbDispatcherState *ds);

/*
 * Compose and dispatch the MPPEXEC commands corresponding to a plan tree
 * within a complete parallel plan.  (A plan tree will correspond either
 * to an initPlan or to the main plan.)
 *
 * If cancelOnError is true, then any dispatching error, a cancellation
 * request from the client, or an error from any of the associated QEs,
 * may cause the unfinished portion of the plan to be abandoned or canceled;
 * and in the event this occurs before all gangs have been dispatched, this
 * function does not return, but waits for all QEs to stop and exits to
 * the caller's error catcher via ereport(ERROR,...).  Otherwise this
 * function returns normally and errors are not reported until later.
 *
 * If cancelOnError is false, the plan is to be dispatched as fully as
 * possible and the QEs allowed to proceed regardless of cancellation
 * requests, errors or connection failures from other QEs, etc.
 *
 * The CdbDispatchResults objects allocated for the plan are returned
 * in *pPrimaryResults.  The caller, after calling
 * CdbCheckDispatchResult(), can examine the CdbDispatchResults
 * objects, can keep them as long as needed, and ultimately must free
 * them with cdbdisp_destroyDispatcherState() prior to deallocation of
 * the caller's memory context.  Callers should use PG_TRY/PG_CATCH to
 * ensure proper cleanup.
 *
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 *
 * Note that the slice tree dispatched is the one specified in the EState
 * of the argument QueryDesc as es_cur__slice.
 *
 * Note that the QueryDesc params must include PARAM_EXEC_REMOTE parameters
 * containing the values of any initplans required by the slice to be run.
 * (This is handled by calls to addRemoteExecParamsToParamList() from the
 * functions preprocess_initplans() and ExecutorRun().)
 *
 * Each QE receives its assignment as a message of type 'M' in PostgresMain().
 * The message is deserialized and processed by exec_mpp_query() in postgres.c.
 */
void
cdbdisp_dispatchPlan(struct QueryDesc *queryDesc,
					 bool planRequiresTxn,
					 bool cancelOnError, struct CdbDispatcherState *ds)
{
	char *splan,
		 *ssliceinfo,
		 *sparams;

	int	splan_len,
		splan_len_uncompressed,
		ssliceinfo_len,
		sparams_len;

	SliceTable *sliceTbl;
	int rootIdx;
	int oldLocalSlice;
	PlannedStmt *stmt;
	bool is_SRI;

	DispatchCommandQueryParms queryParms;
	CdbComponentDatabaseInfo *qdinfo;

	ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;

	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(queryDesc != NULL && queryDesc->estate != NULL);

	/*
	 * Later we'll need to operate with the slice table provided via the
	 * EState structure in the argument QueryDesc.	Cache this information
	 * locally and assert our expectations about it.
	 */
	sliceTbl = queryDesc->estate->es_sliceTable;
	rootIdx = RootSliceIndex(queryDesc->estate);

	Assert(sliceTbl != NULL);
	Assert(rootIdx == 0 ||
		   (rootIdx > sliceTbl->nMotions
			&& rootIdx <= sliceTbl->nMotions + sliceTbl->nInitPlans));

	/*
	 * Keep old value so we can restore it.  We use this field as a parameter.
	 */
	oldLocalSlice = sliceTbl->localSlice;

	/*
	 * This function is called only for planned statements.
	 */
	stmt = queryDesc->plannedstmt;
	Assert(stmt);

	/*
	 * Let's evaluate STABLE functions now, so we get consistent values on the QEs
	 *
	 * Also, if this is a single-row INSERT statement, let's evaluate
	 * nextval() and currval() now, so that we get the QD's values, and a
	 * consistent value for everyone
	 *
	 */
	is_SRI = false;

	if (queryDesc->operation == CMD_INSERT)
	{
		Assert(stmt->commandType == CMD_INSERT);

		/*
		 * We might look for constant input relation (instead of SRI), but I'm afraid
		 * * that wouldn't scale.
		 */
		is_SRI = IsA(stmt->planTree, Result)
			&& stmt->planTree->lefttree == NULL;
	}

	if (!is_SRI)
		clear_relsize_cache();

	if (queryDesc->operation == CMD_INSERT ||
		queryDesc->operation == CMD_SELECT ||
		queryDesc->operation == CMD_UPDATE ||
		queryDesc->operation == CMD_DELETE)
	{

		MemoryContext oldContext;

		oldContext = CurrentMemoryContext;
		if (stmt->qdContext)
		{
			oldContext = MemoryContextSwitchTo(stmt->qdContext);
		}
		else
		/*
		 * memory context of plan tree should not change
		 */
		{
			MemoryContext mc = GetMemoryChunkContext(stmt->planTree);

			oldContext = MemoryContextSwitchTo(mc);
		}

		stmt->planTree = (Plan *) exec_make_plan_constant(stmt, is_SRI);

		MemoryContextSwitchTo(oldContext);
	}

	/*
	 * Cursor queries and bind/execute path queries don't run on the
	 * writer-gang QEs; but they require snapshot-synchronization to
	 * get started.
	 *
	 * initPlans, and other work (see the function pre-evaluation
	 * above) may advance the snapshot "segmateSync" value, so we're
	 * best off setting the shared-snapshot-ready value here. This
	 * will dispatch to the writer gang and force it to set its
	 * snapshot; we'll then be able to serialize the same snapshot
	 * version (see qdSerializeDtxContextInfo() below).
	 */
	if (queryDesc->extended_query)
	{
		verify_shared_snapshot_ready();
	}

	stripPlanBeforeDispatch(queryDesc->plannedstmt);

	/*
	 * serialized plan tree. Note that we're called for a single
	 * slice tree (corresponding to an initPlan or the main plan), so the
	 * parameters are fixed and we can include them in the prefix.
	 */
	splan =
		serializeNode((Node *) queryDesc->plannedstmt, &splan_len,
					  &splan_len_uncompressed);

	/*
	 * compute the total uncompressed size of the query plan for all slices
	 */
	int	num_slices =
		queryDesc->plannedstmt->planTree->nMotionNodes + 1;
	uint64 plan_size_in_kb =
		((uint64) splan_len_uncompressed * (uint64) num_slices) / (uint64) 1024;

	elog(((gp_log_gang >= GPVARS_VERBOSITY_VERBOSE) ? LOG : DEBUG1),
		 "Query plan size to dispatch: " UINT64_FORMAT "KB", plan_size_in_kb);

	if (0 < gp_max_plan_size && plan_size_in_kb > gp_max_plan_size)
	{
		ereport(ERROR,
				(errcode(ERRCODE_STATEMENT_TOO_COMPLEX),
				 (errmsg("Query plan size limit exceeded, current size: "
				   UINT64_FORMAT "KB, max allowed size: %dKB",
				   plan_size_in_kb, gp_max_plan_size),
				  errhint("Size controlled by gp_max_plan_size"))));
	}

	Assert(splan != NULL && splan_len > 0 && splan_len_uncompressed > 0);

	if (queryDesc->params != NULL && queryDesc->params->numParams > 0)
	{
		ParamListInfoData *pli;
		ParamExternData *pxd;
		StringInfoData parambuf;
		Size		length;
		int			plioff;
		int32		iparam;

		/*
		 * Allocate buffer for params
		 */
		initStringInfo(&parambuf);

		/*
		 * Copy ParamListInfoData header and ParamExternData array
		 */
		pli = queryDesc->params;
		length = (char *) &pli->params[pli->numParams] - (char *) pli;
		plioff = parambuf.len;
		Assert(plioff == MAXALIGN(plioff));
		appendBinaryStringInfo(&parambuf, pli, length);

		/*
		 * Copy pass-by-reference param values.
		 */
		for (iparam = 0; iparam < queryDesc->params->numParams; iparam++)
		{
			int16		typlen;
			bool		typbyval;

			/*
			 * Recompute pli each time in case parambuf.data is repalloc'ed 
			 */
			pli = (ParamListInfoData *) (parambuf.data + plioff);
			pxd = &pli->params[iparam];

			/*
			 * Does pxd->value contain the value itself, or a pointer?
			 */
			get_typlenbyval(pxd->ptype, &typlen, &typbyval);
			if (!typbyval)
			{
				char	   *s = DatumGetPointer(pxd->value);

				if (pxd->isnull || !PointerIsValid(s))
				{
					pxd->isnull = true;
					pxd->value = 0;
				}
				else
				{
					length = datumGetSize(pxd->value, typbyval, typlen);

					/*
					 * We *must* set this before we
					 * append. Appending may realloc, which will
					 * invalidate our pxd ptr. (obviously we could
					 * append first if we recalculate pxd from the new
					 * base address)
					 */
					pxd->value = Int32GetDatum(length);

					appendBinaryStringInfo(&parambuf, &iparam, sizeof(iparam));
					appendBinaryStringInfo(&parambuf, s, length);
				}
			}
		}
		sparams = parambuf.data;
		sparams_len = parambuf.len;
	}
	else
	{
		sparams = NULL;
		sparams_len = 0;
	}

	ssliceinfo =
		serializeNode((Node *) sliceTbl, &ssliceinfo_len,
					  NULL /*uncompressed_size */ );

	MemSet(&queryParms, 0, sizeof(queryParms));
	queryParms.strCommand = queryDesc->sourceText;
	queryParms.serializedQuerytree = NULL;
	queryParms.serializedQuerytreelen = 0;
	queryParms.serializedPlantree = splan;
	queryParms.serializedPlantreelen = splan_len;
	queryParms.serializedParams = sparams;
	queryParms.serializedParamslen = sparams_len;
	queryParms.serializedSliceInfo = ssliceinfo;
	queryParms.serializedSliceInfolen = ssliceinfo_len;
	queryParms.rootIdx = rootIdx;

	/*
	 * sequence server info
	 */
	qdinfo = &(getComponentDatabases()->entry_db_info[0]);
	Assert(qdinfo != NULL && qdinfo->hostip != NULL);
	queryParms.seqServerHost = pstrdup(qdinfo->hostip);
	queryParms.seqServerHostlen = strlen(qdinfo->hostip) + 1;
	queryParms.seqServerPort = seqServerCtl->seqServerPort;

	queryParms.primary_gang_id = 0;		/* We are relying on the slice table to provide gang ids */

	/*
	 * serialized a version of our snapshot
	 */
	/*
	 * Generate our transction isolations.	We generally want Plan
	 * based dispatch to be in a global transaction. The executor gets
	 * to decide if the special circumstances exist which allow us to
	 * dispatch without starting a global xact.
	 */
	queryParms.serializedDtxContextInfo =
		qdSerializeDtxContextInfo(&queryParms.serializedDtxContextInfolen,
								  true /* wantSnapshot */ ,
								  queryDesc->extended_query,
								  mppTxnOptions(planRequiresTxn),
								  "cdbdisp_dispatchPlan");

	Assert(sliceTbl);
	Assert(sliceTbl->slices != NIL);

	cdbdisp_dispatchX(&queryParms, cancelOnError, sliceTbl, ds);

	sliceTbl->localSlice = oldLocalSlice;
}

/*
 * Special for sending SET commands that change GUC variables, so they go to all
 * gangs, both reader and writer
 */
void
CdbSetGucOnAllGangs(const char *strCommand,
					bool cancelOnError, bool needTwoPhase)
{
	volatile CdbDispatcherState ds = { NULL, NULL };
	const bool	withSnapshot = true;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),
		 "CdbSetGucOnAllGangs for command = '%s', needTwoPhase = %s",
		 strCommand, (needTwoPhase ? "true" : "false"));

	dtmPreCommand("CdbSetGucOnAllGangs", strCommand, NULL, needTwoPhase,
				  withSnapshot, false /* inCursor */ );

	PG_TRY();
	{
		cdbdisp_dispatchSetCommandToAllGangs(strCommand, NULL, 0, NULL, 0,
											 cancelOnError, needTwoPhase,
											 (struct CdbDispatcherState *)
											 &ds);

		/*
		 * Wait for all QEs to finish. If not all of our QEs were successful,
		 * report the error and throw up.
		 */
		cdbdisp_finishCommand((struct CdbDispatcherState *) &ds, NULL, NULL);
	}
	PG_CATCH();
	{
		/*
		 * Something happend, clean up after ourselves
		 */
		CdbCheckDispatchResult((struct CdbDispatcherState *) &ds,
							   DISPATCH_WAIT_CANCEL);

		cdbdisp_destroyDispatcherState((struct CdbDispatcherState *) &ds);

		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * cdbdisp_dispatchCommand:
 * Send the strCommand SQL statement to all segdbs in the cluster
 * cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during gang creation.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
 *
 * The CdbDispatchResults objects allocated for the command
 * are returned in *pPrimaryResults
 * The caller, after calling CdbCheckDispatchResult(), can
 * examine the CdbDispatchResults objects, can keep them as
 * long as needed, and ultimately must free them with
 * cdbdisp_destroyDispatcherState() prior to deallocation
 * of the memory context from which they were allocated.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatcherState() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchCommand(const char *strCommand,
						char *serializedQuerytree,
						int serializedQuerytreelen,
						bool cancelOnError,
						bool needTwoPhase,
						bool withSnapshot, CdbDispatcherState * ds)
{
	DispatchCommandQueryParms queryParms;
	Gang *primaryGang;
	int	nsegdb = getgpsegmentCount();
	CdbComponentDatabaseInfo *qdinfo;

	if (log_dispatch_stats)
		ResetUsage();

	if (DEBUG5 >= log_min_messages)
		elog(DEBUG3, "cdbdisp_dispatchCommand: %s (needTwoPhase = %s)",
			 strCommand, (needTwoPhase ? "true" : "false"));
	else
		elog((Debug_print_full_dtm ? LOG : DEBUG3),
			 "cdbdisp_dispatchCommand: %.50s (needTwoPhase = %s)", strCommand,
			 (needTwoPhase ? "true" : "false"));

	MemSet(&queryParms, 0, sizeof(queryParms));
	queryParms.strCommand = strCommand;
	queryParms.serializedQuerytree = serializedQuerytree;
	queryParms.serializedQuerytreelen = serializedQuerytreelen;

	/*
	 * Allocate a primary QE for every available segDB in the system.
	 */
	primaryGang = allocateWriterGang();

	Assert(primaryGang);

	queryParms.primary_gang_id = primaryGang->gang_id;

	/*
	 * Serialize a version of our DTX Context Info
	 */
	queryParms.serializedDtxContextInfo =
		qdSerializeDtxContextInfo(&queryParms.serializedDtxContextInfolen,
								  withSnapshot, false,
								  mppTxnOptions(needTwoPhase),
								  "cdbdisp_dispatchCommand");

	/*
	 * sequence server info
	 */
	qdinfo = &(getComponentDatabases()->entry_db_info[0]);
	Assert(qdinfo != NULL && qdinfo->hostip != NULL);
	queryParms.seqServerHost = pstrdup(qdinfo->hostip);
	queryParms.seqServerHostlen = strlen(qdinfo->hostip) + 1;
	queryParms.seqServerPort = seqServerCtl->seqServerPort;

	/*
	 * Dispatch the command.
	 */
	ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;
	cdbdisp_makeDispatcherState(ds, nsegdb, 0, cancelOnError);
	cdbdisp_queryParmsInit(ds, &queryParms);
	ds->primaryResults->writer_gang = primaryGang;

	cdbdisp_dispatchToGang(ds, primaryGang, -1, DEFAULT_DISP_DIRECT);

	/*
	 * don't pfree serializedShapshot here, it will be pfree'd when
	 * the first thread is destroyed.
	 */
}

/*
 * CdbDoCommand:
 * Combination of cdbdisp_dispatchCommand and cdbdisp_finishCommand.
 * If not all QEs execute the command successfully, throws an error and
 * does not return.
 *
 * needTwoPhase specifies a desire to include global transaction control
 * before dispatch.
 */
void
CdbDoCommand(const char *strCommand, bool cancelOnError, bool needTwoPhase)
{
	CdbDispatcherState ds = {NULL, NULL, NULL};
	const bool withSnapshot = true;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),
		 "CdbDoCommand for command = '%s', needTwoPhase = %s", strCommand,
		 (needTwoPhase ? "true" : "false"));

	dtmPreCommand("CdbDoCommand", strCommand, NULL, needTwoPhase, withSnapshot,
				  false /* inCursor */ );

	cdbdisp_dispatchCommand(strCommand, NULL, 0, cancelOnError, needTwoPhase,
							true, &ds);

	/*
	 * Wait for all QEs to finish. If not all of our QEs were successful,
	 * report the error and throw up.
	 */
	cdbdisp_finishCommand(&ds, NULL, NULL);
}

/*
 * Dispatch a command - already parsed and in the form of a Node tree
 * - to all primary segdbs.  Does not wait for completion. Does not
 * start a global transaction.
 *
 * NB: Callers should use PG_TRY()/PG_CATCH() if needed to make
 * certain that the CdbDispatchResults objects are destroyed by
 * cdbdisp_destroyDispatcherState() in case of error.
 * To wait for completion, check for errors, and clean up, it is
 * suggested that the caller use cdbdisp_finishCommand().
 */
void
cdbdisp_dispatchUtilityStatement(struct Node *stmt,
								 bool cancelOnError,
								 bool needTwoPhase,
								 bool withSnapshot,
								 struct CdbDispatcherState *ds,
								 char *debugCaller)
{
	char *serializedQuerytree;
	int	serializedQuerytree_len;
	Query *q = makeNode(Query);
	StringInfoData buffer;

	elog((Debug_print_full_dtm ? LOG : DEBUG5),
		 "cdbdisp_dispatchUtilityStatement debug_query_string = %s (needTwoPhase = %s, debugCaller = %s)",
		 debug_query_string, (needTwoPhase ? "true" : "false"), debugCaller);

	dtmPreCommand("cdbdisp_dispatchUtilityStatement", "(none)", NULL,
				  needTwoPhase, withSnapshot, false /* inCursor */ );

	initStringInfo(&buffer);

	q->commandType = CMD_UTILITY;

	Assert(stmt != NULL);
	Assert(stmt->type < 1000);
	Assert(stmt->type > 0);

	q->utilityStmt = stmt;

	q->querySource = QSRC_ORIGINAL;

	/*
	 * We must set q->canSetTag = true.  False would be used to hide a command
	 * introduced by rule expansion which is not allowed to return its
	 * completion status in the command tag (PQcmdStatus/PQcmdTuples). For
	 * example, if the original unexpanded command was SELECT, the status
	 * should come back as "SELECT n" and should not reflect other commands
	 * inserted by rewrite rules.  True means we want the status.
	 */
	q->canSetTag = true;		/* ? */

	/*
	 * serialized the stmt tree, and create the sql statement: mppexec ....
	 */
	serializedQuerytree =
		serializeNode((Node *) q, &serializedQuerytree_len,
					  NULL /*uncompressed_size */);

	Assert(serializedQuerytree != NULL);

	cdbdisp_dispatchCommand(debug_query_string, serializedQuerytree,
							serializedQuerytree_len, cancelOnError,
							needTwoPhase, withSnapshot, ds);
}

/*
 * cdbdisp_dispatchRMCommand:
 * Sends a non-cancelable command to all segment dbs.
 *
 * Returns a malloc'ed array containing the PGresult objects thus
 * produced; the caller must PQclear() them and free() the array.
 * A NULL entry follows the last used entry in the array.
 *
 * Any error messages - whether or not they are associated with
 * PGresult objects - are appended to a StringInfo buffer provided
 * by the caller.
 */
struct pg_result **
cdbdisp_dispatchRMCommand(const char *strCommand,
						  bool withSnapshot,
						  StringInfo errmsgbuf,
						  int *numresults)
{
	volatile struct CdbDispatcherState ds = {NULL, NULL, NULL};

	PGresult  **resultSets = NULL;

	/*
	 * never want to start a global transaction for these
	 */
	bool needTwoPhase = false;

	elog(((Debug_print_full_dtm || Debug_print_snapshot_dtm) ? LOG : DEBUG5),
		 "cdbdisp_dispatchRMCommand for command = '%s', withSnapshot = %s",
		 strCommand, (withSnapshot ? "true" : "false"));

	PG_TRY();
	{
		/*
		 * Launch the command.	Don't cancel on error. 
		 */
		cdbdisp_dispatchCommand(strCommand, NULL, 0,
								false, /* cancelOnError */
								needTwoPhase, withSnapshot,
								(struct CdbDispatcherState *) &ds);

		/*
		 * Wait for all QEs to finish.	Don't cancel. 
		 */
		CdbCheckDispatchResult((struct CdbDispatcherState *) &ds,
							   DISPATCH_WAIT_NONE);
	}
	PG_CATCH();
	{
		/*
		 * Something happend, clean up after ourselves
		 */
		CdbCheckDispatchResult((struct CdbDispatcherState *) &ds,
							   DISPATCH_WAIT_NONE);

		cdbdisp_destroyDispatcherState((struct CdbDispatcherState *) &ds);
		PG_RE_THROW();
	}
	PG_END_TRY();

	resultSets =
		cdbdisp_returnResults(ds.primaryResults, errmsgbuf, numresults);

	cdbdisp_destroyDispatcherState((struct CdbDispatcherState *) &ds);

	return resultSets;
}

/*
 * Dispatch a command - already parsed and in the form of a Node tree
 * - to all primary segdbs, and wait for completion.  Starts a global
 * transaction first, if not already started.  If not all QEs in the
 * given gang(s) executed the command successfully, throws an error
 * and does not return.
 */
void
CdbDispatchUtilityStatement(struct Node *stmt,
							char *debugCaller __attribute__ ((unused)))
{
	CdbDispatchUtilityStatement_Internal(stmt,
										 true,
										 "CdbDispatchUtilityStatement");
}

void
CdbDispatchUtilityStatement_NoTwoPhase(struct Node *stmt,
									   char *debugCaller __attribute__ ((unused)))
{
	CdbDispatchUtilityStatement_Internal(stmt,
										 false,
										 "CdbDispatchUtilityStatement_NoTwoPhase");
}

/*
 * Remove subquery field in RTE's with subquery kind. This is an optimization used
 * to reduce plan size before serialization for dispatching.
 */
static void
stripPlanBeforeDispatch(PlannedStmt *plan)
{
	remove_subquery_in_RTEs((Node *) plan->rtable);
}

/*
 * Initialize CdbDispatcherState using DispatchCommandQueryParms
 *
 * Allocate query text in memory context, initialize it and assign it to
 * all DispatchCommandQueryParms in this dispatcher state.
 *
 * For now, there's only one field (localSlice) which is different to each
 * dispatcher thread, we set it later.
 *
 * Also, we free the DispatchCommandQueryParms memory.
 */
static void
cdbdisp_queryParmsInit(struct CdbDispatcherState *ds,
					   DispatchCommandQueryParms *pQueryParms)
{
	int	i = 0;
	int	len = 0;

	CdbDispatchCmdThreads *dThreads = ds->dispatchThreads;
	DispatchCommandParms *pParms = &dThreads->dispatchCommandParmsAr[0];

	Assert(pQueryParms->strCommand != NULL);

	char *queryText =
		PQbuildGpQueryString(ds->dispatchStateContext, pParms, pQueryParms,
							 &len);

	if (pQueryParms->serializedQuerytree != NULL)
	{
		pfree(pQueryParms->serializedQuerytree);
		pQueryParms->serializedQuerytree = NULL;
	}

	if (pQueryParms->serializedPlantree != NULL)
	{
		pfree(pQueryParms->serializedPlantree);
		pQueryParms->serializedPlantree = NULL;
	}

	if (pQueryParms->serializedParams != NULL)
	{
		pfree(pQueryParms->serializedParams);
		pQueryParms->serializedParams = NULL;
	}

	if (pQueryParms->serializedSliceInfo != NULL)
	{
		pfree(pQueryParms->serializedSliceInfo);
		pQueryParms->serializedSliceInfo = NULL;
	}

	if (pQueryParms->serializedDtxContextInfo != NULL)
	{
		pfree(pQueryParms->serializedDtxContextInfo);
		pQueryParms->serializedDtxContextInfo = NULL;
	}

	if (pQueryParms->seqServerHost != NULL)
	{
		pfree(pQueryParms->seqServerHost);
		pQueryParms->seqServerHost = NULL;
	}

	for (i = 0; i < dThreads->dispatchCommandParmsArSize; i++)
	{
		pParms = &dThreads->dispatchCommandParmsAr[i];
		pParms->query_text = queryText;
		pParms->query_text_len = len;
	}
}

/*
 * Three Helper functions for CdbDispatchPlan:
 *
 * Used to figure out the dispatch order for the sliceTable by
 * counting the number of dependent child slices for each slice; and
 * then sorting based on the count (all indepenedent slices get
 * dispatched first, then the slice above them and so on).
 *
 * fillSliceVector: figure out the number of slices we're dispatching,
 * and order them.
 *
 * count_dependent_children(): walk tree counting up children.
 *
 * compare_slice_order(): comparison function for qsort(): order the
 * slices by the number of dependent children. Empty slices are
 * sorted last (to make this work with initPlans).
 *
 */
static int
compare_slice_order(const void *aa, const void *bb)
{
	sliceVec *a = (sliceVec *) aa;
	sliceVec *b = (sliceVec *) bb;

	if (a->slice == NULL)
		return 1;
	if (b->slice == NULL)
		return -1;

	/*
	 * sort the writer gang slice first, because he sets the shared snapshot
	 */
	if (a->slice->primary_gang_id == 1 && b->slice->primary_gang_id != 1)
		return -1;
	else if (b->slice->primary_gang_id == 1 && a->slice->primary_gang_id != 1)
		return 1;

	if (a->children == b->children)
		return 0;
	else if (a->children > b->children)
		return 1;
	else
		return -1;
}

/*
 * Quick and dirty bit mask operations
 */
static void
mark_bit(char *bits, int nth)
{
	int	nthbyte = nth >> 3;
	char nthbit = 1 << (nth & 7);

	bits[nthbyte] |= nthbit;
}

static void
or_bits(char *dest, char *src, int n)
{
	int	i;

	for (i = 0; i < n; i++)
		dest[i] |= src[i];
}

static int
count_bits(char *bits, int nbyte)
{
	int	i;
	int	nbit = 0;

	int	bitcount[] =
	{
		0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
	};

	for (i = 0; i < nbyte; i++)
	{
		nbit += bitcount[bits[i] & 0x0F];
		nbit += bitcount[(bits[i] >> 4) & 0x0F];
	}

	return nbit;
}

/*
 * We use a bitmask to count the dep. childrens.
 * Because of input sharing, the slices now are DAG.  We cannot simply go down the
 * tree and add up number of children, which will return too big number.
 */
static int
markbit_dep_children(SliceTable * sliceTable, int sliceIdx,
					 sliceVec * sliceVec, int bitmasklen, char *bits)
{
	ListCell *sublist;
	Slice *slice = (Slice *) list_nth(sliceTable->slices, sliceIdx);

	foreach(sublist, slice->children)
	{
		int	childIndex = lfirst_int(sublist);
		char *newbits = palloc0(bitmasklen);

		markbit_dep_children(sliceTable, childIndex,
							 sliceVec, bitmasklen, newbits);
		or_bits(bits, newbits, bitmasklen);
		mark_bit(bits, childIndex);
		pfree(newbits);
	}

	sliceVec[sliceIdx].sliceIndex = sliceIdx;
	sliceVec[sliceIdx].children = count_bits(bits, bitmasklen);
	sliceVec[sliceIdx].slice = slice;

	return sliceVec[sliceIdx].children;
}

/*
 * Count how many dependent childrens and fill in the sliceVector of dependent childrens.
 */
static int
count_dependent_children(SliceTable * sliceTable, int sliceIndex,
						 sliceVec * sliceVector, int len)
{
	int	ret = 0;
	int	bitmasklen = (len + 7) >> 3;
	char *bitmask = palloc0(bitmasklen);

	ret = markbit_dep_children(sliceTable, sliceIndex,
							   sliceVector, bitmasklen, bitmask);
	pfree(bitmask);

	return ret;
}

static int
fillSliceVector(SliceTable *sliceTbl, int rootIdx,
				sliceVec *sliceVector, int sliceLim)
{
	int top_count;

	/*
	 * count doesn't include top slice add 1, note that sliceVector would be
	 * modified in place by count_dependent_children.
	 */
	top_count =
		1 + count_dependent_children(sliceTbl, rootIdx, sliceVector, sliceLim);

	qsort(sliceVector, sliceLim, sizeof(sliceVec), compare_slice_order);

	return top_count;
}

/*
 * Special Greenplum-only method for executing SQL statements.	Specifies a global
 * transaction context that the statement should be executed within.
 *
 * This should *ONLY* ever be used by the Greenplum Database Query Dispatcher and NEVER (EVER)
 * by anyone else.
 *
 * snapshot - serialized form of Snapshot data.
 * xid		- Global Transaction Id to use.
 * flags	- specifies additional processing instructions to the remote server.
 *			  e.g.	Auto explicitly start a transaction before executing this
 *			  statement.
 * gp_command_count - Client request serial# to be passed to the qExec.
 */
static char *
PQbuildGpQueryString(MemoryContext cxt, DispatchCommandParms * pParms,
					 DispatchCommandQueryParms * pQueryParms, int *finalLen)
{
	const char *command = pQueryParms->strCommand;
	int command_len = strlen(pQueryParms->strCommand) + 1;
	const char *querytree = pQueryParms->serializedQuerytree;
	int querytree_len = pQueryParms->serializedQuerytreelen;
	const char *plantree = pQueryParms->serializedPlantree;
	int	plantree_len = pQueryParms->serializedPlantreelen;
	const char *params = pQueryParms->serializedParams;
	int	params_len = pQueryParms->serializedParamslen;
	const char *sliceinfo = pQueryParms->serializedSliceInfo;
	int	sliceinfo_len = pQueryParms->serializedSliceInfolen;
	const char *snapshotInfo = pQueryParms->serializedDtxContextInfo;
	int	snapshotInfo_len = pQueryParms->serializedDtxContextInfolen;
	int	flags = 0;		/* unused flags */
	int	localSlice = 0; /* localSlice; place holder; set later in dupQueryTextAndSetSliceId */
	int	rootIdx = pQueryParms->rootIdx;
	const char *seqServerHost = pQueryParms->seqServerHost;
	int	seqServerHostlen = pQueryParms->seqServerHostlen;
	int	seqServerPort = pQueryParms->seqServerPort;
	int	primary_gang_id = pQueryParms->primary_gang_id;
	int64 currentStatementStartTimestamp = GetCurrentStatementStartTimestamp();
	Oid	sessionUserId = GetSessionUserId();
	Oid	outerUserId = GetOuterUserId();
	Oid	currentUserId = GetUserId();
	bool sessionUserIsSuper = superuser_arg(GetSessionUserId());
	bool outerUserIsSuper = superuser_arg(GetSessionUserId());

	int	tmp,
		len;
	uint32 n32;
	int	total_query_len;
	char *shared_query,
		 *pos;
	char one = 1;
	char zero = 0;

	total_query_len = 1 /* 'M' */  +
		sizeof(len) /* message length */  +
		sizeof(gp_command_count) +
		sizeof(sessionUserId) + 1 /* sessionUserIsSuper */	+
		sizeof(outerUserId) + 1 /* outerUserIsSuper */	+
		sizeof(currentUserId) +
		sizeof(localSlice) +
		sizeof(rootIdx) +
		sizeof(primary_gang_id) +
		sizeof(n32) * 2 /* currentStatementStartTimestamp */  +
		sizeof(command_len) +
		sizeof(querytree_len) +
		sizeof(plantree_len) +
		sizeof(params_len) +
		sizeof(sliceinfo_len) +
		sizeof(snapshotInfo_len) +
		snapshotInfo_len +
		sizeof(flags) +
		sizeof(seqServerHostlen) +
		sizeof(seqServerPort) +
		command_len +
		querytree_len +
		plantree_len + params_len + sliceinfo_len + seqServerHostlen;

	shared_query = MemoryContextAlloc(cxt, total_query_len);

	pos = shared_query;

	*pos++ = 'M';

	pos += 4;					/* place holder for message length */

	tmp = htonl(localSlice);
	memcpy(pos, &tmp, sizeof(localSlice));
	pos += sizeof(localSlice);

	tmp = htonl(gp_command_count);
	memcpy(pos, &tmp, sizeof(gp_command_count));
	pos += sizeof(gp_command_count);

	tmp = htonl(sessionUserId);
	memcpy(pos, &tmp, sizeof(sessionUserId));
	pos += sizeof(sessionUserId);

	if (sessionUserIsSuper)
		*pos++ = one;
	else
		*pos++ = zero;


	tmp = htonl(outerUserId);
	memcpy(pos, &tmp, sizeof(outerUserId));
	pos += sizeof(outerUserId);

	if (outerUserIsSuper)
		*pos++ = one;
	else
		*pos++ = zero;

	tmp = htonl(currentUserId);
	memcpy(pos, &tmp, sizeof(currentUserId));
	pos += sizeof(currentUserId);

	tmp = htonl(rootIdx);
	memcpy(pos, &tmp, sizeof(rootIdx));
	pos += sizeof(rootIdx);

	tmp = htonl(primary_gang_id);
	memcpy(pos, &tmp, sizeof(primary_gang_id));
	pos += sizeof(primary_gang_id);

	/*
	 * High order half first, since we're doing MSB-first 
	 */
	n32 = (uint32) (currentStatementStartTimestamp >> 32);
	n32 = htonl(n32);
	memcpy(pos, &n32, sizeof(n32));
	pos += sizeof(n32);

	/*
	 * Now the low order half
	 */
	n32 = (uint32) currentStatementStartTimestamp;
	n32 = htonl(n32);
	memcpy(pos, &n32, sizeof(n32));
	pos += sizeof(n32);

	tmp = htonl(command_len);
	memcpy(pos, &tmp, sizeof(command_len));
	pos += sizeof(command_len);

	tmp = htonl(querytree_len);
	memcpy(pos, &tmp, sizeof(querytree_len));
	pos += sizeof(querytree_len);

	tmp = htonl(plantree_len);
	memcpy(pos, &tmp, sizeof(plantree_len));
	pos += sizeof(plantree_len);

	tmp = htonl(params_len);
	memcpy(pos, &tmp, sizeof(params_len));
	pos += sizeof(params_len);

	tmp = htonl(sliceinfo_len);
	memcpy(pos, &tmp, sizeof(tmp));
	pos += sizeof(tmp);

	tmp = htonl(snapshotInfo_len);
	memcpy(pos, &tmp, sizeof(tmp));
	pos += sizeof(tmp);

	if (snapshotInfo_len > 0)
	{
		memcpy(pos, snapshotInfo, snapshotInfo_len);
		pos += snapshotInfo_len;
	}

	tmp = htonl(flags);
	memcpy(pos, &tmp, sizeof(tmp));
	pos += sizeof(tmp);

	tmp = htonl(seqServerHostlen);
	memcpy(pos, &tmp, sizeof(tmp));
	pos += sizeof(tmp);

	tmp = htonl(seqServerPort);
	memcpy(pos, &tmp, sizeof(tmp));
	pos += sizeof(tmp);

	if (command_len > 0)
	{
		memcpy(pos, command, command_len);
		pos += command_len;
	}

	if (querytree_len > 0)
	{
		memcpy(pos, querytree, querytree_len);
		pos += querytree_len;
	}

	if (plantree_len > 0)
	{
		memcpy(pos, plantree, plantree_len);
		pos += plantree_len;
	}

	if (params_len > 0)
	{
		memcpy(pos, params, params_len);
		pos += params_len;
	}

	if (sliceinfo_len > 0)
	{
		memcpy(pos, sliceinfo, sliceinfo_len);
		pos += sliceinfo_len;
	}

	if (seqServerHostlen > 0)
	{
		memcpy(pos, seqServerHost, seqServerHostlen);
		pos += seqServerHostlen;
	}

	len = pos - shared_query - 1;

	/*
	 * fill in length placeholder
	 */
	tmp = htonl(len);
	memcpy(shared_query + 1, &tmp, sizeof(len));

	Assert(len + 1 == total_query_len);

	if (finalLen)
		*finalLen = len + 1;

	return shared_query;
}

/*
 * This code was refactored out of cdbdisp_dispatchPlan.  It's
 * used both for dispatching plans when we are using normal gangs,
 * and for dispatching all statements from Query Dispatch Agents
 * when we are using dispatch agents.
 */
static void
cdbdisp_dispatchX(DispatchCommandQueryParms *pQueryParms,
				  bool cancelOnError,
				  struct SliceTable *sliceTbl,
				  struct CdbDispatcherState *ds)
{
	int			oldLocalSlice = 0;
	sliceVec   *sliceVector = NULL;
	int			nSlices = 1;
	int			sliceLim = 1;
	int			iSlice;
	int			rootIdx = pQueryParms->rootIdx;

	if (log_dispatch_stats)
		ResetUsage();

	Assert(Gp_role == GP_ROLE_DISPATCH);

	if (sliceTbl)
	{
		Assert(rootIdx == 0 ||
			   (rootIdx > sliceTbl->nMotions &&
				rootIdx <= sliceTbl->nMotions + sliceTbl->nInitPlans));

		/*
		 * Keep old value so we can restore it.  We use this field as a parameter.
		 */
		oldLocalSlice = sliceTbl->localSlice;

		/*
		 * Traverse the slice tree in sliceTbl rooted at rootIdx and build a
		 * vector of slice indexes specifying the order of [potential] dispatch.
		 */
		sliceLim = list_length(sliceTbl->slices);
		sliceVector = palloc0(sliceLim * sizeof(sliceVec));

		nSlices = fillSliceVector(sliceTbl, rootIdx, sliceVector, sliceLim);
	}

	/*
	 * Allocate result array with enough slots for QEs of primary gangs.
	 */
	ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;

	cdbdisp_makeDispatcherState(ds, nSlices * largestGangsize(), sliceLim,
								cancelOnError);
	cdbdisp_queryParmsInit(ds, pQueryParms);

	cdb_total_plans++;
	cdb_total_slices += nSlices;
	if (nSlices > cdb_max_slices)
		cdb_max_slices = nSlices;

	/*
	 * must have somebody to dispatch to.
	 */
	Assert(sliceTbl != NULL || pQueryParms->primary_gang_id > 0);

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,
						(errmsg
						 ("duration to start of dispatch send (root %d): %s ms",
						  pQueryParms->rootIdx, msec_str)));
				break;
		}
	}

	/*
	 * Now we need to call CDBDispatchCommand once per slice.  Each such
	 * call dispatches a MPPEXEC command to each of the QEs assigned to
	 * the slice.
	 *
	 * The QE information goes in two places: (1) in the argument to the
	 * function CDBDispatchCommand, and (2) in the serialized
	 * command sent to the QEs.
	 *
	 * So, for each slice in the tree...
	 */
	for (iSlice = 0; iSlice < nSlices; iSlice++)
	{
		CdbDispatchDirectDesc direct;
		Gang	   *primaryGang = NULL;
		Slice	   *slice = NULL;
		int			si = -1;

		if (sliceVector)
		{
			/*
			 * Sliced dispatch, and we are either the dispatch agent, or we are
			 * the QD and are not using Dispatch Agents.
			 *
			 * So, dispatch to each slice.
			 */
			slice = sliceVector[iSlice].slice;
			si = slice->sliceIndex;

			/*
			 * Is this a slice we should dispatch?
			 */
			if (slice && slice->gangType == GANGTYPE_UNALLOCATED)
			{
				/*
				 * Most slices are dispatched, however, in many  cases the
				 * root runs only on the QD and is not dispatched to the QEs.
				 */
				continue;
			}

			primaryGang = slice->primaryGang;

			/*
			 * If we are on the dispatch agent, the gang pointers aren't filled in.
			 * We must look them up by ID
			 */
			if (primaryGang == NULL)
			{
				elog(DEBUG2, "Dispatch %d, Gangs are %d, type=%d", iSlice,
					 slice->primary_gang_id, slice->gangType);
				primaryGang = findGangById(slice->primary_gang_id);

				Assert(primaryGang != NULL);
				if (primaryGang != NULL)
					Assert(primaryGang->type == slice->gangType
						   || primaryGang->type == GANGTYPE_PRIMARY_WRITER);

				if (primaryGang == NULL)
					continue;
			}

			if (slice->directDispatch.isDirectDispatch)
			{
				direct.directed_dispatch = true;
				direct.count = list_length(slice->directDispatch.contentIds);

				/*
				 * only support single content right now.  If this changes then we need to change from
				 * a list to another structure to avoid n^2 cases
				 */
				Assert(direct.count == 1);
				direct.content[0] =
					linitial_int(slice->directDispatch.contentIds);

				if (Test_print_direct_dispatch_info)
				{
					elog(INFO, "Dispatch command to SINGLE content");
				}
			}
			else
			{
				direct.directed_dispatch = false;
				direct.count = 0;

				if (Test_print_direct_dispatch_info)
				{
					elog(INFO, "Dispatch command to ALL contents");
				}
			}
		}
		else
		{
			direct.directed_dispatch = false;
			direct.count = 0;

			if (Test_print_direct_dispatch_info)
			{
				elog(INFO, "Dispatch command to ALL contents");
			}

			/*
			 *	Non-sliced, used specified gangs
			 */
			elog(DEBUG2, "primary %d", pQueryParms->primary_gang_id);
			if (pQueryParms->primary_gang_id > 0)
				primaryGang = findGangById(pQueryParms->primary_gang_id);
		}

		/*
		 * Must have a gang to dispatch to
		 */
		Assert(primaryGang != NULL);	
		if (primaryGang)
		{
			Assert(ds->primaryResults && ds->primaryResults->resultArray);
		}

		/*
		 * Bail out if already got an error or cancellation request.
		 */
		if (cancelOnError)
		{
			if (ds->primaryResults->errcode)
				break;
			if (InterruptPending)
				break;
		}

		/*
		 * Dispatch the plan to our primaryGang.
		 * Doesn't wait for it to finish.
		 */
		if (primaryGang != NULL)
		{
			if (primaryGang->type == GANGTYPE_PRIMARY_WRITER)
				ds->primaryResults->writer_gang = primaryGang;

			cdbdisp_dispatchToGang(ds, primaryGang, si, &direct);
		}
	}

	if (sliceVector)
		pfree(sliceVector);

	if (sliceTbl)
		sliceTbl->localSlice = oldLocalSlice;

	/*
	 * If bailed before completely dispatched, stop QEs and throw error.
	 */
	if (iSlice < nSlices)
	{
		elog(Debug_cancel_print ? LOG : DEBUG2,
			 "Plan dispatch canceled; dispatched %d of %d slices", iSlice,
			 nSlices);

		/*
		 * Cancel any QEs still running, and wait for them to terminate.
		 */
		CdbCheckDispatchResult(ds, DISPATCH_WAIT_CANCEL);

		/*
		 * Check and free the results of all gangs. If any QE had an
		 * error, report it and exit via PG_THROW.
		 */
		cdbdisp_finishCommand(ds, NULL, NULL);

		/*
		 * Wasn't an error, must have been an interrupt. 
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Strange!  Not an interrupt either.
		 */
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg_internal("Unable to dispatch plan.")));
	}

	if (DEBUG1 >= log_min_messages)
	{
		char		msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,
						(errmsg
						 ("duration to dispatch out (root %d): %s ms",
						  pQueryParms->rootIdx, msec_str)));
				break;
		}
	}
}

/*
 * Dispatch SET command to all gangs.
 *
 * Can not dispatch SET commands to busy reader gangs (allocated by cursors) directly because another command is already in progress.
 * Cursors only allocate reader gangs, so primary writer and idle reader gangs can be dispatched to.
 */
static void
cdbdisp_dispatchSetCommandToAllGangs(const char *strCommand,
									 char *serializedQuerytree,
									 int serializedQuerytreelen,
									 char *serializedPlantree,
									 int serializedPlantreelen,
									 bool cancelOnError,
									 bool needTwoPhase,
									 struct CdbDispatcherState *ds)
{
	DispatchCommandQueryParms queryParms;

	Gang *primaryGang;
	List *idleReaderGangs;
	List *busyReaderGangs;
	ListCell *le;

	int nsegdb = getgpsegmentCount();
	int	gangCount;

	MemSet(&queryParms, 0, sizeof(queryParms));
	queryParms.strCommand = strCommand;
	queryParms.serializedQuerytree = serializedQuerytree;
	queryParms.serializedQuerytreelen = serializedQuerytreelen;
	queryParms.serializedPlantree = serializedPlantree;
	queryParms.serializedPlantreelen = serializedPlantreelen;

	/*
	 * Allocate a primary QE for every available segDB in the system.
	 */
	primaryGang = allocateWriterGang();

	Assert(primaryGang);

	queryParms.primary_gang_id = primaryGang->gang_id;

	/*
	 * serialized a version of our snapshot
	 */
	queryParms.serializedDtxContextInfo =
		qdSerializeDtxContextInfo(&queryParms.serializedDtxContextInfolen,
								  true /* withSnapshot */ ,
								  false /* cursor */ ,
								  mppTxnOptions(needTwoPhase),
								  "cdbdisp_dispatchSetCommandToAllGangs");

	idleReaderGangs = getAllIdleReaderGangs();
	busyReaderGangs = getAllBusyReaderGangs();

	/*
	 * Dispatch the command.
	 */
	gangCount = 1 + list_length(idleReaderGangs);

	ds->primaryResults = NULL;
	ds->dispatchThreads = NULL;
	cdbdisp_makeDispatcherState(ds, nsegdb * gangCount, gangCount, cancelOnError);
	cdbdisp_queryParmsInit(ds, &queryParms);
	ds->primaryResults->writer_gang = primaryGang;

	cdbdisp_dispatchToGang(ds, primaryGang, -1, DEFAULT_DISP_DIRECT);

	foreach(le, idleReaderGangs)
	{
		Gang *rg = lfirst(le);

		cdbdisp_dispatchToGang(ds, rg, -1, DEFAULT_DISP_DIRECT);
	}

	/*
	 *Can not send set command to busy gangs, so those gangs
	 *can not be reused because their GUC is not set.
	 */
	foreach(le, busyReaderGangs)
	{
		Gang *rg = lfirst(le);

		rg->noReuse = true;
	}
}
	
static void
CdbDispatchUtilityStatement_Internal(struct Node *stmt,
									 bool needTwoPhase,
									 char *debugCaller)
{
	volatile struct CdbDispatcherState ds = {NULL, NULL, NULL};

	elog((Debug_print_full_dtm ? LOG : DEBUG5),
		 "cdbdisp_dispatchUtilityStatement called (needTwoPhase = %s, debugCaller = %s)",
		 (needTwoPhase ? "true" : "false"),
		 debugCaller);

	PG_TRY();
	{
		cdbdisp_dispatchUtilityStatement(stmt,
										 true /* cancelOnError */ ,
										 needTwoPhase,
										 true /* withSnapshot */ ,
										 (struct CdbDispatcherState *) &ds,
										 debugCaller);

		/*
		 * Wait for all QEs to finish.	Throw up if error.
		 */
		cdbdisp_finishCommand((struct CdbDispatcherState *) &ds, NULL, NULL);
	}
	PG_CATCH();
	{
		/*
		 * Something happend, clean up after ourselves
		 */
		CdbCheckDispatchResult((struct CdbDispatcherState *) &ds,
							   DISPATCH_WAIT_CANCEL);

		cdbdisp_destroyDispatcherState((struct CdbDispatcherState *) &ds);

		PG_RE_THROW();
	}
	PG_END_TRY();
}
