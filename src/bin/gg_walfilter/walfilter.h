/*-------------------------------------------------------------------------
 *
 * walfilter.h
 *		Shared types and prototypes for gg_walfilter.
 *
 * gg_walfilter rewrites selected WAL records into same-length XLOG_NOOP
 * records (the walbouncer technique, applied to archived segments instead
 * of a streaming proxy).  See gg_walfilter.c for the tool-level overview.
 *
 * The segment walker here is deliberately a hand-written mirror of the
 * backend's DecodeXLogRecord()/XLogReadRecord() rather than a consumer of
 * xlogreader.c: the filter must decide and rewrite a record that spills
 * into the NEXT segment from its in-segment prefix alone (the next segment
 * is by design not fetched yet), and it must map records back to file
 * offsets for the in-place rewrite -- both outside xlogreader's contract.
 *
 * Portions Copyright (c) 2026, HashData Technology Limited.
 *
 * src/bin/gg_walfilter/walfilter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GG_WALFILTER_H
#define GG_WALFILTER_H

#include "access/xlogdefs.h"
#include "access/xlogrecord.h"
#include "storage/relfilenode.h"

/*
 * Upper bound on a record's header region (the XLogRecord header plus the
 * block-reference/data headers, before any block data): SizeOfXLogRecord +
 * (XLR_MAX_BLOCK_ID + 1) * MaxSizeOfXLogRecordBlockHeader + the origin and
 * long-data headers comes to well under 1024 bytes.  A record whose decode
 * asks for more is corrupt and fails closed.
 */
#define WF_HDR_MAX			1024

/* Content offset sentinel: no further record in this segment. */
#define WF_OFF_END			0xFFFFFFFFU

/* How many boundary states the state file keeps (restartpoint refetch). */
#define WF_STATE_KEEP		64

/* How many preceding segments the lookback path will fetch. */
#define WF_MAX_LOOKBACK		32

#define WF_HALT_FILE		"HALT"

/*
 * How the tool reports fatal problems: `restore` fails closed with an
 * explicit note and writes the sticky HALT sentinel on a forbidden remap;
 * the other subcommands just print and exit 1.
 */
typedef enum WfErrorMode
{
	WF_ERRMODE_PLAIN,			/* "gg_walfilter: <msg>", exit 1 */
	WF_ERRMODE_RESTORE,			/* "... -- failing closed ...", exit 1 */
} WfErrorMode;

/*
 * One WAL segment held fully in memory.  "Content offsets" are file offsets
 * that never point inside a page header (they may sit exactly on a page
 * boundary only transiently, between a skip and the following align).
 */
typedef struct WfSegment
{
	char		name[25];		/* 24-char segment file name + NUL */
	uint8	   *buf;			/* the whole segment, seg_size bytes */
	uint32		seg_size;
	uint32		blcksz;
	TimeLineID	tli;
	XLogRecPtr	start_lsn;
	uint32		first_rem_len;	/* xlp_rem_len of the first page */
	bool		first_is_overwrite; /* XLP_FIRST_IS_OVERWRITE_CONTRECORD */
	uint32		first_content;	/* first content offset (past long header) */
} WfSegment;

/*
 * A decoded record header: everything the filter decision needs, no data.
 */
typedef struct WfRecordBlock
{
	RelFileNode rnode;
	uint8		fork;
	BlockNumber blkno;
} WfRecordBlock;

typedef struct WfRecord
{
	uint32		off;			/* content offset of the record start */
	XLogRecPtr	lsn;
	uint32		tot_len;
	TransactionId xid;
	XLogRecPtr	prev;
	uint8		info;
	uint8		rmid;
	pg_crc32c	crc;
	int			nblocks;
	WfRecordBlock blocks[XLR_MAX_BLOCK_ID + 1];
	uint32		main_data_off;	/* content offset within the record */
	uint32		main_data_len;
} WfRecord;

