/*-------------------------------------------------------------------------
 *
 * relmapper.h
 *	  Catalog-to-filenode mapping
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/relmapper.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELMAPPER_H
#define RELMAPPER_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/* ----------------
 *		relmap-related XLOG entries
 * ----------------
 */

#define XLOG_RELMAP_UPDATE		0x00

typedef struct xl_relmap_update
{
	Oid			dbid;			/* database ID, or 0 for shared map */
	Oid			tsid;			/* database's tablespace, or pg_global */
	int32		nbytes;			/* size of relmap data */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} xl_relmap_update;

#define MinSizeOfRelmapUpdate offsetof(xl_relmap_update, data)

/* ----------------
 *		the on-disk layout of pg_filenode.map
 *
 * Exposed here (rather than kept private to relmapper.c) so that frontend
 * tools -- notably gg_walfilter -- can read the file and decode
 * XLOG_RELMAP_UPDATE payloads.
 * ----------------
 */

#define RELMAPPER_FILENAME		"pg_filenode.map"

#define RELMAPPER_FILEMAGIC		0x592717	/* version ID value */

/*
 * In Postgres, MAX_MAPPINGS is 62, but GPDB has exceeded this number due to
 * additional GPDB specific shared relations. Increased to 126 to occupy
 * exactly 1 kilobyte.
 *
 * New math: 126 * 8 + 16 = 1024
 */
#define MAX_MAPPINGS			126		/* 62 * 8 + 16 = 512 */

typedef struct RelMapping
{
	Oid			mapoid;			/* OID of a catalog */
	Oid			mapfilenode;	/* its filenode number */
} RelMapping;

typedef struct RelMapFile
{
	int32		magic;			/* always RELMAPPER_FILEMAGIC */
	int32		num_mappings;	/* number of valid RelMapping entries */
	RelMapping	mappings[MAX_MAPPINGS];
	pg_crc32c	crc;			/* CRC of all above */
	int32		pad;			/* to make the struct size be 512 exactly */
} RelMapFile;


extern Oid	RelationMapOidToFilenode(Oid relationId, bool shared);

extern Oid	RelationMapFilenodeToOid(Oid relationId, bool shared);

extern void RelationMapUpdateMap(Oid relationId, Oid fileNode, bool shared,
								 bool immediate);

extern void RelationMapRemoveMapping(Oid relationId);

extern void RelationMapInvalidate(bool shared);
extern void RelationMapInvalidateAll(void);

extern void AtCCI_RelationMap(void);
extern void AtEOXact_RelationMap(bool isCommit, bool isParallelWorker);
extern void AtPrepare_RelationMap(void);

extern void CheckPointRelationMap(void);

extern void RelationMapFinishBootstrap(void);

extern void RelationMapInitialize(void);
extern void RelationMapInitializePhase2(void);
extern void RelationMapInitializePhase3(void);

extern Size EstimateRelationMapSpace(void);
extern void SerializeRelationMap(Size maxSize, char *startAddress);
extern void RestoreRelationMap(char *startAddress);

extern void relmap_redo(XLogReaderState *record);
extern void relmap_desc(StringInfo buf, XLogReaderState *record);
extern const char *relmap_identify(uint8 info);

#endif							/* RELMAPPER_H */
