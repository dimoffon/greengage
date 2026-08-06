/*-------------------------------------------------------------------------
 *
 * segment.c
 *		WAL segment access for gg_walfilter: names, page-aware content
 *		walking, and record-header decoding.
 *
 * The record decoder is a deliberate mirror of the backend's
 * DecodeXLogRecord() (xlogreader.c), kept header-only: it never touches
 * block data, so it can decide the filter fate of a record from the prefix
 * that lives in this segment even when the record's tail spills into the
 * next, not-yet-fetched segment.
 *
 * Portions Copyright (c) 2026, HashData Technology Limited.
 *
 * src/bin/gg_walfilter/segment.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include "access/xlog_internal.h"
#include "common/fe_memutils.h"

#include "walfilter.h"

/*
 * %X/%X formatting with a small ring of static buffers so that it can
 * appear more than once in a single format string.
 */
const char *
wf_lsn_str(XLogRecPtr lsn)
{
	static char bufs[4][32];
	static int	next = 0;
	char	   *buf = bufs[next];

	next = (next + 1) % 4;
	snprintf(buf, sizeof(bufs[0]), "%X/%X",
			 (uint32) (lsn >> 32), (uint32) lsn);
	return buf;
}

/* ---------------------------------------------------------------------------
 * Segment names.  A name is TLI(8) || LOG(8) || SEG(8) uppercase hex, where
 * a "log" covers 0x100000000 bytes of LSN space and holds
 * 0x100000000 / seg_size segments.
 * ---------------------------------------------------------------------------
 */

bool
wf_segno_from_name(const char *name, TimeLineID *tli, uint64 *hi, uint64 *lo)
{
	uint32		t,
				h,
				l;

	if (strlen(name) != 24 || strspn(name, "0123456789ABCDEF") != 24)
		return false;
	if (sscanf(name, "%08X%08X%08X", &t, &h, &l) != 3)
		return false;
	*tli = t;
	*hi = h;
	*lo = l;
	return true;
}

/* Previous segment name; false when there is none (segment 0/0). */
bool
wf_seg_prev_name(const char *name, uint32 seg_size, char *out)
{
	TimeLineID	tli;
	uint64		hi,
				lo;

	if (!wf_segno_from_name(name, &tli, &hi, &lo))
		return false;
	if (lo == 0)
	{
		if (hi == 0)
			return false;
		hi--;
		lo = UINT64CONST(0x100000000) / seg_size - 1;
	}
	else
		lo--;
	snprintf(out, 25, "%08X%08X%08X", tli, (uint32) hi, (uint32) lo);
	return true;
}

void
wf_seg_next_name(const char *name, uint32 seg_size, char *out)
{
	TimeLineID	tli;
	uint64		hi,
				lo;

	if (!wf_segno_from_name(name, &tli, &hi, &lo))
		wf_format_error("%s is not a WAL segment name", name);
	lo++;
	if (lo >= UINT64CONST(0x100000000) / seg_size)
	{
		hi++;
		lo = 0;
	}
	snprintf(out, 25, "%08X%08X%08X", tli, (uint32) hi, (uint32) lo);
}

static XLogRecPtr
wf_seg_start_lsn(const char *name, uint32 seg_size)
{
	TimeLineID	tli;
	uint64		hi,
				lo;

	if (!wf_segno_from_name(name, &tli, &hi, &lo))
		wf_format_error("%s is not a WAL segment name", name);
	return (hi << 32) + lo * seg_size;
}

/* ---------------------------------------------------------------------------
 * Segment open + page validation
 * ---------------------------------------------------------------------------
 */

static bool
wf_is_power_of_2(uint32 n)
{
	return n > 0 && (n & (n - 1)) == 0;
}

/*
 * Take ownership of buf (a whole segment file) and validate its geometry
 * against the long page header.  Geometry is runtime data, not a build-time
 * constant: the tool works on segments from any compatible build.
 */
