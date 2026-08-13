/*-------------------------------------------------------------------------
 *
 * dr_served_snapshot.c
 *	  The frozen restore-point snapshot a DR replica serves reads from.
 *
 * See dr_served_snapshot.h for the rationale.  This file owns the shared memory
 * and the pending/served transition; the snapshot contents come from procarray.c,
 * which is where KnownAssignedXids lives.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/dr_served_snapshot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/dr_served_snapshot.h"
#include "access/twophase.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"

typedef struct DRSnapshotSlot
{
	bool		valid;
	char		rpName[MAXFNAMELEN];
	TimestampTz rpTime;			/* when PRODUCTION created the restore point */
	TransactionId xmin;
	TransactionId xmax;
	int			subxcnt;
	bool		suboverflowed;
	/* xids live in the trailing array, see DRServedSnapshotShmemInit */
	TransactionId *subxip;
} DRSnapshotSlot;

typedef struct DRServedSnapshotCtl
{
	slock_t		mutex;
	DRSnapshotSlot pending;
	DRSnapshotSlot served;
	TransactionId xids[FLEXIBLE_ARRAY_MEMBER];	/* pending block, then served */
} DRServedSnapshotCtl;

static DRServedSnapshotCtl *DRServedSnapshotCtlData = NULL;

/*
 * How many xids a frozen image can hold.  In recovery GetSnapshotData puts every
 * running xid into subxip (xip stays empty), so this must match the sizing
 * GetSnapshotData itself uses for that array:
 * GetMaxSnapshotXidCount() + GetMaxSnapshotSubxidCount().
 *
 * Spelled out from MaxBackends rather than called through those two, because the
 * first caller is the shared-memory *sizing* pass, which runs before
 * CreateSharedProcArray() -- GetMaxSnapshotXidCount() reads procArray->maxProcs
 * and would dereference NULL.  DRCaptureServedSnapshot() asserts the two agree
 * once the proc array exists.
 */
int
DRServedSnapshotMaxXids(void)
{
	return (PGPROC_MAX_CACHED_SUBXIDS + 2) * (MaxBackends + max_prepared_xacts);
}

Size
DRServedSnapshotShmemSize(void)
{
	Size		size;

	size = offsetof(DRServedSnapshotCtl, xids);
	size = add_size(size, mul_size(sizeof(TransactionId),
								   mul_size(2, DRServedSnapshotMaxXids())));
	return size;
}

void
DRServedSnapshotShmemInit(void)
{
	bool		found;
	int			maxXids = DRServedSnapshotMaxXids();

	DRServedSnapshotCtlData = (DRServedSnapshotCtl *)
		ShmemInitStruct("DR Served Snapshot", DRServedSnapshotShmemSize(), &found);

	if (!found)
	{
		MemSet(DRServedSnapshotCtlData, 0, DRServedSnapshotShmemSize());
		SpinLockInit(&DRServedSnapshotCtlData->mutex);
	}

	/* Wire the slot pointers into the trailing array (every backend does this). */
	DRServedSnapshotCtlData->pending.subxip = DRServedSnapshotCtlData->xids;
	DRServedSnapshotCtlData->served.subxip = DRServedSnapshotCtlData->xids + maxXids;
}

void
DRServedSnapshotStorePending(const char *rpName,
							 TimestampTz rpTime,
							 TransactionId xmin,
							 TransactionId xmax,
							 const TransactionId *subxip,
							 int subxcnt,
							 bool suboverflowed)
{
	DRSnapshotSlot *slot;

	if (DRServedSnapshotCtlData == NULL)
		return;

	if (subxcnt > DRServedSnapshotMaxXids())
	{
		/*
		 * Cannot happen -- the caller sized subxip from the same function --
		 * but if it ever did, dropping the image is the only safe response:
		 * a truncated running-xid set would make committed-later transactions
		 * visible, which is precisely the bug this exists to prevent.
		 */
		elog(WARNING,
			 "disaster-recovery replica: running-xid set (%d) exceeds the served-snapshot capacity (%d); not publishing restore point \"%s\"",
			 subxcnt, DRServedSnapshotMaxXids(), rpName);
		return;
	}

	SpinLockAcquire(&DRServedSnapshotCtlData->mutex);
	slot = &DRServedSnapshotCtlData->pending;
	strlcpy(slot->rpName, rpName, MAXFNAMELEN);
	slot->rpTime = rpTime;
	slot->xmin = xmin;
	slot->xmax = xmax;
	slot->subxcnt = subxcnt;
	slot->suboverflowed = suboverflowed;
	if (subxcnt > 0)
		memcpy(slot->subxip, subxip, subxcnt * sizeof(TransactionId));
	slot->valid = true;
	SpinLockRelease(&DRServedSnapshotCtlData->mutex);

	elog(LOG,
		 "disaster-recovery replica: captured the image at restore point \"%s\" (xmin %u, xmax %u, %d running xid(s))",
		 rpName, xmin, xmax, subxcnt);
}