/*
 * Page-header verdict while walking record content across pages.
 */
typedef enum WfPageStatus
{
	WF_PAGE_OK,
	WF_PAGE_STALE,				/* recycled page: valid end of recorded WAL */
	WF_PAGE_OVERWRITE,			/* XLP_FIRST_IS_OVERWRITE_CONTRECORD */
} WfPageStatus;

/*
 * Iterator over the (file_offset, length) runs covering n content bytes,
 * validating every page header crossed.  Structural contradictions are
 * terminal (wf_format_error); an overwrite-contrecord page is reported to
 * the caller when overwrite_ok is set and terminal otherwise.
 */
typedef struct WfRunIter
{
	WfSegment  *seg;
	uint32		off;
	uint32		n;
	int64		expect_rem;		/* record bytes remaining at off, or -1 */
	bool		overwrite_ok;
	uint32		ovw_page;		/* page offset that carried the flag */
} WfRunIter;

typedef enum WfRunStatus
{
	WF_RUN_YIELD,
	WF_RUN_DONE,
	WF_RUN_OVERWRITE,
} WfRunStatus;

/*
 * Boundary-continuation state for one segment boundary.  Only the leading
 * bytes of a NOOP replacement's spilled tail are non-zero (at most 29),
 * so the entry is a short prefix plus the total spill length.  length 0
 * means "known clean": the record spilling over this boundary was NOT
 * filtered and its continuation passes through unmodified.
 */
#define WF_SPILL_PREFIX_MAX 64

typedef struct WfSpill
{
	uint32		length;
	uint32		prefix_len;
	uint8		prefix[WF_SPILL_PREFIX_MAX];
} WfSpill;

typedef enum WfStateLookup
{
	WF_STATE_MISSING,			/* unknown boundary: lookback */
	WF_STATE_CLEAN,				/* known clean: pass the continuation through */
	WF_STATE_SPILL,				/* overwrite the continuation with the spill */
} WfStateLookup;

typedef struct WfStateEntry
{
	char		segname[25];
	WfSpill		spill;
} WfStateEntry;

typedef struct WfState
{
	char		dir[MAXPGPATH]; /* empty = no persistence */
	int			nentries;
	WfStateEntry entries[WF_STATE_KEEP + 1];
} WfState;

/*
 * Filter rules.  mapped_oids/mapped_filenodes are parallel arrays;
 * mapped_known[i] says whether mapped_filenodes[i] currently holds a
 * resolved relfilenode (from pg_filenode.map, updated by shared relmap
 * update records passing through).
 */
typedef struct WfRules
{
	RelFileNode *relfilenodes;
	int			nrelfilenodes;
	Oid		   *databases;
	int			ndatabases;
	Oid		   *tablespaces;
	int			ntablespaces;
	Oid		   *mapped_oids;
	Oid		   *mapped_filenodes;
	bool	   *mapped_known;
	int			nmapped;
	bool		remap_guard;
} WfRules;

typedef struct WfFilterResult
{
	uint64		records;
	uint64		rewritten;
	XLogRecPtr *rewritten_lsns;
	uint64		nlsns_alloc;
	bool		has_exit_spill;
	WfSpill		exit_spill;
} WfFilterResult;

/* Per-record callback for `inspect`. */
typedef void (*wf_report_fn) (WfSegment *seg, WfRecord *rec, bool matched,
							  void *arg);

/* A fetcher for the lookback path: segment bytes, or NULL when unfetchable. */
typedef uint8 *(*wf_fetch_fn) (const char *name, size_t *len, void *arg);

/* gg_walfilter.c */
extern WfErrorMode wf_error_mode;
extern const char *wf_halt_dir;	/* set in restore mode: where HALT goes */

extern void wf_log(const char *fmt,...) pg_attribute_printf(1, 2);
extern void wf_format_error(const char *fmt,...) pg_attribute_printf(1, 2) pg_attribute_noreturn();
extern void wf_forbidden_remap(const char *fmt,...) pg_attribute_printf(1, 2) pg_attribute_noreturn();

