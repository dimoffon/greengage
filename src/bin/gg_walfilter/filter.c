/*-------------------------------------------------------------------------
 *
 * filter.c
 *		gg_walfilter's filter rules, the same-length XLOG_NOOP rewrite,
 *		the per-segment record walk, and the boundary lookback.
 *
 * Portions Copyright (c) 2026, HashData Technology Limited.
 *
 * src/bin/gg_walfilter/filter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "access/rmgr.h"
#include "access/xlog_internal.h"
#include "catalog/pg_control.h"
#include "catalog/pg_tablespace_d.h"
#include "catalog/storage_xlog.h"
#include "common/fe_memutils.h"
#include "utils/relmapper.h"

#include "walfilter.h"

/*
 * CRC of a fully assembled record: the data after the header first, then
 * the header up to xl_crc -- mirrors ValidXLogRecord().
 */
pg_crc32c
wf_record_crc(const uint8 *content, uint32 len)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, content + SizeOfXLogRecord, len - SizeOfXLogRecord);
	COMP_CRC32C(crc, content, offsetof(XLogRecord, xl_crc));
	FIN_CRC32C(crc);
	return crc;
}

/* ---------------------------------------------------------------------------
 * Filter rules
 * ---------------------------------------------------------------------------
 */

void
wf_rules_init(WfRules *rules)
{
	memset(rules, 0, sizeof(*rules));
}

bool
wf_rules_empty(const WfRules *rules)
{
	return rules->nrelfilenodes == 0 && rules->ndatabases == 0 &&
		rules->ntablespaces == 0 && rules->nmapped == 0;
}

void
wf_rules_add_mapped_oid(WfRules *rules, Oid oid)
{
	int			i;

	for (i = 0; i < rules->nmapped; i++)
		if (rules->mapped_oids[i] == oid)
			return;
	rules->mapped_oids = pg_realloc(rules->mapped_oids,
									(rules->nmapped + 1) * sizeof(Oid));
	rules->mapped_filenodes = pg_realloc(rules->mapped_filenodes,
										 (rules->nmapped + 1) * sizeof(Oid));
	rules->mapped_known = pg_realloc(rules->mapped_known,
									 (rules->nmapped + 1) * sizeof(bool));
	rules->mapped_oids[rules->nmapped] = oid;
	rules->mapped_filenodes[rules->nmapped] = InvalidOid;
	rules->mapped_known[rules->nmapped] = false;
	rules->nmapped++;
}

/*
 * Parse global/pg_filenode.map.  Handles the stock 512-byte (62 mappings)
 * and Greengage 1024-byte (126 mappings) layouts; validates magic and CRC.
 * The parse is size-driven rather than an overlay of RelMapFile, which
 * only describes the 1024-byte layout.
 */
static void
read_filenode_map(const char *path, RelMapping **entries, uint32 *num)
{
	uint8	   *raw;
	size_t		len;
	int32		magic;
	uint32		nmap;
	uint32		max_mappings;
	uint32		crc_off;
	pg_crc32c	crc,
				file_crc;

	raw = wf_read_file(path, &len);
	if (raw == NULL)
		wf_format_error("cannot read %s: %s", path, strerror(errno));
	if (len != 512 && len != 1024)
		wf_format_error("%s: unexpected size %zu", path, len);
	memcpy(&magic, raw, sizeof(int32));
	memcpy(&nmap, raw + 4, sizeof(uint32));
	max_mappings = (len - 16) / 8;
	if (magic != RELMAPPER_FILEMAGIC || nmap > max_mappings)
		wf_format_error("%s: bad magic 0x%X or mapping count %u",
						path, magic, nmap);
	crc_off = 8 + max_mappings * 8;
	memcpy(&file_crc, raw + crc_off, sizeof(pg_crc32c));
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, raw, crc_off);
	FIN_CRC32C(crc);
	if (crc != file_crc)
		wf_format_error("%s: CRC mismatch", path);

	*entries = pg_malloc(nmap * sizeof(RelMapping) + 1);
	memcpy(*entries, raw + 8, nmap * sizeof(RelMapping));
	*num = nmap;
	pg_free(raw);
}

