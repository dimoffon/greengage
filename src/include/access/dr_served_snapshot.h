/*-------------------------------------------------------------------------
 *
 * dr_served_snapshot.h
 *	  The frozen restore-point snapshot a DR replica serves reads from.
 *
 * A disaster-recovery replica recovers *up to* a distributed restore point and
 * serves reads there.  While it advances to the next restore point its nodes are
 * at different WAL positions, so anything read from live state is torn: a single
 * distributed transaction can be visible on one segment and absent on another.
 *
 * The fix is not to reconstruct a distributed snapshot -- a restore point already
 * supplies the cross-node order that a distributed snapshot exists to provide.
 * Instead each node freezes its ordinary local MVCC snapshot at the moment it
 * pauses at the restore point, and every read is served from that frozen image
 * until the next one is published.  Hot standby already maintains exactly the
 * right object by redo (KnownAssignedXids), including prepared transactions,
 * which is the part a gxid-based reconstruction cannot produce.
 *
 * Two slots, because the nodes do not arrive together:
 *
 *	  pending  written by the startup process when *this* node pauses;
 *	  served   flipped from pending only once EVERY node has arrived, which the
 *			   coordinator establishes before dispatching the publish.
 *
 * Publishing at each node's own pause would serve the new point on a node that
 * reached it early while a lagging segment is still short of it -- the same torn
 * read, one restore point later.  A failed or cancelled switch simply leaves
 * `served` at the previous point: stale, but never wrong.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/include/access/dr_served_snapshot.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DR_SERVED_SNAPSHOT_H
#define DR_SERVED_SNAPSHOT_H

#include "access/xlogdefs.h"
#include "storage/lock.h"

extern Size DRServedSnapshotShmemSize(void);
extern void DRServedSnapshotShmemInit(void);

/*
 * Record this node's frozen image as *pending*, tagged with the restore point it
 * was taken at.  Called by the startup process from pauseRecoveryOnRestorePoint()
 * while it holds nothing but its own hands -- the caller supplies the snapshot
 * contents because KnownAssignedXids lives in procarray.c.
 */
extern void DRServedSnapshotStorePending(const char *rpName,
										 TransactionId xmin,
										 TransactionId xmax,
										 const TransactionId *subxip,
										 int subxcnt,
										 bool suboverflowed);

/*
 * Publish pending as served, but only if pending was taken at rpName.  Returns
 * false when there is nothing matching to publish -- which is not an error: the
 * first dispatch of a switch arrives before this node has reached the point.
 */
extern bool DRServedSnapshotPublish(const char *rpName);

/*
 * Publish without waiting for the coordinator, if and only if this node has
 * served nothing at all since the postmaster started.  Called by the startup
 * process on arrival at a restore point; see the implementation for why a node
 * that has already served something must never take this path.
 */
extern void DRServedSnapshotPublishIfNothingServed(const char *rpName);

/* Is there anything to serve?  Reads must fail closed when there is not. */
extern bool DRServedSnapshotIsPublished(void);

/* Name of the restore point currently being served, or "" if none. */
extern void DRServedSnapshotGetName(char *buf, Size buflen);

/*
 * Copy the served image into an in-progress snapshot.  subxip must have room for
 * DRServedSnapshotMaxXids() entries.  Returns false if nothing is published.
 */
extern bool DRServedSnapshotFill(TransactionId *xmin, TransactionId *xmax,
								 TransactionId *subxip, int *subxcnt,
								 bool *suboverflowed);

extern int	DRServedSnapshotMaxXids(void);

#endif							/* DR_SERVED_SNAPSHOT_H */