void
wf_seg_open(WfSegment *seg, uint8 *buf, size_t len, const char *filename)
{
	XLogLongPageHeaderData longhdr;

	if (len < SizeOfXLogLongPHD)
		wf_format_error("%s: too small to be a WAL segment", filename);
	memcpy(&longhdr, buf, sizeof(longhdr));

	memset(seg, 0, sizeof(*seg));
	strlcpy(seg->name, filename, sizeof(seg->name));
	seg->buf = buf;
	seg->tli = longhdr.std.xlp_tli;
	seg->first_rem_len = longhdr.std.xlp_rem_len;

	if (longhdr.std.xlp_magic != XLOG_PAGE_MAGIC)
		wf_format_error("%s: page magic 0x%X, expected 0x%X (different WAL version?)",
						filename, longhdr.std.xlp_magic, XLOG_PAGE_MAGIC);
	if (!(longhdr.std.xlp_info & XLP_LONG_HEADER))
		wf_format_error("%s: first page lacks the long header", filename);
	if (longhdr.std.xlp_info & ~XLP_ALL_FLAGS)
		wf_format_error("%s: undefined xlp_info flag bits 0x%X on the first page",
						filename, longhdr.std.xlp_info);

	seg->seg_size = longhdr.xlp_seg_size;
	seg->blcksz = longhdr.xlp_xlog_blcksz;
	if (!IsValidWalSegSize(seg->seg_size) ||
		!wf_is_power_of_2(seg->blcksz) ||
		seg->blcksz < 1024 || seg->blcksz > 65536 ||
		seg->seg_size % seg->blcksz != 0)
		wf_format_error("%s: implausible geometry seg=%u blck=%u",
						filename, seg->seg_size, seg->blcksz);
	if (len != seg->seg_size)
		wf_format_error("%s: file is %zu bytes but the long header says segments are %u",
						filename, len, seg->seg_size);

	seg->start_lsn = wf_seg_start_lsn(filename, seg->seg_size);
	if (longhdr.std.xlp_pageaddr != seg->start_lsn)
		wf_format_error("%s: first page address %s does not match the file name (%s)",
						filename, wf_lsn_str(longhdr.std.xlp_pageaddr),
						wf_lsn_str(seg->start_lsn));

	if (longhdr.std.xlp_info & XLP_FIRST_IS_OVERWRITE_CONTRECORD)
	{
		/*
		 * The continuation expected at this segment's start was aborted and
		 * overwritten: the segment begins with fresh records (the first must
		 * be an XLOG_OVERWRITE_CONTRECORD record; the walker verifies that).
		 */
		if ((longhdr.std.xlp_info & XLP_FIRST_IS_CONTRECORD) ||
			seg->first_rem_len != 0)
			wf_format_error("%s: overwrite-contrecord flag combined with a continuation",
							filename);
		seg->first_is_overwrite = true;
	}
	else if (((longhdr.std.xlp_info & XLP_FIRST_IS_CONTRECORD) != 0) !=
			 (seg->first_rem_len > 0))
		wf_format_error("%s: contrecord flag / rem_len mismatch", filename);

	seg->first_content = SizeOfXLogLongPHD;
}

uint32
wf_page_header_size(WfSegment *seg, uint32 page_off)
{
	return page_off == 0 ? SizeOfXLogLongPHD : SizeOfXLogShortPHD;
}

/*
 * Validate the page header at page_off.  WF_PAGE_STALE is a valid end of
 * recorded WAL when a fresh record was expected; contradictions with a
 * record walk in progress are terminal.  WF_PAGE_OVERWRITE is returned to
 * the caller (only meaningful with expect_cont; the walker decides whether
 * an aborted contrecord is acceptable at that point).
 */