void
wf_rules_resolve_mapped(WfRules *rules, const char *datadir)
{
	char		path[MAXPGPATH];
	RelMapping *entries;
	uint32		num;
	int			i;
	uint32		j;

	if (rules->nmapped == 0)
		return;
	snprintf(path, sizeof(path), "%s/global/%s", datadir, RELMAPPER_FILENAME);
	read_filenode_map(path, &entries, &num);
	for (i = 0; i < rules->nmapped; i++)
	{
		bool		found = false;

		for (j = 0; j < num; j++)
		{
			if (entries[j].mapoid == rules->mapped_oids[i])
			{
				rules->mapped_filenodes[i] = entries[j].mapfilenode;
				rules->mapped_known[i] = true;
				found = true;
				break;
			}
		}
		if (!found)
			wf_log("warning: mapped catalog %u is not in %s; it will not be filtered",
				   rules->mapped_oids[i], path);
	}
	pg_free(entries);
}

/*
 * A shared relmap update record is passing through: enforce the
 * forbidden-remap guard and track the new mapping from here on.
 */
static void
wf_apply_relmap_update(WfRules *rules, const RelMapping *mappings, uint32 num)
{
	uint32		j;
	int			i;

	for (j = 0; j < num; j++)
	{
		for (i = 0; i < rules->nmapped; i++)
		{
			if (rules->mapped_oids[i] != mappings[j].mapoid)
				continue;
			if (rules->remap_guard && rules->mapped_known[i] &&
				rules->mapped_filenodes[i] != mappings[j].mapfilenode)
				wf_forbidden_remap("protected catalog %u was rebuilt upstream (relfilenode %u -> %u): VACUUM FULL / CLUSTER / REINDEX / TRUNCATE of a cluster-topology catalog is incompatible with an attached DR replica; re-create this DR node from a fresh base backup and re-run the topology seed",
								   mappings[j].mapoid,
								   rules->mapped_filenodes[i],
								   mappings[j].mapfilenode);
			rules->mapped_filenodes[i] = mappings[j].mapfilenode;
			rules->mapped_known[i] = true;
			break;
		}
	}
}

static bool
wf_block_matches(const WfRules *rules, const RelFileNode *rnode)
{
	int			i;

	for (i = 0; i < rules->nrelfilenodes; i++)
		if (RelFileNodeEquals(rules->relfilenodes[i], *rnode))
			return true;
	for (i = 0; i < rules->ndatabases; i++)
		if (rules->databases[i] == rnode->dbNode)
			return true;
	for (i = 0; i < rules->ntablespaces; i++)
		if (rules->tablespaces[i] == rnode->spcNode)
			return true;
	if (rnode->spcNode == GLOBALTABLESPACE_OID && rnode->dbNode == 0)
	{
		for (i = 0; i < rules->nmapped; i++)
			if (rules->mapped_known[i] &&
				rules->mapped_filenodes[i] == rnode->relNode)
				return true;
	}
	return false;
}

/*
 * Decode the relation an XLOG_SMGR_TRUNCATE record targets.
 *
 * This record names its relation in the main data rather than in a block
 * reference -- RelationTruncate() registers no buffers -- so the block-tag
 * walk below cannot see it.  Only the record's leading bytes are needed: with
 * no block references the main data begins within the first few dozen bytes,
 * so this resolves even for a record that spills past the end of the segment.
 *
 * Returns false only when those bytes are genuinely unavailable (the record
 * starts too close to the segment end); a malformed payload is corruption and
 * is terminal.  Callers decide what "unavailable" means for them.
 */
bool
wf_truncate_target(WfSegment *seg, const WfRecord *rec, RelFileNode *rnode,
				   BlockNumber *blkno)
{
	uint32		need;
	uint8	   *content;

	if (rec->rmid != RM_SMGR_ID ||
		(rec->info & XLR_RMGR_INFO_MASK) != XLOG_SMGR_TRUNCATE)
		return false;
	if (rec->main_data_len < sizeof(xl_smgr_truncate))
		wf_format_error("%s: smgr truncate record at %s carries %u payload byte(s), expected %zu",
						seg->name, wf_lsn_str(rec->lsn), rec->main_data_len,
						sizeof(xl_smgr_truncate));

	need = rec->main_data_off + sizeof(xl_smgr_truncate);
	if (need > wf_content_available(seg, rec->off))
		return false;

	content = pg_malloc(need);
	wf_read_content(seg, rec->off, need, rec->tot_len, content);
	memcpy(blkno, content + rec->main_data_off + offsetof(xl_smgr_truncate, blkno),
		   sizeof(BlockNumber));
	memcpy(rnode, content + rec->main_data_off + offsetof(xl_smgr_truncate, rnode),
		   sizeof(RelFileNode));
	pg_free(content);
	return true;
}

