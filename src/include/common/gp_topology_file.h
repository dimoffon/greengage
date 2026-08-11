/*-------------------------------------------------------------------------
 *
 * gp_topology_file.h
 *	  One entry of the cluster topology, and the on-disk format of the
 *	  file-backed topology store.
 *
 * This header is deliberately frontend-safe.  Cluster topology has to be
 * writable by things that are not a running backend -- initdb, the
 * disaster-recovery replica builder, single-user mode -- so the struct and the
 * format live in src/common rather than behind cdbutil.h, which drags in the
 * catalog and the planner.
 *
 * GpSegConfigEntry is one row of the topology, whatever holds it: the
 * gp_segment_configuration catalog, a flat file, or (later) an external store
 * such as etcd or consul.  Everything above the storage layer consumes an
 * array of these and never sees tuples.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/include/common/gp_topology_file.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GP_TOPOLOGY_FILE_H
#define GP_TOPOLOGY_FILE_H

/*
 * How many addresses getDnsCachedAddress() will cache for one host.  Lives
 * here because it sizes a member of the struct below; it is not part of the
 * on-disk format.
 */
#define COMPONENT_DBS_MAX_ADDRS (8)

typedef struct GpSegConfigEntry
{
	/* one row of the cluster topology */
	int16		dbid;			/* the dbid of this database */
	int16		segindex;		/* content indicator: -1 for entry database,
								 * 0, ..., n-1 for segment database */

	char		role;			/* primary, coordinator, mirror, coordinator-standby */
	char		preferred_role; /* what role would we "like" to have this segment in ? */
	char		mode;
	char		status;
	int32		port;			/* port that instance is listening on */
	char	   *hostname;		/* name or ip address of host machine */
	char	   *address;		/* ip address of host machine */
	char	   *datadir;		/* absolute path to data directory on the host. */

	/*
	 * Read-side scratch, never serialized.  Filled in by the DNS cache in
	 * cdbutil.c after the entry has been read from a store; a parser must
	 * zero these.
	 */
	char	   *hostip;			/* cached lookup of name */
	char	   *hostaddrs[COMPONENT_DBS_MAX_ADDRS];	/* cached lookup of names */
} GpSegConfigEntry;

/* Number of serialized fields per entry, in order, as listed above. */
#define GPSEGCONFIGNUMATTR 10

#endif							/* GP_TOPOLOGY_FILE_H */