WfPageStatus
wf_page_ok(WfSegment *seg, uint32 page_off, bool expect_cont, int64 expect_rem)
{
	XLogPageHeaderData hdr;

	memcpy(&hdr, seg->buf + page_off, sizeof(hdr));

	if (hdr.xlp_magic != XLOG_PAGE_MAGIC ||
		hdr.xlp_pageaddr != seg->start_lsn + page_off)
	{
		if (expect_cont)
			wf_format_error("%s: stale/corrupt page at +0x%X in the middle of a record",
							seg->name, page_off);
		return WF_PAGE_STALE;
	}
	if (hdr.xlp_info & ~XLP_ALL_FLAGS)
		wf_format_error("%s: undefined xlp_info flag bits 0x%X at +0x%X",
						seg->name, hdr.xlp_info, page_off);

	if (hdr.xlp_info & XLP_FIRST_IS_OVERWRITE_CONTRECORD)
	{
		if ((hdr.xlp_info & XLP_FIRST_IS_CONTRECORD) || hdr.xlp_rem_len != 0)
			wf_format_error("%s: overwrite-contrecord flag combined with a continuation at +0x%X",
							seg->name, page_off);
		if (expect_cont)
			return WF_PAGE_OVERWRITE;

		/*
		 * A fresh-record walk restarting exactly at an overwrite page is
		 * what the flag instructs readers to do; nothing to check here (the
		 * xl_prev chain and the overwrite-record type check still apply).
		 */
		return WF_PAGE_OK;
	}

	if (expect_cont)
	{
		if (!(hdr.xlp_info & XLP_FIRST_IS_CONTRECORD))
			wf_format_error("%s: page at +0x%X should continue a record but is not marked as a continuation",
							seg->name, page_off);
		if (expect_rem >= 0 && hdr.xlp_rem_len != expect_rem)
			wf_format_error("%s: page at +0x%X continues a record with rem_len %u, expected " INT64_FORMAT,
							seg->name, page_off, hdr.xlp_rem_len, expect_rem);
	}
	else
	{
		if (hdr.xlp_info & XLP_FIRST_IS_CONTRECORD)
			wf_format_error("%s: expected a fresh record at page +0x%X but it is marked as a record continuation",
							seg->name, page_off);
		if (hdr.xlp_rem_len != 0)
			wf_format_error("%s: page at +0x%X has rem_len %u without the contrecord flag",
							seg->name, page_off, hdr.xlp_rem_len);
	}
	return WF_PAGE_OK;
}

/*
 * Content bytes from content offset off to segment end (O(1); ignores page
 * validity -- an upper bound for reads and spill arithmetic).
 */
uint32
wf_content_available(WfSegment *seg, uint32 off)
{
	uint32		later_pages;

	if (off >= seg->seg_size)
		return 0;
	later_pages = (seg->seg_size - 1) / seg->blcksz - off / seg->blcksz;
	return seg->seg_size - off - later_pages * SizeOfXLogShortPHD;
}

/* ---------------------------------------------------------------------------
 * Run iterator: (file_offset, length) runs covering n content bytes from
 * off, validating every page crossed.
 * ---------------------------------------------------------------------------
 */

void
wf_run_init(WfRunIter *it, WfSegment *seg, uint32 off, uint32 n,
			int64 expect_rem, bool overwrite_ok)
{
	it->seg = seg;
	it->off = off;
	it->n = n;
	it->expect_rem = expect_rem;
	it->overwrite_ok = overwrite_ok;
	it->ovw_page = 0;
}

WfRunStatus
wf_run_next(WfRunIter *it, uint32 *foff, uint32 *len)
{
	WfSegment  *seg = it->seg;
	uint32		page_off;
	uint32		take;

	if (it->n == 0)
		return WF_RUN_DONE;
	if (it->off >= seg->seg_size)
		wf_format_error("%s: content walk past segment end", seg->name);
	page_off = it->off - it->off % seg->blcksz;
	if (it->off == page_off)
	{
		/* crossing into a page: validate it */
		if (wf_page_ok(seg, page_off, true, it->expect_rem) == WF_PAGE_OVERWRITE)
		{
			if (!it->overwrite_ok)
				wf_format_error("%s: unexpected overwrite-contrecord page at +0x%X",
								seg->name, page_off);
			it->ovw_page = page_off;
			return WF_RUN_OVERWRITE;
		}
		it->off += wf_page_header_size(seg, page_off);
	}
	take = Min(it->n, page_off + seg->blcksz - it->off);
	*foff = it->off;
	*len = take;
	it->off += take;
	it->n -= take;
	if (it->expect_rem >= 0)
		it->expect_rem -= take;
	return WF_RUN_YIELD;
}

/* Read n content bytes into out; page contradictions are terminal. */
void
wf_read_content(WfSegment *seg, uint32 off, uint32 n, int64 expect_rem,
				uint8 *out)
{
	WfRunIter	it;
	uint32		foff,
				len;

	wf_run_init(&it, seg, off, n, expect_rem, false);
	while (wf_run_next(&it, &foff, &len) == WF_RUN_YIELD)
	{
		memcpy(out, seg->buf + foff, len);
		out += len;
	}
}