/*
 * Does this XLOG_SMGR_TRUNCATE target a relation the rules protect?
 *
 * XLOG_SMGR_TRUNCATE names its relation in the payload rather than in a block
 * reference, so the block-reference rules cannot see it.  Without this, an
 * upstream VACUUM that truncates a protected relation would truncate the
 * replica's copy -- which the rules exist to keep different, and which
 * legitimately has its own page count, so upstream's truncation point does not
 * apply to it.
 *
 * The backend had the same rule until P7, in smgr_redo() via
 * DRRedoShouldFilterRelFileNode(); it went with the rest of the DR redo filter
 * once the cluster topology stopped living in a replicated catalog.  This tool
 * keeps it because its rules are caller-supplied and may still name a relation
 * the caller wants left alone.
 */
static bool
wf_truncate_matches(WfSegment *seg, const WfRules *rules, const WfRecord *rec)
{
	RelFileNode rnode;
	BlockNumber blkno;

	if (!wf_truncate_target(seg, rec, &rnode, &blkno))
	{
		/*
		 * The target is unreadable from this segment alone.  Under the guard,
		 * refuse rather than let a truncation of an unknown relation through;
		 * a ~50-byte record landing on the boundary is vanishingly rare, and
		 * this keeps the guard sound.  Same posture as the relmap case above.
		 */
		if (rules->remap_guard)
			wf_format_error("%s: smgr truncate at %s spans the segment boundary; cannot tell whether it targets a protected catalog",
							seg->name, wf_lsn_str(rec->lsn));
		return false;
	}
	return wf_block_matches(rules, &rnode);
}

/*
 * A record is rewritten iff it has at least one block reference and EVERY
 * block reference matches some rule; records with no block references
 * always pass -- that is also a commit record's shape.  The one exception is
 * XLOG_SMGR_TRUNCATE, which has no block references but still names a relation
 * (see wf_truncate_matches).
 */
bool
wf_record_matches(WfSegment *seg, const WfRules *rules, const WfRecord *rec)
{
	int			i;

	if (rec->rmid == RM_SMGR_ID &&
		(rec->info & XLR_RMGR_INFO_MASK) == XLOG_SMGR_TRUNCATE)
		return wf_truncate_matches(seg, rules, rec);

	if (rec->nblocks == 0)
		return false;
	for (i = 0; i < rec->nblocks; i++)
		if (!wf_block_matches(rules, &rec->blocks[i].rnode))
			return false;
	return true;
}

/*
 * For an XLOG_RELMAP_UPDATE record that fits in this segment: decode
 * (dbid, mappings).  The caller frees *entries.
 */
static void
parse_relmap_mappings(WfSegment *seg, const WfRecord *rec, Oid *dbid,
					  RelMapping **entries, uint32 *num)
{
	uint8	   *content;
	const uint8 *data;
	uint32		data_len;
	int32		nbytes;
	uint32		plen;
	uint32		nmap;

	content = pg_malloc(rec->tot_len);
	wf_read_content(seg, rec->off, rec->tot_len, rec->tot_len, content);
	data = content + rec->main_data_off;
	data_len = rec->main_data_len;
	if (data_len < 12)
		wf_format_error("%s: relmap update record at %s is truncated",
						seg->name, wf_lsn_str(rec->lsn));
	memcpy(dbid, data, sizeof(Oid));
	memcpy(&nbytes, data + 8, sizeof(int32));
	plen = (nbytes < 0) ? 0 : Min((uint32) nbytes, data_len - 12);
	if (plen < 8)
		wf_format_error("%s: relmap payload at %s is truncated",
						seg->name, wf_lsn_str(rec->lsn));
	memcpy(&nmap, data + 12 + 4, sizeof(uint32));
	if ((uint64) 8 + (uint64) nmap * 8 > plen)
		wf_format_error("%s: relmap payload at %s overflows",
						seg->name, wf_lsn_str(rec->lsn));
	*entries = pg_malloc(nmap * sizeof(RelMapping) + 1);
	memcpy(*entries, data + 12 + 8, nmap * sizeof(RelMapping));
	*num = nmap;
	pg_free(content);
}

