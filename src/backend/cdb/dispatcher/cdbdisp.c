
/*-------------------------------------------------------------------------
 *
 * cdbdisp.c
 *	  Functions to dispatch commands to QExecutors.
 *
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include <limits.h>

#include "storage/ipc.h"		/* For proc_exit_inprogress */
#include "tcop/tcopprot.h"
#include "cdb/cdbdisp.h"
#include "cdb/cdbdisp_thread.h"
#include "cdb/cdbdispatchresult.h"
#include "cdb/cdbfts.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbsreh.h"
#include "cdb/cdbvars.h"

/*
 * default directed-dispatch parameters: don't direct anything.
 */
CdbDispatchDirectDesc default_dispatch_direct_desc = { false, 0, {0}};

static void cdbdisp_clearGangActiveFlag(CdbDispatcherState * ds);

/*
 * cdbdisp_dispatchToGang:
 * Send the strCommand SQL statement to the subset of all segdbs in the cluster
 * specified by the gang parameter.  cancelOnError indicates whether an error
 * occurring on one of the qExec segdbs should cause all still-executing commands to cancel
 * on other qExecs. Normally this would be true.  The commands are sent over the libpq
 * connections that were established during cdblink_setup.	They are run inside of threads.
 * The number of segdbs handled by any one thread is determined by the
 * guc variable gp_connections_per_thread.
 *
 * The caller must provide a CdbDispatchResults object having available
 * resultArray slots sufficient for the number of QEs to be dispatched:
 * i.e., resultCapacity - resultCount >= gp->size.	This function will
 * assign one resultArray slot per QE of the Gang, paralleling the Gang's
 * db_descriptors array.  Success or failure of each QE will be noted in
 * the QE's CdbDispatchResult entry; but before examining the results, the
 * caller must wait for execution to end by calling CdbCheckDispatchResult().
 *
 * The CdbDispatchResults object owns some malloc'ed storage, so the caller
 * must make certain to free it by calling cdbdisp_destroyDispatcherState().
 *
 * When dispatchResults->cancelOnError is false, strCommand is to be
 * dispatched to every connected gang member if possible, despite any
 * cancellation requests, QE errors, connection failures, etc.
 *
 * NB: This function should return normally even if there is an error.
 * It should not longjmp out via elog(ERROR, ...), ereport(ERROR, ...),
 * PG_THROW, CHECK_FOR_INTERRUPTS, etc.
 */
void
cdbdisp_dispatchToGang(struct CdbDispatcherState *ds,
					   struct Gang *gp,
					   int sliceIndex,
					   CdbDispatchDirectDesc * disp_direct)
{
	struct CdbDispatchResults *dispatchResults = ds->primaryResults;

	Assert(Gp_role == GP_ROLE_DISPATCH);
	Assert(gp && gp->size > 0);
	Assert(dispatchResults && dispatchResults->resultArray);

	if (dispatchResults->writer_gang)
	{
		/*
		 * Are we dispatching to the writer-gang when it is already busy ?
		 */
		if (gp == dispatchResults->writer_gang)
		{
			if (dispatchResults->writer_gang->dispatcherActive)
			{
				ereport(ERROR,
						(errcode(ERRCODE_INTERNAL_ERROR),
						 errmsg("query plan with multiple segworker groups is not supported"),
						 errhint("likely caused by a function that reads or modifies data in a distributed table")));
			}

			dispatchResults->writer_gang->dispatcherActive = true;
		}
	}

	/*
	 * WIP: will use a function pointer for implementation later, currently just use an internal function to move dispatch
	 * thread related code into a separate file.
	 */
	cdbdisp_dispatchToGang_internal(ds, gp, sliceIndex, disp_direct);
}

/*
 * CdbCheckDispatchResult:
 *
 * Waits for completion of threads launched by cdbdisp_dispatchToGang().
 *
 * QEs that were dispatched with 'cancelOnError' true and are not yet idle
 * will be canceled/finished according to waitMode.
 */
