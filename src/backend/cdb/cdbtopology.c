/*-------------------------------------------------------------------------
 *
 * cdbtopology.c
 *	  Provider selection and the generic half of the cluster-topology store.
 *
 * See cdbtopology.h for what a provider is and why.  This file owns the
 * dispatch table and everything that is the same whatever the storage is; the
 * providers themselves live in cdbtopology_catalog.c and cdbtopology_file.c.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/backend/cdb/cdbtopology.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbtopology.h"
#include "miscadmin.h"
#include "utils/memutils.h"

int			gp_topology_source = GP_TOPOLOGY_SOURCE_CATALOG;

extern const GpTopologyRoutine gp_topology_catalog_routine;

/*
 * Indexed by GpTopologySourceKind, in the smgrsw[] style: a fixed set of
 * backends selected by an enum, no dynamic registration, no catalog involved.
 */
static const GpTopologyRoutine *const gp_topology_routines[] = {
	&gp_topology_catalog_routine	/* GP_TOPOLOGY_SOURCE_CATALOG */
};

#define NGpTopologyRoutines lengthof(gp_topology_routines)

static bool gp_topology_started = false;

const GpTopologyRoutine *
GpTopoActiveProvider(void)
{
	if (gp_topology_source < 0 || gp_topology_source >= NGpTopologyRoutines)
		elog(PANIC, "invalid gp_topology_source value %d", gp_topology_source);

	return gp_topology_routines[gp_topology_source];
}

/*
 * Initialise the active provider.
 *
 * Called eagerly from the postmaster once shared memory exists, so that an
 * unusable store is diagnosed at startup rather than at the first query, and
 * idempotently from the read and write paths, so that EXEC_BACKEND and any
 * path that reaches topology earlier than that still works.
 */
void
GpTopologyProviderStartup(void)
{
	const GpTopologyRoutine *routine;

	if (gp_topology_started)
		return;

	routine = GpTopoActiveProvider();
	if (routine->startup)
		routine->startup();

	gp_topology_started = true;
}

/*
 * Read the whole topology.
 *
 * No validation happens here on purpose.  getCdbComponentInfo() already checks
 * the structural invariants it depends on (one or two entry databases, dense
 * content ids, this node present) after building its array, and applying them
 * on the way out of the store would make every diagnostic path -- dumping the
 * topology of a broken node above all -- fail exactly when an operator needs to
 * see what it actually says.
 */
GpSegConfigEntry *
GpTopologyGetAll(MemoryContext cxt, int *nentries)
{
	const GpTopologyRoutine *routine;
	uint64		gen;

	GpTopologyProviderStartup();
	routine = GpTopoActiveProvider();

	return routine->read_all(cxt, nentries, &gen);
}