/*
 * Read up to n content bytes WITHOUT page validation (used only to peek at
 * header regions; real walks re-validate).  Returns the number of bytes
 * actually available and copied.
 */
uint32
wf_read_span(WfSegment *seg, uint32 off, uint32 n, uint8 *out)
{
	uint32		total = 0;

	n = Min(n, wf_content_available(seg, off));
	while (n > 0)
	{
		uint32		page_off = off - off % seg->blcksz;
		uint32		take;

		if (off == page_off)
			off += wf_page_header_size(seg, page_off);
		take = Min(n, page_off + seg->blcksz - off);
		memcpy(out + total, seg->buf + off, take);
		off += take;
		total += take;
		n -= take;
	}
	return total;
}

/*
 * Content offset just past n content bytes from off (may sit exactly on a
 * page boundary / segment end).
 */
uint32
wf_skip_content(WfSegment *seg, uint32 off, uint32 n, int64 expect_rem)
{
	WfRunIter	it;
	uint32		foff,
				len;

	wf_run_init(&it, seg, off, n, expect_rem, false);
	while (wf_run_next(&it, &foff, &len) == WF_RUN_YIELD)
		off = foff + len;
	return off;
}

/*
 * Next possible record start at/after content offset off: MAXALIGN, then
 * step over the page header if that lands on a page boundary.  WF_OFF_END
 * at/after segment end.
 */
uint32
wf_align_next_record(WfSegment *seg, uint32 off)
{
	off = MAXALIGN(off);
	if (off >= seg->seg_size)
		return WF_OFF_END;
	if (off % seg->blcksz == 0)
		off += wf_page_header_size(seg, off);
	return off;
}

/* ---------------------------------------------------------------------------
 * Record-header decoding
 * ---------------------------------------------------------------------------
 */

/*
 * Parse the record starting at content offset off.  Returns false when off
 * does not hold a record (zeroed tail, recycled page, segment end).  extra
 * supplies continuation content from FOLLOWING segments for a record whose
 * header region crosses the segment boundary (lookback path).  Structural
 * damage is terminal (fail closed).
 */