/* segment.c */
extern bool wf_segno_from_name(const char *name, TimeLineID *tli,
							   uint64 *hi, uint64 *lo);
extern bool wf_seg_prev_name(const char *name, uint32 seg_size, char *out);
extern void wf_seg_next_name(const char *name, uint32 seg_size, char *out);
extern const char *wf_lsn_str(XLogRecPtr lsn);

extern void wf_seg_open(WfSegment *seg, uint8 *buf, size_t len,
						const char *filename);
extern uint32 wf_page_header_size(WfSegment *seg, uint32 page_off);
extern WfPageStatus wf_page_ok(WfSegment *seg, uint32 page_off,
							   bool expect_cont, int64 expect_rem);
extern uint32 wf_content_available(WfSegment *seg, uint32 off);
extern void wf_run_init(WfRunIter *it, WfSegment *seg, uint32 off, uint32 n,
						int64 expect_rem, bool overwrite_ok);
extern WfRunStatus wf_run_next(WfRunIter *it, uint32 *foff, uint32 *len);
extern void wf_read_content(WfSegment *seg, uint32 off, uint32 n,
							int64 expect_rem, uint8 *out);
extern uint32 wf_read_span(WfSegment *seg, uint32 off, uint32 n, uint8 *out);
extern uint32 wf_skip_content(WfSegment *seg, uint32 off, uint32 n,
							  int64 expect_rem);
extern uint32 wf_align_next_record(WfSegment *seg, uint32 off);
extern bool wf_parse_record_at(WfSegment *seg, uint32 off,
							   const uint8 *extra, uint32 extralen,
							   WfRecord *rec);

/* filter.c */
extern pg_crc32c wf_record_crc(const uint8 *content, uint32 len);
extern void wf_rules_init(WfRules *rules);
extern bool wf_rules_empty(const WfRules *rules);
extern void wf_rules_add_mapped_oid(WfRules *rules, Oid oid);
extern void wf_rules_resolve_mapped(WfRules *rules, const char *datadir);
extern bool wf_record_matches(WfSegment *seg, const WfRules *rules,
							  const WfRecord *rec);
extern bool wf_truncate_target(WfSegment *seg, const WfRecord *rec,
							   RelFileNode *rnode, BlockNumber *blkno);
extern uint8 *wf_build_noop_record(const WfRecord *rec);
extern void wf_spill_from_replacement(const uint8 *content, uint32 tot_len,
									  uint32 rem_len, WfSpill *out);
extern void wf_filter_segment(WfSegment *seg, WfRules *rules,
							  const WfSpill *entry_spill,
							  wf_report_fn report, void *report_arg,
							  bool dry_run, WfFilterResult *res);
extern bool wf_resolve_entry_spill(WfSegment *seg, WfRules *rules,
								   wf_fetch_fn fetch, void *fetch_arg,
								   WfSpill *out);

/* state.c */
extern void wf_state_load(WfState *state, const char *state_dir);
extern WfStateLookup wf_state_get(WfState *state, const char *segname,
								  WfSpill *out);
extern void wf_state_set(WfState *state, const char *segname,
						 const WfSpill *spill);
extern void wf_state_delete(WfState *state, const char *segname);
extern void wf_state_save(WfState *state);

/* fetch.c */
extern char *wf_expand_fetch(const char *template, const char *filename,
							 const char *dest);
extern bool wf_run_fetch(const char *template, const char *filename,
						 const char *dest, bool verbose);
extern uint8 *wf_fetch_lookback(const char *template, const char *scratch_dir,
								const char *name, size_t *len, bool verbose);
extern void wf_write_and_install(const uint8 *data, size_t len,
								 const char *dest);
extern uint8 *wf_read_file(const char *path, size_t *len);

#endif							/* GG_WALFILTER_H */