/* ---------------------------------------------------------------------------
 * The rewrite: record -> same-length XLOG_NOOP
 * ---------------------------------------------------------------------------
 */

/*
 * Replacement content for a filtered record: identical framing (xl_tot_len /
 * xl_prev / xl_xid), rmgr XLOG + XLOG_NOOP, and the block references
 * replaced by an all-zeros main-data payload.  Dropping the block references
 * is required, not cosmetic: xlog_redo asserts that non-FPI XLOG records
 * carry none, and swapping xl_info also clears XLR_CHECK_CONSISTENCY (the
 * record no longer describes those pages).
 */
uint8 *
wf_build_noop_record(const WfRecord *rec)
{
	uint32		payload_area = rec->tot_len - SizeOfXLogRecord;
	uint8	   *content;
	XLogRecord *r;

	if (payload_area < 2)
		wf_format_error("record at %s is too short to neutralize",
						wf_lsn_str(rec->lsn));
	content = pg_malloc0(rec->tot_len);
	r = (XLogRecord *) content;
	r->xl_tot_len = rec->tot_len;
	r->xl_xid = rec->xid;
	r->xl_prev = rec->prev;
	r->xl_info = XLOG_NOOP;
	r->xl_rmid = RM_XLOG_ID;
	if (payload_area - 2 <= 255)
	{
		content[SizeOfXLogRecord] = XLR_BLOCK_ID_DATA_SHORT;
		content[SizeOfXLogRecord + 1] = (uint8) (payload_area - 2);
	}
	else
	{
		uint32		dlen = payload_area - 5;

		content[SizeOfXLogRecord] = XLR_BLOCK_ID_DATA_LONG;
		memcpy(content + SizeOfXLogRecord + 1, &dlen, sizeof(uint32));
	}
	r->xl_crc = wf_record_crc(content, rec->tot_len);
	return content;
}

/*
 * State entry for the next segment: the trailing rem_len bytes of the
 * replacement content, stored as (non-zero prefix, total length).
 */
void
wf_spill_from_replacement(const uint8 *content, uint32 tot_len,
						  uint32 rem_len, WfSpill *out)
{
	const uint8 *tail = content + tot_len - rem_len;
	uint32		nz = rem_len;

	while (nz > 0 && tail[nz - 1] == 0)
		nz--;
	if (nz > WF_SPILL_PREFIX_MAX)
		wf_format_error("replacement spill prefix is unexpectedly long (%u bytes)",
						nz);
	out->length = rem_len;
	out->prefix_len = nz;
	memcpy(out->prefix, tail, nz);
}

/* Materialize n leading bytes of a spill: the prefix padded with zeros. */
static uint8 *
spill_bytes(const WfSpill *spill, uint32 n)
{
	uint8	   *out = pg_malloc0(n + 1);
	uint32		take = Min(n, spill->prefix_len);

	memcpy(out, spill->prefix, take);
	return out;
}

/* ---------------------------------------------------------------------------
 * Core: filter one in-memory segment
 * ---------------------------------------------------------------------------
 */

static void
result_add_lsn(WfFilterResult *res, XLogRecPtr lsn)
{
	if (res->rewritten >= res->nlsns_alloc)
	{
		res->nlsns_alloc = res->nlsns_alloc ? res->nlsns_alloc * 2 : 8;
		res->rewritten_lsns = pg_realloc(res->rewritten_lsns,
										 res->nlsns_alloc * sizeof(XLogRecPtr));
	}
	res->rewritten_lsns[res->rewritten] = lsn;
}

/*
 * Write len bytes over the content at off (pages already validated by the
 * caller's walk over the same span).
 */
static void
write_runs(WfSegment *seg, uint32 off, const uint8 *data, uint32 len,
		   int64 expect_rem)
{
	WfRunIter	it;
	uint32		foff,
				rlen;
	uint32		pos = 0;

	wf_run_init(&it, seg, off, len, expect_rem, false);
	while (wf_run_next(&it, &foff, &rlen) == WF_RUN_YIELD)
	{
		memcpy(seg->buf + foff, data + pos, rlen);
		pos += rlen;
	}
}

