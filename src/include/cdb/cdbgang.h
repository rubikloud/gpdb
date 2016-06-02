/*-------------------------------------------------------------------------
 *
 * cdbgang.h
 *
 * Copyright (c) 2005-2008, Greenplum inc
 *
 *-------------------------------------------------------------------------
 */
#ifndef _CDBGANG_H_
#define _CDBGANG_H_

#include "cdb/cdbutil.h"
#include "executor/execdesc.h"
#include <pthread.h>

struct Port;
struct QueryDesc;
struct DirectDispatchInfo;
struct EState;

typedef enum GangType
{
	GANGTYPE_UNALLOCATED, /* a root slice executed by the qDisp */
	GANGTYPE_ENTRYDB_READER, /* a 1-gang with read access to the entry db */
	GANGTYPE_SINGLETON_READER, /* a 1-gang to read the segment dbs */
	GANGTYPE_PRIMARY_READER, /* a N-gang to read the segment dbs */
	GANGTYPE_PRIMARY_WRITER /* the N-gang that can update the segment dbs */
} GangType;

/*
 * A gang represents a single group of workers on each connected segDB
 */
typedef struct Gang
{
	GangType type;
	int	gang_id;
	int	size;

	/*
	 * Keep track of dispatcher use for writer gang. (reader gangs already track
	 * this properly, since they get allocated from a list of available gangs.)
	 */
	bool dispatcherActive;

	/* the named portal that owns this gang, NULL if none */
	char *portal_name;

	/*
	 * Array of QEs/segDBs that make up this gang. Sorted by segment index.
	 */
	struct SegmentDatabaseDescriptor *db_descriptors;	

	/* For debugging purposes only. These do not add any actual functionality. */
	bool allocated;

	/* should be destroyed if set */
	bool noReuse;

	/* memory context */
	MemoryContext perGangContext;
} Gang;

extern Gang *allocateReaderGang(GangType type, char *portal_name);

extern Gang *allocateWriterGang(void);

extern List *getCdbProcessList(Gang *gang, int sliceIndex, struct DirectDispatchInfo *directDispatch);

extern bool gangOK(Gang *gp);

extern List *getCdbProcessesForQD(int isPrimary);

extern void freeGangsForPortal(char *portal_name);

extern void disconnectAndDestroyAllGangs(bool resetSession);

extern void CheckForResetSession(void);

extern List *getAllIdleReaderGangs(void);

extern List *getAllBusyReaderGangs(void);

extern CdbComponentDatabases *getComponentDatabases(void);

extern bool gangsExist(void);

extern struct SegmentDatabaseDescriptor *getSegmentDescriptorFromGang(const Gang *gp, int seg);

extern Gang *findGangById(int gang_id);


/*
 * disconnectAndDestroyIdleReaderGangs()
 *
 * This routine is used when a session has been idle for a while (waiting for the
 * client to send us SQL to execute). The idea is to consume less resources while sitting idle.
 *
 * The expectation is that if the session is logged on, but nobody is sending us work to do,
 * we want to free up whatever resources we can. Usually it means there is a human being at the
 * other end of the connection, and that person has walked away from their terminal, or just hasn't
 * decided what to do next. We could be idle for a very long time (many hours).
 *
 * Of course, freeing gangs means that the next time the user does send in an SQL statement,
 * we need to allocate gangs (at least the writer gang) to do anything. This entails extra work,
 * so we don't want to do this if we don't think the session has gone idle.
 *
 * Only call these routines from an idle session.
 *
 * This routine is also called from the sigalarm signal handler (hopefully that is safe to do).
 */
extern void disconnectAndDestroyIdleReaderGangs(void);

extern void cleanupPortalGangs(Portal portal);

extern int largestGangsize(void);

extern int gp_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg, const char *caller);

/*
 * cdbgang_parse_gpqeid_params
 *
 * Called very early in backend initialization, to interpret the "gpqeid"
 * parameter value that a qExec receives from its qDisp.
 *
 * At this point, client authentication has not been done; the backend
 * command line options have not been processed; GUCs have the settings
 * inherited from the postmaster; etc; so don't try to do too much in here.
 */
extern void cdbgang_parse_gpqeid_params(struct Port *port, const char *gpqeid_value);

extern void cdbgang_parse_gpqdid_params(struct Port *port, const char *gpqdid_value);

extern void cdbgang_parse_gpdaid_params(struct Port *port, const char *gpdaid_value);