bool
wf_parse_record_at(WfSegment *seg, uint32 off, const uint8 *extra,
				   uint32 extralen, WfRecord *rec)
{
	uint8		hdr[WF_HDR_MAX];
	uint32		page_off;
	uint32		avail;
	uint64		cap;
	uint32		want,
				got,
				hdr_limit;
	uint32		p;
	int64		remaining,
				datatotal;
	RelFileNode rnode;
	bool		have_rnode = false;
	int			max_block_id = -1;

	if (off == WF_OFF_END)
		return false;
	page_off = off - off % seg->blcksz;
	if (off == page_off + wf_page_header_size(seg, page_off))
	{
		/* First content byte of a page: a recycled page ends the WAL. */
		if (wf_page_ok(seg, page_off, false, -1) == WF_PAGE_STALE)
			return false;
	}

	avail = wf_content_available(seg, off);
	cap = (uint64) avail + extralen;

	want = Min(WF_HDR_MAX, cap);
	got = wf_read_span(seg, off, Min(want, avail), hdr);
	if (got < want)
	{
		uint32		n = Min(want - got, extralen);

		memcpy(hdr + got, extra, n);
		got += n;
	}
	if (got < 4)
		return false;

	memset(rec, 0, sizeof(*rec));
	rec->off = off;
	rec->lsn = seg->start_lsn + off;
	memcpy(&rec->tot_len, hdr, sizeof(uint32));
	if (rec->tot_len == 0)
		return false;			/* zeroed tail: end of recorded WAL */
	if (rec->tot_len < SizeOfXLogRecord)
		wf_format_error("%s: record at %s: impossible xl_tot_len %u",
						seg->name, wf_lsn_str(rec->lsn), rec->tot_len);
	if (got < SizeOfXLogRecord)
		wf_format_error("%s: record header at %s is cut off",
						seg->name, wf_lsn_str(rec->lsn));
	memcpy(&rec->xid, hdr + 4, sizeof(uint32));
	memcpy(&rec->prev, hdr + 8, sizeof(uint64));
	rec->info = hdr[16];
	rec->rmid = hdr[17];
	memcpy(&rec->crc, hdr + offsetof(XLogRecord, xl_crc), sizeof(pg_crc32c));

	/*
	 * Decode the block-reference/data headers -- DecodeXLogRecord() logic,
	 * bounded by what is actually available: a header region that runs past
	 * the record, the segment prefix, or WF_HDR_MAX fails closed.
	 */
	hdr_limit = Min(got, rec->tot_len);

#define NEED(nb) \
	do { \
		if (p + (nb) > hdr_limit) \
			wf_format_error("%s: record at %s: header region runs past the available bytes", \
							seg->name, wf_lsn_str(rec->lsn)); \
	} while (0)

	p = SizeOfXLogRecord;
	remaining = (int64) rec->tot_len - SizeOfXLogRecord;
	datatotal = 0;

	while (remaining > datatotal)
	{
		uint8		block_id;

		NEED(1);
		block_id = hdr[p];
		p += 1;
		remaining -= 1;

		if (block_id == XLR_BLOCK_ID_DATA_SHORT)
		{
			NEED(1);
			rec->main_data_len = hdr[p];
			p += 1;
			remaining -= 1;
			datatotal += rec->main_data_len;
		}
		else if (block_id == XLR_BLOCK_ID_DATA_LONG)
		{
			NEED(4);
			memcpy(&rec->main_data_len, hdr + p, sizeof(uint32));
			p += 4;
			remaining -= 4;
			datatotal += rec->main_data_len;
		}
		else if (block_id == XLR_BLOCK_ID_ORIGIN)
		{
			NEED(2);
			p += 2;
			remaining -= 2;
		}
		else if (block_id <= XLR_MAX_BLOCK_ID)
		{
			uint8		fork_flags;
			uint16		data_len;
			BlockNumber blkno;

			if ((int) block_id <= max_block_id)
				wf_format_error("%s: record at %s: out-of-order block id %d",
								seg->name, wf_lsn_str(rec->lsn), block_id);
			max_block_id = block_id;

			NEED(3);
			fork_flags = hdr[p];
			memcpy(&data_len, hdr + p + 1, sizeof(uint16));
			p += 3;
			remaining -= 3;
			if (fork_flags & BKPBLOCK_HAS_DATA)
				datatotal += data_len;
			else if (data_len != 0)
				wf_format_error("%s: record at %s: BKPBLOCK_HAS_DATA not set but data_length is %u",
								seg->name, wf_lsn_str(rec->lsn), data_len);
			if (fork_flags & BKPBLOCK_HAS_IMAGE)
			{
				uint16		img_len;
				uint8		bimg_info;

				NEED(5);
				memcpy(&img_len, hdr + p, sizeof(uint16));
				bimg_info = hdr[p + 4];
				p += 5;
				remaining -= 5;
				datatotal += img_len;
				if ((bimg_info & BKPIMAGE_HAS_HOLE) &&
					(bimg_info & BKPIMAGE_IS_COMPRESSED))
				{
					NEED(2);	/* hole_length */
					p += 2;
					remaining -= 2;
				}
			}
			if (!(fork_flags & BKPBLOCK_SAME_REL))
			{
				NEED(12);
				memcpy(&rnode, hdr + p, sizeof(RelFileNode));
				p += 12;
				remaining -= 12;
				have_rnode = true;
			}
			else if (!have_rnode)
				wf_format_error("%s: record at %s: BKPBLOCK_SAME_REL without a previous relfilenode",
								seg->name, wf_lsn_str(rec->lsn));
			NEED(4);
			memcpy(&blkno, hdr + p, sizeof(BlockNumber));
			p += 4;
			remaining -= 4;
			rec->blocks[rec->nblocks].rnode = rnode;
			rec->blocks[rec->nblocks].fork = fork_flags & BKPBLOCK_FORK_MASK;
			rec->blocks[rec->nblocks].blkno = blkno;
			rec->nblocks++;
		}
		else
			wf_format_error("%s: record at %s: invalid block_id %d",
							seg->name, wf_lsn_str(rec->lsn), block_id);
	}
#undef NEED

	if (remaining != datatotal)
		wf_format_error("%s: record at %s: header/data length mismatch (" INT64_FORMAT " vs " INT64_FORMAT ")",
						seg->name, wf_lsn_str(rec->lsn), remaining, datatotal);
	rec->main_data_off = p + (uint32) (remaining - rec->main_data_len);
	return true;
}
