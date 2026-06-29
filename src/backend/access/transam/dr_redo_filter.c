/*-------------------------------------------------------------------------
 *
 * dr_redo_filter.c
 *	  Apply-time WAL redo filter for Greengage disaster-recovery (DR) replicas.
 *
 * See dr_redo_filter.h for the rationale.  This file provides:
 *	- DRRedoShouldFilter(): the per-record predicate called from the main redo
 *	  loop in xlog.c, and
 *	- DRRelfilenodeIsProtected()/DRResolveProtectedRelfilenodes(): the protected
 *	  topology-catalog set, resolved via the shared relmapper at startup.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/access/transam/dr_redo_filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/dr_redo_filter.h"
#include "access/xlog.h"
#include "access/xlogrecord.h"

/*
 * DRRelfilenodeIsProtected
 *		Is rnode one of the protected cluster-topology catalogs?
 *
 * The protected set is resolved from the shared relmapper at startup
 * (see DRResolveProtectedRelfilenodes()).  Until that is implemented (M1.3)
 * this returns false, so DRRedoShouldFilter() is a strict no-op and replay
 * behaves exactly as on a normal standby.
 */
bool
DRRelfilenodeIsProtected(const RelFileNode *rnode)
{
	/* M1.3: match rnode against the relmapper-resolved protected set. */
	return false;
}

/*
 * DRResolveProtectedRelfilenodes
 *		Resolve the protected topology-catalog relfilenodes (M1.3).
 *
 * Placeholder; wired up when the protected set is implemented.
 */
void
DRResolveProtectedRelfilenodes(void)
{
	/* M1.3 */
}

/*
 * DRRedoShouldFilter
 *		Should this WAL record's redo be skipped on a DR replica?
 *
 * Returns true only when DR mode is active and *every* block the record
 * references belongs to a protected topology catalog.  A record with no block
 * references, or one that touches any non-protected relation, is always
 * applied.  Skipping redo is safe: the replay LSN is advanced by the record
 * reader, not by the resource manager's redo routine.
 */
bool
DRRedoShouldFilter(XLogReaderState *record)
{
	bool		any_block = false;
	int			block_id;

	if (!IsDRReplicaMode())
		return false;

	for (block_id = 0; block_id <= XLR_MAX_BLOCK_ID; block_id++)
	{
		RelFileNode rnode;
		ForkNumber	forknum;
		BlockNumber blknum;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		any_block = true;
		XLogRecGetBlockTag(record, block_id, &rnode, &forknum, &blknum);

		/* Any non-protected block means we must apply the record normally. */
		if (!DRRelfilenodeIsProtected(&rnode))
			return false;
	}

	return any_block;
}