/*
 * Rewrite matching records of seg in place.  entry_spill: NULL when the
 * leading continuation (if any) belongs to an unfiltered record; else the
 * replacement tail to write over it.  The caller persists res->exit_spill
 * as the NEXT segment's entry state.
 *
 * An overwrite-contrecord page (XLP_FIRST_IS_OVERWRITE_CONTRECORD) aborts
 * the record being walked: its bytes are dead space that recovery skips,
 * so the walk leaves them untouched and restarts at that page, requiring
 * the record found there to be the XLOG_OVERWRITE_CONTRECORD marker.  (The
 * xl_prev chain check is suspended for that one record: its predecessor
 * may live in an earlier segment.)
 */
void
wf_filter_segment(WfSegment *seg, WfRules *rules, const WfSpill *entry_spill,
				  wf_report_fn report, void *report_arg, bool dry_run,
				  WfFilterResult *res)
{
	uint32		off = seg->first_content;
	bool		expect_ovw = seg->first_is_overwrite;
	bool		have_prev = false;
	XLogRecPtr	prev_lsn = 0;

	memset(res, 0, sizeof(*res));

	/* --- 1. leading continuation of a record begun in an earlier segment --- */
	if (seg->first_rem_len > 0)
	{
		uint32		in_this = Min(seg->first_rem_len,
								  wf_content_available(seg, off));
		bool		ovw = false;
		uint32		ovw_page = 0;
		WfRunIter	it;
		WfRunStatus st;
		uint32		foff,
					len;
		uint8	   *sp = NULL;
		uint32		pos = 0;

		if (entry_spill != NULL && entry_spill->length != seg->first_rem_len)
			wf_format_error("%s: boundary state says %u continuation bytes but the page header says %u remain (stale state)",
							seg->name, entry_spill->length, seg->first_rem_len);
		if (entry_spill != NULL && !dry_run)
			sp = spill_bytes(entry_spill, in_this);

		wf_run_init(&it, seg, off, in_this, seg->first_rem_len, true);
		while ((st = wf_run_next(&it, &foff, &len)) == WF_RUN_YIELD)
		{
			if (sp != NULL)
				memcpy(seg->buf + foff, sp + pos, len);
			pos += len;
			off = foff + len;
		}
		if (sp != NULL)
			pg_free(sp);
		if (st == WF_RUN_OVERWRITE)
		{
			ovw = true;
			ovw_page = it.ovw_page;
		}

		if (ovw)
		{
			/* the spilled-into record was aborted: no exit spill from it */
			off = ovw_page + wf_page_header_size(seg, ovw_page);
			expect_ovw = true;
		}
		else
		{
			if (entry_spill != NULL && entry_spill->length > in_this)
			{
				/*
				 * Still not finished (a record larger than a whole segment):
				 * the rest is all zeros by now.
				 */
				res->has_exit_spill = true;
				res->exit_spill.length = entry_spill->length - in_this;
				res->exit_spill.prefix_len = 0;
			}
			if (seg->first_rem_len > in_this)
				return;			/* the whole segment is continuation */
			off = wf_align_next_record(seg, off);
		}
	}

	/* --- 2. walk the fresh records --- */
	while (off != WF_OFF_END)
	{
		WfRecord	rec;
		uint32		avail,
					in_seg,
					next_off;
		bool		ovw = false;
		uint32		ovw_page = 0;
		bool		matched;
		WfRunIter	it;
		WfRunStatus st;
		uint32		foff,
					len;

		if (!wf_parse_record_at(seg, off, NULL, 0, &rec))
			break;

		if (expect_ovw)
		{
			if (!(rec.rmid == RM_XLOG_ID &&
				  (rec.info & XLR_RMGR_INFO_MASK) == XLOG_OVERWRITE_CONTRECORD))
				wf_format_error("%s: expected an overwrite-contrecord record at %s after an aborted continuation",
								seg->name, wf_lsn_str(rec.lsn));
			expect_ovw = false;
		}
		else if (have_prev && rec.prev != prev_lsn)
			wf_format_error("%s: record at %s: xl_prev %s does not point at the previous record (%s)",
							seg->name, wf_lsn_str(rec.lsn),
							wf_lsn_str(rec.prev), wf_lsn_str(prev_lsn));

		avail = wf_content_available(seg, rec.off);
		in_seg = Min(rec.tot_len, avail);

		/* validation walk over the in-segment span */
		next_off = rec.off;
		wf_run_init(&it, seg, rec.off, in_seg, rec.tot_len, true);
		while ((st = wf_run_next(&it, &foff, &len)) == WF_RUN_YIELD)
			next_off = foff + len;
		if (st == WF_RUN_OVERWRITE)
		{
			ovw = true;
			ovw_page = it.ovw_page;
		}
		if (ovw)
		{
			off = ovw_page + wf_page_header_size(seg, ovw_page);
			expect_ovw = true;
			continue;
		}

		res->records++;
		have_prev = true;
		prev_lsn = rec.lsn;

		/* Shared relmap updates: guard + track protected relfilenodes. */
		if (rec.rmid == RM_RELMAP_ID &&
			(rec.info & XLR_RMGR_INFO_MASK) == XLOG_RELMAP_UPDATE)
		{
			if (rec.tot_len <= avail)
			{
				Oid			dbid;
				RelMapping *entries;
				uint32		num;

				parse_relmap_mappings(seg, &rec, &dbid, &entries, &num);
				if (dbid == 0)
					wf_apply_relmap_update(rules, entries, num);
				pg_free(entries);
			}
			else if (rules->remap_guard)
			{
				/*
				 * A relmap update spanning a segment boundary is vanishingly
				 * rare; refusing keeps the guard sound.
				 */
				wf_format_error("%s: shared relmap update at %s spans the segment boundary; cannot verify the protected catalogs",
								seg->name, wf_lsn_str(rec.lsn));
			}
		}

		matched = wf_record_matches(seg, rules, &rec);
		if (report != NULL)
			report(seg, &rec, matched, report_arg);

		if (matched)
		{
			uint8	   *replacement;
			uint32		spilled;

			if (rec.tot_len <= avail)
			{
				/* Before touching anything, prove we parsed it correctly. */
				uint8	   *orig = pg_malloc(rec.tot_len);

				wf_read_content(seg, rec.off, rec.tot_len, rec.tot_len, orig);
				if (wf_record_crc(orig, rec.tot_len) != rec.crc)
					wf_format_error("%s: record at %s fails its CRC before rewrite -- refusing to touch a segment we may be misparsing",
									seg->name, wf_lsn_str(rec.lsn));
				pg_free(orig);
			}
			replacement = wf_build_noop_record(&rec);
			result_add_lsn(res, rec.lsn);
			res->rewritten++;
			if (dry_run)
				spilled = rec.tot_len > avail ? rec.tot_len - avail : 0;
			else
			{
				write_runs(seg, rec.off, replacement, in_seg, rec.tot_len);
				spilled = rec.tot_len - in_seg;
			}
			if (spilled > 0)
			{
				wf_spill_from_replacement(replacement, rec.tot_len, spilled,
										  &res->exit_spill);
				res->has_exit_spill = true;
				pg_free(replacement);
				return;
			}
			pg_free(replacement);
		}
		else if (rec.tot_len > avail)
		{
			/*
			 * An unfiltered record spills onward: the state for the next
			 * segment stays "clean".
			 */
			return;
		}

		off = next_off;
		if (rec.rmid == RM_XLOG_ID &&
			(rec.info & XLR_RMGR_INFO_MASK) == XLOG_SWITCH)
			break;				/* rest of the segment is dead space */
		off = wf_align_next_record(seg, off);
	}
}

