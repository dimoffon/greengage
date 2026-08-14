/*-------------------------------------------------------------------------
 *
 * gg_topology_file.h
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
 *	  src/include/common/gg_topology_file.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GG_TOPOLOGY_FILE_H
#define GG_TOPOLOGY_FILE_H

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


/* ----------------------------------------------------------------
 *				$PGDATA/gg_topology
 * ----------------------------------------------------------------
 *
 * The on-disk form of a whole topology, for the file-backed store:
 *
 *		GPTOPOLOGY 1
 *		system_identifier 7123456789012345678
 *		generation 42
 *		nentries 9
 *		1 -1 p p s u 5432 cdw cdw /data/coordinator/gpseg-1
 *		...
 *		crc32c 9f3ac21b
 *
 * Text, for three reasons: the entry lines are byte-identical to the ones the
 * catalog provider has always dumped for out-of-transaction readers, so
 * migration is a header wrap; the file is a few hundred lines at most; and the
 * first thing anyone does to a node that will not start is cat it.
 *
 * The CRC covers every byte from the start of the file up to and including the
 * newline that ends the last entry -- i.e. everything but the crc32c line, so
 * the writer never has to special-case where the covered region stops.  Unlike
 * pg_control's, it is host-endian-independent: FIN_CRC32C byte-swaps, and every
 * other byte in the file is ASCII.
 *
 * generation is the compare-and-set token, and doubles as the bootstrap
 * marker: initdb writes generation 0 with no entries, and every real writer
 * writes at least 1.  So "generation 0 with entries" is a corrupt header, and
 * "generation 0" on a node that is supposed to be serving is a store nobody
 * ever migrated into.  system_identifier catches a file copied in from an
 * unrelated cluster -- but only one way round: a disaster-recovery replica is a
 * physical copy of production and shares its identifier, so a match proves
 * nothing.
 *
 * Nothing here reports errors.  Every entry point returns a typed code, because
 * the same condition is FATAL in the postmaster, an ERROR in a backend and an
 * exit(1) in initdb, and only the caller knows which.
 */

#define GG_TOPOLOGY_FILENAME		"gg_topology"
#define GG_TOPOLOGY_TMP_PREFIX		"gg_topology.tmp"
#define GG_TOPOLOGY_FORMAT_VERSION	1

typedef struct GgTopologyFile
{
	int			version;		/* as read; always _FORMAT_VERSION on write */
	uint64		system_identifier;	/* 0 == not recorded, not comparable */
	uint64		generation;		/* CAS token; 0 == never written for real */
	int			nentries;
	GpSegConfigEntry *entries;	/* NULL iff nentries == 0 */
} GgTopologyFile;

typedef enum GgTopologyFileError
{
	GG_TOPOFILE_OK = 0,
	GG_TOPOFILE_ENOENT,			/* read: no such file; errno preserved */
	GG_TOPOFILE_IO,				/* open/read/write/rename failed; errno kept */
	GG_TOPOFILE_TRUNCATED,		/* ended before the crc32c line */
	GG_TOPOFILE_BAD_MAGIC,
	GG_TOPOFILE_BAD_VERSION,
	GG_TOPOFILE_BAD_CRC,
	GG_TOPOFILE_BAD_COUNT,		/* nentries disagrees with the lines present */
	GG_TOPOFILE_BAD_HEADER,		/* generation 0 with entries */
	GG_TOPOFILE_BAD_SYNTAX,		/* an entry line; *errline names it */
	GG_TOPOFILE_TOO_LARGE,		/* past the ceiling a topology can reach */
	GG_TOPOFILE_UNSERIALIZABLE,	/* write: a field that could not be read back */
	GG_TOPOFILE_SYSID_CONFLICT	/* write: would overwrite another cluster's */
} GgTopologyFileError;

extern const char *gg_topology_file_error_str(GgTopologyFileError err);

/*
 * Parse `len` bytes of `buf`.  The caller owns buf throughout and may free it
 * the moment this returns: every string is copied.  On success out->entries and
 * its strings are freshly allocated (palloc in the backend, malloc in a
 * frontend) and the read-side scratch fields are zeroed.  On any failure *out
 * is left zeroed and nothing is allocated.
 */
extern GgTopologyFileError gg_topology_parse(const char *buf, size_t len,
											 GgTopologyFile *out, int *errline);

/*
 * Render `topo`, CRC included.  Returns a NUL-terminated buffer the caller
 * owns; *len is its length without the terminator.  Returns NULL with *err set
 * to UNSERIALIZABLE and *errentry the 0-based offender when a field could not
 * be written and read back -- whitespace in a string, a NULL string, a dbid or
 * port out of range.  Never mutates *topo.
 */
extern char *gg_topology_serialize(const GgTopologyFile *topo, size_t *len,
								   GgTopologyFileError *err, int *errentry);

extern GgTopologyFileError gg_topology_read_file(const char *datadir,
												 GgTopologyFile *out,
												 int *errline);

/*
 * Write durably: serialize, write a per-pid temp file, fsync, durable_rename.
 * Never mutates *topo -- in particular it does not touch generation, which is
 * the caller's decision.  Unless `force`, refuses when the file already there
 * records a different system identifier.
 */
extern GgTopologyFileError gg_topology_write_file(const char *datadir,
												  const GgTopologyFile *topo,
												  bool force, int *errentry);

extern void gg_topology_file_free(GgTopologyFile *topo);

#endif							/* GG_TOPOLOGY_FILE_H */