/*
 * MPP Worker Process information
 *
 * This structure represents the global information about a worker process.
 * It is constructed on the entry process (QD) and transmitted as part of
 * the global slice table to the involved QEs. Note that this is an
 * immutable, fixed-size structure so it can be held in a contiguous
 * array. In the Slice node, however, it is held in a List.
 */
typedef struct CdbProcess
{

	NodeTag type;

	/*
	 * These fields are established at connection (libpq) time and are
	 * available to the QD in PGconn structure associated with connected
	 * QE. It needs to be explicitly transmitted to QE's
	 */
	char *listenerAddr; /* Interconnect listener IPv4 address, a C-string */

	int listenerPort; /* Interconnect listener port */
	int pid; /* Backend PID of the process. */

	int contentid;
} CdbProcess;

/*
 * MPP Plan Slice information
 *
 * These structures summarize how a plan tree is sliced up into separate
 * units of execution or slices. A slice will execute on a each worker within
 * a gang of processes. Some gangs have a worker process on each of several
 * databases, others have a single worker.
 *
 */
typedef struct Slice
{
	NodeTag type;

	/*
	 * The index in the global slice table of this slice. The root slice of
	 * the main plan is always 0. Slices that have senders at their local
	 * root have a sliceIndex equal to the motionID of their sender Motion.
	 *
	 * Undefined slices should have this set to -1.
	 */
	int sliceIndex;

	/*
	 * The root slice of the slice tree of which this slice is a part.
	 */
	int rootIndex;

	/*
	 * the index of parent in global slice table (origin 0) or -1 if
	 * this is root slice.
	 */
	int parentIndex;

	/*
	 * An integer list of indices in the global slice table (origin 0)
	 * of the child slices of this slice, or -1 if this is a leaf slice.
	 * A child slice corresponds to a receiving motion in this slice.
	 */
	List *children;

	/* What kind of gang does this slice need */
	GangType gangType;

	/*
	 * How many gang members needed
	 *
	 * It is set before the process lists below and used to decide how
	 * to initialize them.
	 */
	int gangSize;

	/*
	 * How many of the gang members will actually be used. This takes into
	 * account directDispatch information
	 */
	int numGangMembersToBeActive;

	/*
	 * directDispatch->isDirectDispatch should ONLY be set for a slice when it requires an n-gang.
	 */
	DirectDispatchInfo directDispatch;

	struct Gang *primaryGang;

	/* tell dispatch agents which gang we're talking about.*/
	int primary_gang_id;


	/*
	 * A list of CDBProcess nodes corresponding to the worker processes allocated
	 * to implement this plan slice.
	 *
	 * The number of processes must agree with the the plan slice to be implemented.
	 */
	List *primaryProcesses;
} Slice;

/*
 * The SliceTable is a list of Slice structures organized into root slices
 * and motion slices as follows:
 *
 * Slice 0 is the root slice of plan as a whole.
 * Slices 1 through nMotion are motion slices with a sending motion at
 * the root of the slice.
 * Slices nMotion+1 and on are root slices of initPlans.
 *
 * There may be unused slices in case the plan contains subplans that
 * are not initPlans. (This won't happen unless MPP decides to support
 * subplans similarly to PostgreSQL, which isn't the current plan.)
 */
typedef struct SliceTable
{
	NodeTag type;

	int nMotions; /* The number Motion nodes in the entire plan */
	int nInitPlans; /* The number of initplan slices allocated */
	int localSlice; /* Index of the slice to execute */
	List *slices; /* List of slices */
	bool doInstrument; /* true => collect stats for EXPLAIN ANALYZE */
	uint32 ic_instance_id;
} SliceTable;

extern void InitSliceTable(struct EState *estate, int nMotions, int nSubplans);
extern Slice *getCurrentSlice(struct EState *estate, int sliceIndex);
extern bool sliceRunsOnQD(Slice *slice);
extern bool sliceRunsOnQE(Slice *slice);
extern int sliceCalculateNumSendingProcesses(Slice *slice);

extern void InitRootSlices(QueryDesc *queryDesc);
extern void AssignGangs(QueryDesc *queryDesc);
extern void ReleaseGangs(QueryDesc *queryDesc);

#ifdef USE_ASSERT_CHECKING
struct PlannedStmt;

extern void AssertSliceTableIsValid(SliceTable *st, struct PlannedStmt *pstmt);
#endif

#endif   /* _CDBGANG_H_ */