void
CdbCheckDispatchResult(struct CdbDispatcherState *ds,
					   DispatchWaitMode waitMode)
{
	PG_TRY();
	{
		CdbCheckDispatchResult_internal(ds, NULL, NULL, waitMode);
	}
	PG_CATCH();
	{
		cdbdisp_clearGangActiveFlag(ds);
		PG_RE_THROW();
	}
	PG_END_TRY();

	cdbdisp_clearGangActiveFlag(ds);

	if (log_dispatch_stats)
		ShowUsage("DISPATCH STATISTICS");

	if (DEBUG1 >= log_min_messages)
	{
		char msec_str[32];

		switch (check_log_duration(msec_str, false))
		{
			case 1:
			case 2:
				ereport(LOG,
						(errmsg("duration to dispatch result received from all QEs: %s ms", msec_str)));
				break;
		}
	}
}

/*
 * Wait for all QEs to finish, then report any errors from the given
 * CdbDispatchResults objects and free them.  If not all QEs in the
 * associated gang(s) executed the command successfully, throws an
 * error and does not return.  No-op if both CdbDispatchResults ptrs are NULL.
 * This is a convenience function; callers with unusual requirements may
 * instead call CdbCheckDispatchResult(), etc., directly.
 */
void
cdbdisp_finishCommand(struct CdbDispatcherState *ds,
					  void (*handle_results_callback) (CdbDispatchResults *
													   primaryResults,
													   void *ctx), void *ctx)
{
	StringInfoData buf;
	int errorcode = 0;

	/*
	 * If cdbdisp_dispatchToGang() wasn't called, don't wait.
	 */
	if (!ds || !ds->primaryResults)
		return;

	/*
	 * If we are called in the dying sequence, don't touch QE connections.
	 * Anything below could cause ERROR in which case we would miss a chance
	 * to clean up shared memory as this is from AbortTransaction.
	 * QE may stay a bit longer, but since we can consider QD as libpq
	 * client to QE, they will notice that we as a client do not
	 * appear anymore and will finish soon.  Also ERROR report doesn't
	 * go to the client anyway since we are in proc_exit.
	 */
	if (proc_exit_inprogress)
		return;

	/*
	 * Wait for all QEs to finish. Don't cancel them. 
	 */
	CdbCheckDispatchResult(ds, DISPATCH_WAIT_NONE);

	/*
	 * If no errors, free the CdbDispatchResults objects and return.
	 */
	if (ds->primaryResults)
		errorcode = ds->primaryResults->errcode;

	if (!errorcode)
	{
		/*
		 * Call the callback function to handle the results
		 */
		if (handle_results_callback != NULL)
			handle_results_callback(ds->primaryResults, ctx);

		cdbdisp_destroyDispatcherState(ds);
		return;
	}

	/*
	 * Format error messages from the primary gang.
	 */
	initStringInfo(&buf);
	cdbdisp_dumpDispatchResults(ds->primaryResults, &buf, false);

	cdbdisp_destroyDispatcherState(ds);

	/*
	 * Too bad, our gang got an error.
	 */
	PG_TRY();
	{
		ereport(ERROR, (errcode(errorcode),
						errOmitLocation(true), errmsg("%s", buf.data)));
	}
	PG_CATCH();
	{
		pfree(buf.data);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * cdbdisp_handleError
 *
 * When caller catches an error, the PG_CATCH handler can use this
 * function instead of cdbdisp_finishCommand to wait for all QEs
 * to finish, clean up, and report QE errors if appropriate.
 * This function should be called only from PG_CATCH handlers.
 *
 * This function destroys and frees the given CdbDispatchResults objects.
 * It is a no-op if both CdbDispatchResults ptrs are NULL.
 *
 * On return, the caller is expected to finish its own cleanup and
 * exit via PG_RE_THROW().
 */
void
cdbdisp_handleError(struct CdbDispatcherState *ds)
{
	int			qderrcode;
	bool		useQeError = false;

	qderrcode = elog_geterrcode();

	/*
	 * If cdbdisp_dispatchToGang() wasn't called, don't wait.
	 */
	if (!ds || !ds->primaryResults)
		return;

	/*
	 * Request any remaining commands executing on qExecs to stop.
	 * We need to wait for the threads to finish.  This allows for proper
	 * cleanup of the results from the async command executions.
	 * Cancel any QEs still running.
	 */
	CdbCheckDispatchResult(ds, DISPATCH_WAIT_CANCEL);

	/*
	 * When a QE stops executing a command due to an error, as a
	 * consequence there can be a cascade of interconnect errors
	 * (usually "sender closed connection prematurely") thrown in
	 * downstream processes (QEs and QD).  So if we are handling
	 * an interconnect error, and a QE hit a more interesting error,
	 * we'll let the QE's error report take precedence.
	 */
	if (qderrcode == ERRCODE_GP_INTERCONNECTION_ERROR)
	{
		bool qd_lost_flag = false;
		char *qderrtext = elog_message();

		if (qderrtext
			&& strcmp(qderrtext, CDB_MOTION_LOST_CONTACT_STRING) == 0)
			qd_lost_flag = true;

		if (ds->primaryResults && ds->primaryResults->errcode)
		{
			if (qd_lost_flag
				&& ds->primaryResults->errcode == ERRCODE_GP_INTERCONNECTION_ERROR)
				useQeError = true;
			else if (ds->primaryResults->errcode != ERRCODE_GP_INTERCONNECTION_ERROR)
				useQeError = true;
		}
	}

	if (useQeError)
	{
		/*
		 * Throw the QE's error, catch it, and fall thru to return
		 * normally so caller can finish cleaning up.  Afterwards
		 * caller must exit via PG_RE_THROW().
		 */
		PG_TRY();
		{
			cdbdisp_finishCommand(ds, NULL, NULL);
		}
		PG_CATCH();
		{
		}						/* nop; fall thru */
		PG_END_TRY();
	}
	else
	{
		/*
		 * Discard any remaining results from QEs; don't confuse matters by
		 * throwing a new error.  Any results of interest presumably should
		 * have been examined before raising the error that the caller is
		 * currently handling.
		 */
		cdbdisp_destroyDispatcherState(ds);
	}
}

/*
 * Allocate memory and initialize CdbDispatcherState.
 *
 * Call cdbdisp_destroyDispatcherState to free it.
 *
 *	 maxResults: max number of results, normally equals to max number of QEs.
 *	 maxSlices: max number of slices of the query/command.
 */
void
cdbdisp_makeDispatcherState(CdbDispatcherState * ds, int maxResults,
							int maxSlices, bool cancelOnError)
{
	MemoryContext oldContext = NULL;

	Assert(ds != NULL);
	Assert(ds->dispatchStateContext == NULL);
	Assert(ds->dispatchThreads == NULL);
	Assert(ds->primaryResults == NULL);

	ds->dispatchStateContext = AllocSetContextCreate(TopMemoryContext,
													 "Dispatch Context",
													 ALLOCSET_DEFAULT_MINSIZE,
													 ALLOCSET_DEFAULT_INITSIZE,
													 ALLOCSET_DEFAULT_MAXSIZE);

	oldContext = MemoryContextSwitchTo(ds->dispatchStateContext);
	ds->primaryResults = cdbdisp_makeDispatchResults(maxResults,
													 maxSlices,
													 cancelOnError);
	ds->dispatchThreads = cdbdisp_makeDispatchThreads(maxSlices);
	MemoryContextSwitchTo(oldContext);
}

/*
 * Free memory in CdbDispatcherState
 *
 * Free the PQExpBufferData allocated in libpq.
 * Free dispatcher memory context.
 */
void
cdbdisp_destroyDispatcherState(CdbDispatcherState * ds)
{
	CdbDispatchResults *results = ds->primaryResults;

	if (results != NULL && results->resultArray != NULL)
	{
		int i;

		for (i = 0; i < results->resultCount; i++)
		{
			cdbdisp_termResult(&results->resultArray[i]);
		}
		results->resultArray = NULL;
	}

	if (ds->dispatchStateContext != NULL)
	{
		MemoryContextDelete(ds->dispatchStateContext);
		ds->dispatchStateContext = NULL;
	}

	ds->dispatchStateContext = NULL;
	ds->dispatchThreads = NULL;
	ds->primaryResults = NULL;
}

/*
 * Clear our "active" flags; so that we know that the writer gangs are busy -- and don't stomp on
 * internal dispatcher structures.
 */
static void
cdbdisp_clearGangActiveFlag(CdbDispatcherState * ds)
{
	if (ds && ds->primaryResults && ds->primaryResults->writer_gang)
	{
		ds->primaryResults->writer_gang->dispatcherActive = false;
	}
}