/* ---------------------------------------------------------------------------
 * Lookback: derive the entry state without a saved boundary entry, by
 * re-fetching preceding segment(s) from the archive and re-deriving the
 * spilled record's filter decision.  Deterministic: reproduces exactly what
 * a previous invocation did (or would have done).
 * ---------------------------------------------------------------------------
 */

/*
 * Concatenated leading continuation content of consecutive following
 * segments -- used to complete a record header that crosses a boundary.
 * Stops as soon as a segment's continuation is not fully consumed (the
 * result must stay contiguous).
 */
static uint32
continuation_bytes(WfSegment **segs, int nsegs, uint8 *out, uint32 limit)
{
	uint32		total = 0;
	int			i;

	for (i = 0; i < nsegs; i++)
	{
		WfSegment  *s = segs[i];
		uint32		hav = Min(s->first_rem_len,
							  wf_content_available(s, s->first_content));
		uint32		take = Min(hav, limit - total);

		if (take == 0)
			break;
		wf_read_span(s, s->first_content, take, out + total);
		total += take;
		if (take < hav)
			break;				/* truncated by limit: do not append more */
		if (s->first_rem_len <= wf_content_available(s, s->first_content))
			break;				/* record ends inside this segment */
	}
	return total;
}

/*
 * Entry state for seg; false = pass its continuation through.
 */