bool
DRServedSnapshotPublish(const char *rpName)
{
	DRSnapshotSlot *pending;
	DRSnapshotSlot *served;
	bool		published = false;

	if (DRServedSnapshotCtlData == NULL)
		return false;

	SpinLockAcquire(&DRServedSnapshotCtlData->mutex);
	pending = &DRServedSnapshotCtlData->pending;
	served = &DRServedSnapshotCtlData->served;

	if (pending->valid && strcmp(pending->rpName, rpName) == 0)
	{
		strlcpy(served->rpName, pending->rpName, MAXFNAMELEN);
		served->rpTime = pending->rpTime;
		served->xmin = pending->xmin;
		served->xmax = pending->xmax;
		served->subxcnt = pending->subxcnt;
		served->suboverflowed = pending->suboverflowed;
		if (pending->subxcnt > 0)
			memcpy(served->subxip, pending->subxip,
				   pending->subxcnt * sizeof(TransactionId));
		served->valid = true;
		published = true;
	}
	SpinLockRelease(&DRServedSnapshotCtlData->mutex);

	if (published)
		elog(LOG, "disaster-recovery replica: now serving restore point \"%s\"", rpName);

	return published;
}

/*
 * Serve this point immediately, without waiting for the coordinator, when this
 * node has served nothing at all since the postmaster started.
 *
 * Without this a replica would be dead on arrival and dead after every restart:
 * the frozen image lives in shared memory, which the postmaster does not
 * outlive, so a restarted node has nothing to serve and every snapshot fails
 * closed -- including the snapshots gg_dr_switch() itself needs to talk to the
 * segments.  There would be no way back in.
 *
 * The "nothing served yet" condition is what keeps it from tearing.  A node in
 * that state is not participating in an advance from one served point to
 * another; it is converging on its armed target from a cold start, as are its
 * peers.  Whichever arrives first serves that point; the ones still short of it
 * serve nothing and error, and an error is not a torn read.
 *
 * The exception is a node that restarts *during* an advance: it comes back with
 * nothing served, replays to the new target, and starts serving it while its
 * peers still serve the old one.  That window closes when the coordinator's
 * gg_dr_switch() publishes the new point everywhere.  It is not closed here
 * because every way of closing it leaves the node refusing snapshots, which is
 * the unrecoverable state described above.
 */
void
DRServedSnapshotPublishIfNothingServed(const char *rpName)
{
	if (DRServedSnapshotIsPublished())
		return;

	DRServedSnapshotPublish(rpName);
}

TimestampTz
DRServedSnapshotGetTime(void)
{
	TimestampTz when = 0;

	if (DRServedSnapshotCtlData == NULL)
		return 0;

	SpinLockAcquire(&DRServedSnapshotCtlData->mutex);
	if (DRServedSnapshotCtlData->served.valid)
		when = DRServedSnapshotCtlData->served.rpTime;
	SpinLockRelease(&DRServedSnapshotCtlData->mutex);

	return when;
}

bool
DRServedSnapshotIsPublished(void)
{
	bool		valid;

	if (DRServedSnapshotCtlData == NULL)
		return false;

	SpinLockAcquire(&DRServedSnapshotCtlData->mutex);
	valid = DRServedSnapshotCtlData->served.valid;
	SpinLockRelease(&DRServedSnapshotCtlData->mutex);

	return valid;
}

void
DRServedSnapshotGetName(char *buf, Size buflen)
{
	if (DRServedSnapshotCtlData == NULL)
	{
		buf[0] = '\0';
		return;
	}

	SpinLockAcquire(&DRServedSnapshotCtlData->mutex);
	if (DRServedSnapshotCtlData->served.valid)
		strlcpy(buf, DRServedSnapshotCtlData->served.rpName, buflen);
	else
		buf[0] = '\0';
	SpinLockRelease(&DRServedSnapshotCtlData->mutex);
}

bool
DRServedSnapshotFill(TransactionId *xmin, TransactionId *xmax,
					 TransactionId *subxip, int *subxcnt,
					 bool *suboverflowed)
{
	DRSnapshotSlot *served;

	if (DRServedSnapshotCtlData == NULL)
		return false;

	SpinLockAcquire(&DRServedSnapshotCtlData->mutex);
	served = &DRServedSnapshotCtlData->served;
	if (!served->valid)
	{
		SpinLockRelease(&DRServedSnapshotCtlData->mutex);
		return false;
	}
	*xmin = served->xmin;
	*xmax = served->xmax;
	*subxcnt = served->subxcnt;
	*suboverflowed = served->suboverflowed;
	if (served->subxcnt > 0)
		memcpy(subxip, served->subxip, served->subxcnt * sizeof(TransactionId));
	SpinLockRelease(&DRServedSnapshotCtlData->mutex);

	return true;
}
