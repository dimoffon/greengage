/*-------------------------------------------------------------------------
 *
 * dr_redo_filter.h
 *	  Apply-time WAL redo filter for Greengage disaster-recovery (DR) replicas.
 *
 * On a read-only DR replica the cluster-topology catalogs
 * (gp_segment_configuration and its siblings) describe the *DR* cluster, not
 * production.  Because those are shared catalogs replicated block-for-block by
 * physical WAL, blindly replaying production's WAL would overwrite the
 * DR-local rows.  This filter skips redo of records that touch only the
 * protected topology catalogs, leaving DR's own pages intact while the replay
 * LSN still advances.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/include/access/dr_redo_filter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DR_REDO_FILTER_H
#define DR_REDO_FILTER_H

#include "access/xlogreader.h"
#include "storage/relfilenode.h"

/*
 * Resolve the protected relfilenode set from the shared relmapper.  Done lazily
 * on first use; call DRInvalidateProtectedRelfilenodes() to force a recompute
 * (e.g. after a shared relmap update is replayed).
 */
extern void DRResolveProtectedRelfilenodes(void);
extern void DRInvalidateProtectedRelfilenodes(void);

/* True if rnode is one of the protected cluster-topology catalogs. */
extern bool DRRelfilenodeIsProtected(const RelFileNode *rnode);

/*
 * True if this record's redo should be skipped on a DR replica: DR mode is
 * active and every block the record modifies belongs to a protected topology
 * catalog.
 */
extern bool DRRedoShouldFilter(XLogReaderState *record);

#endif							/* DR_REDO_FILTER_H */