bool
wf_resolve_entry_spill(WfSegment *seg, WfRules *rules, wf_fetch_fn fetch,
					   void *fetch_arg, WfSpill *out)
{
	WfSegment	chain[WF_MAX_LOOKBACK];
	int			nchain = 0;
	WfSegment  *base = NULL;
	WfSegment  *following[WF_MAX_LOOKBACK + 1];
	int			nfollowing = 0;
	char		name[25];
	uint8		extra[4096];
	uint32		extralen;
	uint32		off;
	WfRecord	rec,
				spiller;
	bool		have_spiller = false;
	bool		result = false;
	uint8	   *replacement;
	int			i;

	strlcpy(name, seg->name, sizeof(name));
	while (nchain < WF_MAX_LOOKBACK)
	{
		char		prev[25];
		bool		have_name;
		uint8	   *raw = NULL;
		size_t		rawlen = 0;

		have_name = wf_seg_prev_name(name, seg->seg_size, prev);
		if (have_name)
			raw = fetch(prev, &rawlen, fetch_arg);
		if (raw == NULL)
		{
			wf_log("warning: %s starts mid-record and %s is not fetchable; passing the continuation through unmodified (normal at the very start of the archived history)",
				   seg->name, have_name ? prev : "<segment 0>");
			goto done;
		}
		memmove(&chain[1], &chain[0], nchain * sizeof(WfSegment));
		nchain++;
		wf_seg_open(&chain[0], raw, rawlen, prev);
		strlcpy(name, prev, sizeof(name));
		if (chain[0].first_rem_len <
			wf_content_available(&chain[0], chain[0].first_content))
		{
			base = &chain[0];	/* contains at least one fresh record start */
			break;
		}
	}
	if (base == NULL)
		wf_format_error("%s: no record start found within %d preceding segments",
						seg->name, WF_MAX_LOOKBACK);

	for (i = 1; i < nchain; i++)
		following[nfollowing++] = &chain[i];
	following[nfollowing++] = seg;
	extralen = continuation_bytes(following, nfollowing, extra, sizeof(extra));

	/* Find the last record that STARTS in base: that is the spiller. */
	off = base->first_content;
	if (base->first_rem_len > 0)
		off = wf_align_next_record(base,
								   wf_skip_content(base, off,
												   base->first_rem_len,
												   base->first_rem_len));
	while (off != WF_OFF_END)
	{
		uint32		avail;

		if (!wf_parse_record_at(base, off, extra, extralen, &rec))
			break;
		avail = wf_content_available(base, rec.off);
		if (rec.tot_len > avail)
		{
			spiller = rec;
			have_spiller = true;
			break;
		}
		off = wf_skip_content(base, rec.off, rec.tot_len, rec.tot_len);
		if (rec.rmid == RM_XLOG_ID &&
			(rec.info & XLR_RMGR_INFO_MASK) == XLOG_SWITCH)
			break;
		off = wf_align_next_record(base, off);
	}
	if (!have_spiller)
		wf_format_error("%s: its page header says it starts mid-record, but no record in %s spills forward",
						seg->name, base->name);

	if (!wf_record_matches(base, rules, &spiller))
		goto done;
	replacement = wf_build_noop_record(&spiller);
	if (seg->first_rem_len > spiller.tot_len)
		wf_format_error("%s: rem_len %u exceeds the spilling record's length %u",
						seg->name, seg->first_rem_len, spiller.tot_len);
	wf_spill_from_replacement(replacement, spiller.tot_len,
							  seg->first_rem_len, out);
	pg_free(replacement);
	result = true;

done:
	for (i = 0; i < nchain; i++)
		pg_free(chain[i].buf);
	return result;
}
