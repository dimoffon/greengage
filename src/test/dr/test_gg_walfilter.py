#!/usr/bin/env python3
"""Black-box tests for the C gg_walfilter (src/bin/gg_walfilter).

Self-contained apart from the binary under test: builds synthetic WAL
segments (PG12/Greengage 7 on-disk format, including page headers,
contrecord splits and per-record CRCs), runs them through the installed
gg_walfilter, and re-verifies the output with an independent parser (the
"oracle" below, inherited from the original pure-Python implementation of
the tool).

The binary is located via $GG_WALFILTER, then PATH, then $GPHOME/bin, then
the build tree (src/bin/gg_walfilter/).

    GG_WALFILTER=$(command -v gg_walfilter) python3 src/test/dr/test_gg_walfilter.py -v
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def find_binary(name):
    cands = [os.environ.get("GG_WALFILTER" if name == "gg_walfilter" else
                            name.upper())]
    cands.append(shutil.which(name))
    if os.environ.get("GPHOME"):
        cands.append(os.path.join(os.environ["GPHOME"], "bin", name))
    cands.append(os.path.normpath(os.path.join(
        HERE, "..", "..", "bin", "gg_walfilter", name)))
    return next((c for c in cands if c and os.path.exists(c)), None)


GG_WALFILTER = find_binary("gg_walfilter")
if GG_WALFILTER is None:
    sys.exit("test_gg_walfilter: cannot find the gg_walfilter binary "
             "(set GG_WALFILTER, or put it on PATH / in $GPHOME/bin, or "
             "build src/bin/gg_walfilter)")

PG_WALDUMP = find_binary("pg_waldump")


def readf(path, mode="rb"):
    with open(path, mode) as f:
        return f.read()


def run_wf(args, expect=None, **kw):
    proc = subprocess.run([GG_WALFILTER] + args, capture_output=True,
                          text=True, **kw)
    if expect is not None:
        assert proc.returncode == expect, \
            "gg_walfilter %s: rc %d, expected %d\nstdout: %s\nstderr: %s" % (
                " ".join(args), proc.returncode, expect, proc.stdout,
                proc.stderr)
    return proc


# ---------------------------------------------------------------------------
# Verification oracle: an independent WAL parser (the record decoder of the
# original pure-Python gg_walfilter).  It only reads; the filtering itself is
# exercised through the binary.
# ---------------------------------------------------------------------------

XLOG_PAGE_MAGIC = 0xD101
XLP_FIRST_IS_CONTRECORD = 0x0001
XLP_LONG_HEADER = 0x0002
XLP_FIRST_IS_OVERWRITE_CONTRECORD = 0x0008
SIZE_OF_PAGE_HEADER = 24
SIZE_OF_LONG_PAGE_HEADER = 40
SIZE_OF_XLOG_RECORD = 24
XLR_MAX_BLOCK_ID = 32
BKPBLOCK_FORK_MASK = 0x0F
BKPBLOCK_HAS_IMAGE = 0x10
BKPBLOCK_HAS_DATA = 0x20
BKPBLOCK_SAME_REL = 0x80
BKPIMAGE_HAS_HOLE = 0x01
BKPIMAGE_IS_COMPRESSED = 0x02
MAXALIGN = 8


class WalFormatError(Exception):
    pass


def _crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC32C_TABLE = _crc32c_table()


def crc32c(data):
    c = 0xFFFFFFFF
    tab = _CRC32C_TABLE
    for b in data:
        c = tab[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def xlog_record_crc(content):
    """CRC of a fully assembled record: data after the header first, then
    the header up to xl_crc -- mirrors ValidXLogRecord()."""
    c = 0xFFFFFFFF
    tab = _CRC32C_TABLE
    for b in content[SIZE_OF_XLOG_RECORD:]:
        c = tab[(c ^ b) & 0xFF] ^ (c >> 8)
    for b in content[:20]:
        c = tab[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def seg_name_parse(name):
    return int(name[0:8], 16), int(name[8:16], 16), int(name[16:24], 16)


def seg_start_lsn(name, seg_size):
    _tli, hi, lo = seg_name_parse(name)
    return hi * 0x100000000 + lo * seg_size


def lsn_str(lsn):
    return "%X/%X" % (lsn >> 32, lsn & 0xFFFFFFFF)


class Segment(object):
    """Page-aware read access to one in-memory segment (oracle)."""

    def __init__(self, buf, filename):
        self.buf = bytearray(buf)
        self.filename = filename
        magic, info, self.tli, pageaddr, self.first_rem_len = \
            struct.unpack_from("<HHIQI", self.buf, 0)
        if magic != XLOG_PAGE_MAGIC or not info & XLP_LONG_HEADER:
            raise WalFormatError("%s: bad first page" % filename)
        _sysid, self.seg_size, self.blcksz = \
            struct.unpack_from("<QII", self.buf, SIZE_OF_PAGE_HEADER)
        if len(buf) != self.seg_size:
            raise WalFormatError("%s: size/geometry mismatch" % filename)
        self.start_lsn = seg_start_lsn(filename, self.seg_size)
        if pageaddr != self.start_lsn:
            raise WalFormatError("%s: pageaddr mismatch" % filename)
        self.first_content = SIZE_OF_LONG_PAGE_HEADER

    def page_header_size(self, page_off):
        return SIZE_OF_LONG_PAGE_HEADER if page_off == 0 else SIZE_OF_PAGE_HEADER

    def page_ok(self, page_off, expect_cont, expect_rem=None):
        magic, info, _tli, pageaddr, rem = \
            struct.unpack_from("<HHIQI", self.buf, page_off)
        if magic != XLOG_PAGE_MAGIC or pageaddr != self.start_lsn + page_off:
            if expect_cont:
                raise WalFormatError("%s: stale page mid-record at +0x%X"
                                     % (self.filename, page_off))
            return False
        if expect_cont:
            if not info & XLP_FIRST_IS_CONTRECORD:
                raise WalFormatError("%s: continuation missing at +0x%X"
                                     % (self.filename, page_off))
            if expect_rem is not None and rem != expect_rem:
                raise WalFormatError("%s: rem_len mismatch at +0x%X"
                                     % (self.filename, page_off))
        elif info & XLP_FIRST_IS_CONTRECORD:
            raise WalFormatError("%s: unexpected continuation at +0x%X"
                                 % (self.filename, page_off))
        return True

    def content_available(self, off):
        if off >= self.seg_size:
            return 0
        later_pages = (self.seg_size - 1) // self.blcksz - off // self.blcksz
        return self.seg_size - off - later_pages * SIZE_OF_PAGE_HEADER

    def content_runs(self, off, n, expect_rem=None):
        remaining_rec = expect_rem
        while n > 0:
            if off >= self.seg_size:
                raise WalFormatError("%s: walk past segment end"
                                     % self.filename)
            page_off = off - off % self.blcksz
            if off == page_off:
                self.page_ok(page_off, expect_cont=True,
                             expect_rem=remaining_rec)
                off += self.page_header_size(page_off)
            take = min(n, page_off + self.blcksz - off)
            yield off, take
            off += take
            n -= take
            if remaining_rec is not None:
                remaining_rec -= take

    def read_content(self, off, n, expect_rem=None):
        out = bytearray()
        for foff, ln in self.content_runs(off, n, expect_rem):
            out += self.buf[foff:foff + ln]
        return bytes(out)

    def read_span(self, off, n):
        n = min(n, self.content_available(off))
        out = bytearray()
        while n > 0:
            page_off = off - off % self.blcksz
            if off == page_off:
                off += self.page_header_size(page_off)
            take = min(n, page_off + self.blcksz - off)
            out += self.buf[off:off + take]
            off += take
            n -= take
        return bytes(out)

    def skip_content(self, off, n, expect_rem=None):
        for foff, ln in self.content_runs(off, n, expect_rem):
            off = foff + ln
        return off

    def align_next_record(self, off):
        off = (off + MAXALIGN - 1) & ~(MAXALIGN - 1)
        if off >= self.seg_size:
            return None
        if off % self.blcksz == 0:
            off += self.page_header_size(off)
        return off

    def content_lsn(self, off):
        return self.start_lsn + off


class Record(object):
    __slots__ = ("off", "lsn", "tot_len", "xid", "prev", "info", "rmid",
                 "crc", "blocks", "main_data_off", "main_data_len")

    def __init__(self):
        self.blocks = []


def parse_record_at(seg, off):
    """Header-only record decode at content offset off (oracle)."""
    if off is None:
        return None
    page_off = off - off % seg.blcksz
    if off == page_off + seg.page_header_size(page_off):
        if not seg.page_ok(page_off, expect_cont=False):
            return None

    avail = seg.content_available(off)
    hdr = seg.read_span(off, min(1024, avail))
    if len(hdr) < 4:
        return None
    (tot_len,) = struct.unpack_from("<I", hdr, 0)
    if tot_len == 0:
        return None
    rec = Record()
    rec.off = off
    rec.lsn = seg.content_lsn(off)
    rec.tot_len = tot_len
    if tot_len < SIZE_OF_XLOG_RECORD or len(hdr) < SIZE_OF_XLOG_RECORD:
        raise WalFormatError("%s: bad record at %s"
                             % (seg.filename, lsn_str(rec.lsn)))
    rec.xid, rec.prev, rec.info, rec.rmid, _pad, rec.crc = \
        struct.unpack_from("<IQBBHI", hdr, 4)

    limit = min(len(hdr), tot_len)
    p = SIZE_OF_XLOG_RECORD
    remaining = tot_len - SIZE_OF_XLOG_RECORD
    datatotal = 0
    last_rnode = None
    rec.main_data_len = 0

    def need(n):
        if p + n > limit:
            raise WalFormatError("%s: header region truncated at %s"
                                 % (seg.filename, lsn_str(rec.lsn)))

    while remaining > datatotal:
        need(1)
        block_id = hdr[p]
        p += 1
        remaining -= 1
        if block_id == 255:              # DATA_SHORT
            need(1)
            rec.main_data_len = hdr[p]
            p += 1
            remaining -= 1
            datatotal += rec.main_data_len
        elif block_id == 254:            # DATA_LONG
            need(4)
            (rec.main_data_len,) = struct.unpack_from("<I", hdr, p)
            p += 4
            remaining -= 4
            datatotal += rec.main_data_len
        elif block_id == 253:            # ORIGIN
            need(2)
            p += 2
            remaining -= 2
        elif block_id <= XLR_MAX_BLOCK_ID:
            need(3)
            fork_flags = hdr[p]
            (data_len,) = struct.unpack_from("<H", hdr, p + 1)
            p += 3
            remaining -= 3
            if fork_flags & BKPBLOCK_HAS_DATA:
                datatotal += data_len
            if fork_flags & BKPBLOCK_HAS_IMAGE:
                need(5)
                (img_len, _ho) = struct.unpack_from("<HH", hdr, p)
                bimg_info = hdr[p + 4]
                p += 5
                remaining -= 5
                datatotal += img_len
                if (bimg_info & BKPIMAGE_HAS_HOLE and
                        bimg_info & BKPIMAGE_IS_COMPRESSED):
                    need(2)
                    p += 2
                    remaining -= 2
            if not fork_flags & BKPBLOCK_SAME_REL:
                need(12)
                last_rnode = struct.unpack_from("<III", hdr, p)
                p += 12
                remaining -= 12
            need(4)
            (blkno,) = struct.unpack_from("<I", hdr, p)
            p += 4
            remaining -= 4
            rec.blocks.append((last_rnode, fork_flags & BKPBLOCK_FORK_MASK,
                               blkno))
        else:
            raise WalFormatError("%s: invalid block_id %d at %s"
                                 % (seg.filename, block_id, lsn_str(rec.lsn)))
    if remaining != datatotal:
        raise WalFormatError("%s: header/data mismatch at %s"
                             % (seg.filename, lsn_str(rec.lsn)))
    rec.main_data_off = p + (remaining - rec.main_data_len)
    return rec


def reparse_all(segdata, name):
    """Parse every record of a segment with the oracle; returns the Record
    list (raises on any structural damage)."""
    seg = Segment(segdata, name)
    out = []
    off = seg.first_content
    if seg.first_rem_len:
        avail = seg.content_available(off)
        if seg.first_rem_len >= avail:
            return out
        off = seg.align_next_record(
            seg.skip_content(off, seg.first_rem_len,
                             expect_rem=seg.first_rem_len))
    prev = None
    while off is not None:
        rec = parse_record_at(seg, off)
        if rec is None:
            break
        if prev is not None:
            assert rec.prev == prev, "xl_prev chain broken in reparse"
        prev = rec.lsn
        out.append(rec)
        avail = seg.content_available(rec.off)
        if rec.tot_len > avail:
            break
        off = seg.align_next_record(seg.skip_content(rec.off, rec.tot_len))
    return out


# ---------------------------------------------------------------------------
# Synthetic WAL writer: the mirror image of the parser, kept deliberately
# independent of it (page bookkeeping is recomputed from scratch here).
# ---------------------------------------------------------------------------

class WalWriter(object):
    def __init__(self, n_segments=2, seg_size=1024 * 1024, blcksz=8192,
                 tli=1, start_segno=16, sysid=0x0123456789ABCDEF):
        self.seg_size = seg_size
        self.blcksz = blcksz
        self.tli = tli
        self.start_segno = start_segno
        self.sysid = sysid
        self.segs = [bytearray(seg_size) for _ in range(n_segments)]
        self.pos = 0                  # stream offset from segment 0 start
        self.prev_lsn = 0
        self.lsns = []
        self._init_page(0, 0)

    # -- naming ------------------------------------------------------------
    def seg_name(self, i):
        segno = self.start_segno + i
        spi = 0x100000000 // self.seg_size
        return "%08X%08X%08X" % (self.tli, segno // spi, segno % spi)

    def lsn_of(self, stream_pos):
        return self.start_segno * self.seg_size + stream_pos

    # -- page bookkeeping ---------------------------------------------------
    def _hdr_size(self, page_stream_off):
        return 40 if page_stream_off % self.seg_size == 0 else 24

    def _init_page(self, seg_i, page_off, info_extra=0):
        stream = seg_i * self.seg_size + page_off
        info = info_extra
        if page_off == 0:
            info |= 0x0002            # XLP_LONG_HEADER
        struct.pack_into("<HHIQI", self.segs[seg_i], page_off,
                         0xD101, info, self.tli, self.lsn_of(stream), 0)
        if page_off == 0:
            struct.pack_into("<QII", self.segs[seg_i], 24,
                             self.sysid, self.seg_size, self.blcksz)

    def _mark_cont(self, seg_i, page_off, rem_len):
        magic, info, tli, addr, _rem = \
            struct.unpack_from("<HHIQI", self.segs[seg_i], page_off)
        struct.pack_into("<HHIQI", self.segs[seg_i], page_off,
                         magic, info | 0x0001, tli, addr, rem_len)

    # -- record emission ----------------------------------------------------
    def _advance_to_content(self):
        """Align self.pos to the next legal record start."""
        self.pos = (self.pos + 7) & ~7
        in_page = self.pos % self.blcksz
        if in_page == 0:
            seg_i, off = divmod(self.pos, self.seg_size)
            if seg_i < len(self.segs):
                if off % self.blcksz == 0 and off != 0:
                    self._init_page(seg_i, off)
                self.pos += self._hdr_size(self.pos % self.seg_size)

    def _emit(self, data):
        """Write record content bytes at self.pos, splitting across pages and
        segments with continuation headers."""
        remaining = len(data)
        di = 0
        while remaining > 0:
            seg_i, off = divmod(self.pos, self.seg_size)
            if seg_i >= len(self.segs):
                raise RuntimeError("writer ran out of segments")
            in_page = off % self.blcksz
            if in_page == 0:
                self._init_page(seg_i, off)
                self._mark_cont(seg_i, off, remaining)
                self.pos += self._hdr_size(off)
                continue
            room = self.blcksz - in_page
            take = min(room, remaining)
            self.segs[seg_i][off:off + take] = data[di:di + take]
            di += take
            remaining -= take
            self.pos += take

    def add_record(self, rmid=10, info=0x00, xid=1000, blocks=(),
                   main_data=b"", origin=None):
        """blocks: [(spc, db, rel, blkno, blockdata), ...] -- always emitted
        as HAS_DATA references (image-less), like ordinary heap records."""
        body = bytearray()
        datas = []
        for i, (spc, db, rel, blkno, bdata) in enumerate(blocks):
            fork_flags = 0x20         # BKPBLOCK_HAS_DATA, fork 0
            body += struct.pack("<BBH", i, fork_flags, len(bdata))
            body += struct.pack("<III", spc, db, rel)
            body += struct.pack("<I", blkno)
            datas.append(bdata)
        if origin is not None:
            body += struct.pack("<BH", 253, origin)
        if main_data:
            if len(main_data) <= 255:
                body += struct.pack("<BB", 255, len(main_data))
            else:
                body += struct.pack("<BI", 254, len(main_data))
        for d in datas:
            body += d
        body += main_data

        tot_len = 24 + len(body)
        self._advance_to_content()
        lsn = self.lsn_of(self.pos)
        header = struct.pack("<IIQBBHI", tot_len, xid, self.prev_lsn,
                             info, rmid, 0, 0)
        content = bytearray(header + bytes(body))
        struct.pack_into("<I", content, 20, xlog_record_crc(content))
        self._emit(bytes(content))
        self.prev_lsn = lsn
        self.lsns.append(lsn)
        return lsn

    def add_smgr_truncate(self, spc, db, rel, blkno=1, flags=7, xid=0,
                          info=0x21):
        """XLOG_SMGR_TRUNCATE (rmid 2).

        RelationTruncate() registers no buffers, so this record names its
        relation only in the payload -- xl_smgr_truncate {blkno, rnode, flags}.
        That is precisely why the block-tag rule cannot see it.

        info defaults to the byte production actually emits, 0x21 =
        XLOG_SMGR_TRUNCATE | XLR_SPECIAL_REL_UPDATE, so the low bits exercise
        the XLR_RMGR_INFO_MASK the matcher applies.
        """
        main = struct.pack("<IIIIi", blkno, spc, db, rel, flags)
        return self.add_record(rmid=2, info=info, xid=xid, main_data=main)

    def add_relmap_update(self, dbid, tsid, mappings, xid=0,
                          map_slots=126):
        payload = bytearray(16 + map_slots * 8)
        struct.pack_into("<iI", payload, 0, 0x592717, len(mappings))
        for i, (oid, fn) in enumerate(mappings):
            struct.pack_into("<II", payload, 8 + i * 8, oid, fn)
        crc_off = 8 + map_slots * 8
        struct.pack_into("<I", payload, crc_off, crc32c(payload[:crc_off]))
        main = struct.pack("<IIi", dbid, tsid, len(payload)) + bytes(payload)
        return self.add_record(rmid=7, info=0x00, xid=xid, main_data=main)

    def segments(self):
        return [(self.seg_name(i), bytes(b)) for i, b in enumerate(self.segs)]


def make_filenode_map(mappings, slots=126):
    raw = bytearray(16 + slots * 8)
    struct.pack_into("<iI", raw, 0, 0x592717, len(mappings))
    for i, (oid, fn) in enumerate(mappings):
        struct.pack_into("<II", raw, 8 + i * 8, oid, fn)
    crc_off = 8 + slots * 8
    struct.pack_into("<I", raw, crc_off, crc32c(raw[:crc_off]))
    return bytes(raw)


# The rule set the retired --gp-dr-topology preset used to install, spelled out.
#
# The preset is gone (P7): the backend stopped keeping cluster topology in a
# replicated catalog, so there is nothing Greengage-specific left for this tool
# to preset.  What remains is generic -- caller-supplied rules plus the
# --halt-on-remap guard the preset used to turn on implicitly.  These tests keep
# using the same eight catalogs so they go on testing the same behaviour.
DR_TOPOLOGY_OIDS = ["5036", "7139", "7140", "6092", "6093", "5106", "5101", "5103"]


def dr_topology_args():
    args = []
    for oid in DR_TOPOLOGY_OIDS:
        args += ["--protect-mapped-oid", oid]
    return args + ["--halt-on-remap"]


PROTECTED = (1664, 0, 5036)          # a protected relfilenode (global spc)
USER_REL = (1663, 16384, 24576)      # an ordinary user relation

PROTECT_ARGS = ["--exclude-relfilenode", "%d/%d/%d" % PROTECTED]
NOMATCH_ARGS = ["--exclude-relfilenode", "9999/9999/9999"]


class WfTempDir(unittest.TestCase):
    """Base: a temp dir with segments written from a WalWriter."""

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ggwf-test-")
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)

    def write_segments(self, w, subdir=""):
        d = os.path.join(self.dir, subdir) if subdir else self.dir
        os.makedirs(d, exist_ok=True)
        paths = []
        for name, data in w.segments():
            p = os.path.join(d, name)
            with open(p, "wb") as f:
                f.write(data)
            paths.append(p)
        return paths

    def filter_in_place(self, paths, rules_args, expect=0):
        return run_wf(["filter"] + rules_args + paths, expect=expect)


# ---------------------------------------------------------------------------

class TestCrc32c(unittest.TestCase):
    def test_check_value(self):
        # The canonical CRC-32C check value (oracle self-check).
        self.assertEqual(crc32c(b"123456789"), 0xE3069283)

    def test_empty(self):
        self.assertEqual(crc32c(b""), 0)


class TestFilenodeMap(WfTempDir):
    def make_datadir(self, raw):
        datadir = os.path.join(self.dir, "data")
        os.makedirs(os.path.join(datadir, "global"), exist_ok=True)
        with open(os.path.join(datadir, "global", "pg_filenode.map"),
                  "wb") as f:
            f.write(raw)
        return datadir

    def build_seg(self):
        w = WalWriter(n_segments=1)
        self.lsn = w.add_record(blocks=[PROTECTED + (0, b"B" * 40)])
        return self.write_segments(w)[0]

    def test_map_resolution_both_sizes(self):
        for slots in (62, 126):
            p = self.build_seg()
            datadir = self.make_datadir(
                make_filenode_map([(5036, 5036), (1262, 1262)], slots=slots))
            proc = run_wf(["filter", "--protect-mapped-oid", "5036",
                           "-D", datadir, p], expect=0)
            self.assertIn("1 rewritten to NOOP", proc.stdout)

    def test_bad_crc_rejected(self):
        p = self.build_seg()
        raw = bytearray(make_filenode_map([(5036, 5036)]))
        raw[20] ^= 0xFF
        datadir = self.make_datadir(bytes(raw))
        proc = run_wf(["filter", "--protect-mapped-oid", "5036",
                       "-D", datadir, p], expect=1)
        self.assertIn("CRC mismatch", proc.stderr)


class TestBasicFilter(WfTempDir):
    def build(self):
        w = WalWriter(n_segments=1)
        self.keep1 = w.add_record(blocks=[USER_REL + (0, b"A" * 40)])
        self.filt1 = w.add_record(blocks=[PROTECTED + (0, b"B" * 40)])
        self.keep2 = w.add_record(blocks=[USER_REL + (1, b"C" * 300)],
                                  main_data=b"m" * 20)
        # A record touching BOTH a protected and a user relation must be kept.
        self.mixed = w.add_record(blocks=[PROTECTED + (0, b"D" * 10),
                                          USER_REL + (2, b"E" * 10)])
        # No block refs at all (e.g. a commit record): always kept.
        self.plain = w.add_record(rmid=1, info=0x00, main_data=b"commit")
        self.filt2 = w.add_record(blocks=[PROTECTED + (3, b"F" * 500)])
        return w

    def test_selective_noop_rewrite(self):
        w = self.build()
        p = self.write_segments(w)[0]
        name = os.path.basename(p)
        proc = self.filter_in_place([p], PROTECT_ARGS)
        self.assertIn("%s: 6 record(s), 2 rewritten to NOOP" % name,
                      proc.stdout)

        # The filtered result must reparse cleanly end to end.
        data = readf(p)
        recs = reparse_all(data, name)
        self.assertEqual(len(recs), 6)
        seg = Segment(data, name)
        by_lsn = {r.lsn: r for r in recs}
        for lsn in (self.filt1, self.filt2):
            r = by_lsn[lsn]
            self.assertEqual((r.rmid, r.info), (0, 0x20))      # XLOG_NOOP
            self.assertEqual(r.blocks, [])
            content = seg.read_content(r.off, r.tot_len)
            self.assertEqual(xlog_record_crc(content), r.crc)
            # payload is zeros
            self.assertEqual(content[r.main_data_off:].lstrip(b"\0"), b"")
        for lsn in (self.keep1, self.keep2, self.mixed, self.plain):
            r = by_lsn[lsn]
            self.assertNotEqual((r.rmid, r.info & 0xF0), (0, 0x20))
            content = seg.read_content(r.off, r.tot_len)
            self.assertEqual(xlog_record_crc(content), r.crc)

    def test_nonmatching_rules_leave_bytes_untouched(self):
        w = self.build()
        p = self.write_segments(w)[0]
        before = readf(p)
        self.filter_in_place([p], NOMATCH_ARGS)
        self.assertEqual(readf(p), before)

    def test_tot_len_and_framing_preserved(self):
        w = self.build()
        p = self.write_segments(w)[0]
        name = os.path.basename(p)
        before = {r.lsn: (r.tot_len, r.prev, r.xid)
                  for r in reparse_all(readf(p), name)}
        self.filter_in_place([p], PROTECT_ARGS)
        after = {r.lsn: (r.tot_len, r.prev, r.xid)
                 for r in reparse_all(readf(p), name)}
        self.assertEqual(before, after)

    def test_inspect_is_readonly_and_counts(self):
        w = self.build()
        p = self.write_segments(w)[0]
        before = readf(p)
        proc = run_wf(["inspect"] + PROTECT_ARGS + [p], expect=0)
        self.assertEqual(readf(p), before)
        self.assertIn("total: 6 record(s), 2 would be rewritten", proc.stdout)
        self.assertEqual(len(re.findall(r"^FILTER ", proc.stdout,
                                        re.MULTILINE)), 2)
        # --filtered-only prints only the FILTER lines
        proc = run_wf(["inspect", "--filtered-only"] + PROTECT_ARGS + [p],
                      expect=0)
        self.assertNotIn("\nkeep", proc.stdout)

    def test_output_option_leaves_input_untouched(self):
        w = self.build()
        p = self.write_segments(w)[0]
        before = readf(p)
        out = os.path.join(self.dir, "filtered")
        run_wf(["filter", "-o", out] + PROTECT_ARGS + [p], expect=0)
        self.assertEqual(readf(p), before)
        recs = reparse_all(readf(out), os.path.basename(p))
        self.assertEqual(
            sorted(r.lsn for r in recs if (r.rmid, r.info) == (0, 0x20)),
            sorted([self.filt1, self.filt2]))

    def test_database_and_tablespace_rules(self):
        w = self.build()
        name = w.seg_name(0)
        p = self.write_segments(w, "db")[0]
        proc = run_wf(["filter", "--exclude-database", "16384", p], expect=0)
        # keep1, keep2 match their db; mixed touches 1664/0 too -> kept.
        self.assertIn("%s: 6 record(s), 2 rewritten to NOOP" % name,
                      proc.stdout)
        p = self.write_segments(w, "ts")[0]
        proc = run_wf(["filter", "--exclude-tablespace", "1664", p], expect=0)
        self.assertIn("%s: 6 record(s), 2 rewritten to NOOP" % name,
                      proc.stdout)  # filt1, filt2


class TestPageCrossing(WfTempDir):
    def test_record_across_pages(self):
        w = WalWriter(n_segments=1)
        # Big records force page crossings inside the segment.
        lsns = [w.add_record(blocks=[(PROTECTED if i % 2 else USER_REL) +
                                     (i, bytes([i % 251]) * 5000)])
                for i in range(20)]
        p = self.write_segments(w)[0]
        name = os.path.basename(p)
        proc = self.filter_in_place([p], PROTECT_ARGS)
        self.assertIn("%s: 20 record(s), 10 rewritten to NOOP" % name,
                      proc.stdout)
        data = readf(p)
        recs = reparse_all(data, name)
        self.assertEqual([r.lsn for r in recs], lsns)
        seg = Segment(data, name)
        for r in recs:
            content = seg.read_content(r.off, r.tot_len)
            self.assertEqual(xlog_record_crc(content), r.crc)


class TestSegmentBoundary(WfTempDir):
    def build_spill(self, filtered):
        """A record that starts near the end of segment 0 and spills into
        segment 1; filtered or not depending on its relfilenode."""
        w = WalWriter(n_segments=2)
        while w.pos < w.seg_size - 3000:
            w.add_record(blocks=[USER_REL + (7, b"x" * 512)])
        rel = PROTECTED if filtered else USER_REL
        self.spiller = w.add_record(blocks=[rel + (9, b"S" * 6000)])
        assert w.pos > w.seg_size, "spiller did not cross the boundary"
        self.after = w.add_record(blocks=[USER_REL + (1, b"z" * 64)])
        return w

    def test_filtered_spill_chains_state(self):
        w = self.build_spill(filtered=True)
        p0, p1 = self.write_segments(w)
        (n0, _), (n1, _) = w.segments()
        self.filter_in_place([p0, p1], PROTECT_ARGS)

        # Reassemble the spiller across the two filtered segments and check
        # it is now a CRC-valid NOOP.
        s0 = Segment(readf(p0), n0)
        recs0 = reparse_all(readf(p0), n0)
        spiller = [r for r in recs0 if r.lsn == self.spiller][0]
        self.assertEqual((spiller.rmid, spiller.info), (0, 0x20))
        in0 = s0.content_available(spiller.off)
        part0 = s0.read_content(spiller.off, in0)
        s1 = Segment(readf(p1), n1)
        part1 = s1.read_content(s1.first_content, spiller.tot_len - in0,
                                expect_rem=s1.first_rem_len)
        content = part0 + part1
        self.assertEqual(xlog_record_crc(content), spiller.crc)
        # And the record after the continuation still parses in segment 1.
        recs1 = reparse_all(readf(p1), n1)
        self.assertEqual([r.lsn for r in recs1], [self.after])

    def test_unfiltered_spill_passthrough(self):
        w = self.build_spill(filtered=False)
        p0, p1 = self.write_segments(w)
        before1 = readf(p1)
        self.filter_in_place([p0, p1], PROTECT_ARGS)
        self.assertEqual(readf(p1), before1)   # untouched


class TestRelmapGuard(WfTempDir):
    def make_datadir(self):
        datadir = os.path.join(self.dir, "data")
        os.makedirs(os.path.join(datadir, "global"), exist_ok=True)
        with open(os.path.join(datadir, "global", "pg_filenode.map"),
                  "wb") as f:
            f.write(make_filenode_map([(5036, 5036)]))
        return datadir

    def test_benign_update_passes_and_tracks(self):
        w = WalWriter(n_segments=1)
        w.add_relmap_update(0, 1664, [(1262, 99999), (5036, 5036)])
        w.add_record(blocks=[PROTECTED + (0, b"p" * 16)])
        p = self.write_segments(w)[0]
        proc = run_wf(["filter", *dr_topology_args(),
                       "-D", self.make_datadir(), p], expect=0)
        # protected write still caught
        self.assertIn("1 rewritten to NOOP", proc.stdout)

    def test_benign_remap_then_new_filenode_filtered(self):
        # After a benign remap of a protected catalog, writes to the NEW
        # relfilenode are filtered from that point on.
        w = WalWriter(n_segments=1)
        w.add_relmap_update(0, 1664, [(5036, 5036)])
        w.add_record(blocks=[(1664, 0, 4242) + (0, b"n" * 16)])  # not yet
        p = self.write_segments(w)[0]
        proc = run_wf(["filter", "--protect-mapped-oid", "5036",
                       "-D", self.make_datadir(), p], expect=0)
        self.assertIn("0 rewritten to NOOP", proc.stdout)

    def test_forbidden_remap_fails_filter(self):
        w = WalWriter(n_segments=1)
        w.add_relmap_update(0, 1664, [(5036, 424242)])
        p = self.write_segments(w)[0]
        proc = run_wf(["filter", *dr_topology_args(),
                       "-D", self.make_datadir(), p], expect=1)
        self.assertIn("rebuilt upstream", proc.stderr)

    def test_local_db_update_ignored(self):
        w = WalWriter(n_segments=1)
        w.add_relmap_update(16384, 1663, [(5036, 424242)])  # not the shared map
        p = self.write_segments(w)[0]
        proc = run_wf(["filter", *dr_topology_args(),
                       "-D", self.make_datadir(), p], expect=0)
        self.assertIn("0 rewritten to NOOP", proc.stdout)


class TestSmgrTruncate(WfTempDir):
    """XLOG_SMGR_TRUNCATE names its relation in the payload, not in a block
    reference, so the block-tag rule alone would let it through -- and an
    upstream VACUUM truncating a protected catalog would then truncate the
    replica's own copy of it, whose page count legitimately differs.

    The backend closed this in smgr_redo() via DRRedoShouldFilterRelFileNode()
    until P7, when the DR redo filter was deleted -- the cluster topology stopped
    living in a replicated catalog, so the backend has nothing left to protect.
    The tool keeps the rule because its rules are caller-supplied: whoever names a
    relation on the command line means it for truncations too, and a rule that
    silently did not cover XLOG_SMGR_TRUNCATE would be a trap.
    """

    def test_truncate_of_protected_relation_is_filtered(self):
        w = WalWriter(n_segments=1)
        lsn = w.add_smgr_truncate(*PROTECTED, blkno=1)
        p = self.write_segments(w)[0]
        proc = run_wf(["filter"] + PROTECT_ARGS + [p], expect=0)
        self.assertIn("1 rewritten to NOOP", proc.stdout)
        recs = reparse_all(readf(p), os.path.basename(p))
        noops = [r for r in recs if (r.rmid, r.info) == (0, 0x20)]
        self.assertEqual([r.lsn for r in noops], [lsn])

    def test_truncate_matched_without_special_rel_update_bit(self):
        # The matcher keys on XLR_RMGR_INFO_MASK, so a bare 0x20 must match too.
        w = WalWriter(n_segments=1)
        w.add_smgr_truncate(*PROTECTED, blkno=1, info=0x20)
        p = self.write_segments(w)[0]
        proc = run_wf(["filter"] + PROTECT_ARGS + [p], expect=0)
        self.assertIn("1 rewritten to NOOP", proc.stdout)

    def test_truncate_of_ordinary_relation_passes(self):
        w = WalWriter(n_segments=1)
        w.add_smgr_truncate(*USER_REL, blkno=3)
        p = self.write_segments(w)[0]
        before = readf(p)
        proc = run_wf(["filter"] + PROTECT_ARGS + [p], expect=0)
        self.assertIn("0 rewritten to NOOP", proc.stdout)
        self.assertEqual(readf(p), before)

    def test_truncate_matched_via_mapped_oid(self):
        # The DR preset resolves protected OIDs through pg_filenode.map rather
        # than naming relfilenodes, so exercise that path too.
        datadir = os.path.join(self.dir, "data")
        os.makedirs(os.path.join(datadir, "global"), exist_ok=True)
        with open(os.path.join(datadir, "global", "pg_filenode.map"), "wb") as f:
            f.write(make_filenode_map([(5036, 5036)]))
        w = WalWriter(n_segments=1)
        w.add_smgr_truncate(*PROTECTED, blkno=1)
        p = self.write_segments(w)[0]
        proc = run_wf(["filter", *dr_topology_args(), "-D", datadir, p],
                      expect=0)
        self.assertIn("1 rewritten to NOOP", proc.stdout)

    def test_smgr_create_is_not_filtered(self):
        # Fidelity check: smgr_redo guards only its truncate branch, so a
        # CREATE naming the same relation must pass here too.  (The tool never
        # parses CREATE, so the payload shape is irrelevant.)
        w = WalWriter(n_segments=1)
        w.add_record(rmid=2, info=0x10,
                     main_data=struct.pack("<IIIii", *PROTECTED, 0, 0))
        p = self.write_segments(w)[0]
        before = readf(p)
        proc = run_wf(["filter"] + PROTECT_ARGS + [p], expect=0)
        self.assertIn("0 rewritten to NOOP", proc.stdout)
        self.assertEqual(readf(p), before)

    def test_inspect_reports_the_payload_target(self):
        # `inspect` exists to explain *why*; a filtered record with an empty
        # relation column would defeat that.
        w = WalWriter(n_segments=1)
        w.add_smgr_truncate(*PROTECTED, blkno=1)
        p = self.write_segments(w)[0]
        proc = run_wf(["inspect"] + PROTECT_ARGS + [p], expect=0)
        self.assertIn("FILTER", proc.stdout)
        self.assertIn("%d/%d/%d truncate to 1 blk (payload)" % PROTECTED,
                      proc.stdout)

    def test_truncate_payload_too_short_fails_closed(self):
        w = WalWriter(n_segments=1)
        w.add_record(rmid=2, info=0x20, main_data=b"\x00" * 8)   # < 20 bytes
        p = self.write_segments(w)[0]
        proc = run_wf(["filter"] + PROTECT_ARGS + [p], expect=1)
        self.assertIn("payload byte(s)", proc.stderr)


class TestStreamEnds(WfTempDir):
    def test_xlog_switch_stops_the_walk(self):
        w = WalWriter(n_segments=1)
        w.add_record(blocks=[PROTECTED + (0, b"a" * 16)])
        w.add_record(rmid=0, info=0x40)          # XLOG_SWITCH
        # Garbage beyond the switch must not be interpreted.
        w.segs[0][w.pos + 64:w.pos + 96] = b"\xde\xad" * 16
        p = self.write_segments(w)[0]
        proc = self.filter_in_place([p], PROTECT_ARGS)
        self.assertIn("2 record(s), 1 rewritten to NOOP", proc.stdout)

    def test_recycled_page_stops_the_walk(self):
        w = WalWriter(n_segments=1)
        w.add_record(blocks=[USER_REL + (0, b"a" * 16)])
        # Overwrite the second page with a page from an unrelated position.
        bogus = bytearray(24)
        struct.pack_into("<HHIQI", bogus, 0, 0xD101, 0, 1, 0xDEADBEEF00, 0)
        w.segs[0][w.blcksz:w.blcksz + 24] = bogus
        p = self.write_segments(w)[0]
        proc = self.filter_in_place([p], PROTECT_ARGS)
        self.assertIn("1 record(s), 0 rewritten to NOOP", proc.stdout)

    def test_broken_prev_chain_fails_closed(self):
        w = WalWriter(n_segments=1)
        w.add_record(blocks=[USER_REL + (0, b"a" * 16)])
        w.prev_lsn += 8                      # corrupt the chain
        w.add_record(blocks=[USER_REL + (1, b"b" * 16)])
        p = self.write_segments(w)[0]
        proc = self.filter_in_place([p], PROTECT_ARGS, expect=1)
        self.assertIn("xl_prev", proc.stderr)

    def test_crc_mismatch_on_filtered_record_fails_closed(self):
        w = WalWriter(n_segments=1)
        w.add_record(blocks=[PROTECTED + (0, b"a" * 64)])
        rec = reparse_all(bytes(w.segs[0]), w.seg_name(0))[0]
        # Flip one payload byte WITHOUT fixing the CRC.
        w.segs[0][rec.off + rec.tot_len - 1] ^= 0xFF
        p = self.write_segments(w)[0]
        proc = self.filter_in_place([p], PROTECT_ARGS, expect=1)
        self.assertIn("CRC", proc.stderr)

    def test_undefined_page_flag_fails_closed(self):
        w = WalWriter(n_segments=1)
        w.add_record(blocks=[USER_REL + (0, b"a" * 16)])
        # Set an undefined xlp_info bit on the first page.
        magic, info = struct.unpack_from("<HH", w.segs[0], 0)
        struct.pack_into("<HH", w.segs[0], 0, magic, info | 0x0010)
        p = self.write_segments(w)[0]
        proc = self.filter_in_place([p], PROTECT_ARGS, expect=1)
        self.assertIn("xlp_info", proc.stderr)


class TestOverwriteContrecord(WfTempDir):
    """XLP_FIRST_IS_OVERWRITE_CONTRECORD: an aborted contrecord that was
    overwritten by the new primary after a crash."""

    def build_entry(self):
        """Segment 0 ends with a protected record whose continuation into
        segment 1 was never written; segment 1 instead starts fresh with an
        XLOG_OVERWRITE_CONTRECORD record."""
        w = WalWriter(n_segments=2)
        while w.pos < w.seg_size - 3000:
            w.add_record(blocks=[USER_REL + (7, b"x" * 512)])
        self.aborted = w.add_record(blocks=[PROTECTED + (9, b"S" * 6000)])
        assert w.pos > w.seg_size
        # Rewrite segment 1's first page as the overwrite page: no
        # continuation, rem_len 0, OVERWRITE flag; then place the marker
        # record and a follow-on record there.
        w.segs[1] = bytearray(w.seg_size)
        w.pos = w.seg_size          # start of segment 1
        w._init_page(1, 0, info_extra=0x0008)   # XLP_FIRST_IS_OVERWRITE_CONTRECORD
        w.pos += 40
        self.marker = w.add_record(rmid=0, info=0xE0,       # OVERWRITE_CONTRECORD
                                   main_data=struct.pack("<Q", self.aborted))
        self.after = w.add_record(blocks=[USER_REL + (1, b"z" * 64)])
        return w

    def test_entry_overwrite_starts_fresh(self):
        w = self.build_entry()
        p0, p1 = self.write_segments(w)
        # Segment 0 is filtered in ignorance of the later overwrite: the
        # spilling protected record's head is rewritten (its bytes become
        # dead space either way).
        proc = self.filter_in_place([p0], PROTECT_ARGS)
        self.assertIn("1 rewritten to NOOP", proc.stdout)
        # Segment 1 starts fresh at the overwrite page: both records are
        # parsed, nothing rewritten, bytes untouched.
        before1 = readf(p1)
        proc = self.filter_in_place([p1], PROTECT_ARGS)
        self.assertIn("2 record(s), 0 rewritten to NOOP", proc.stdout)
        self.assertEqual(readf(p1), before1)

    def build_midseg(self):
        """The aborted contrecord and its overwrite page live inside ONE
        segment: a protected record crosses a page boundary and the next
        page was rewritten by the new primary."""
        w = WalWriter(n_segments=1)
        while (w.pos % w.blcksz) < w.blcksz - 200:
            w.add_record(blocks=[USER_REL + (7, b"y" * 256)])
        self.aborted = w.add_record(blocks=[PROTECTED + (9, b"S" * 3000)])
        start = self.aborted - w.lsn_of(0)
        next_page = (start // w.blcksz + 1) * w.blcksz
        assert start + 24 < next_page and w.pos > next_page, \
            "aborted record does not cross a page boundary as intended"
        w.segs[0][next_page:] = b"\0" * (w.seg_size - next_page)
        w.pos = next_page
        w._init_page(0, next_page, info_extra=0x0008)
        w.pos += 24
        self.marker = w.add_record(rmid=0, info=0xE0,
                                   main_data=struct.pack("<Q", self.aborted))
        self.after = w.add_record(blocks=[USER_REL + (1, b"z" * 64)])
        return w

    def test_midseg_overwrite_skips_aborted_record(self):
        w = self.build_midseg()
        p = self.write_segments(w)[0]
        before = readf(p)
        proc = self.filter_in_place([p], PROTECT_ARGS)
        # The aborted protected record is dead space: left untouched, not
        # counted, not rewritten; the walk restarts at the overwrite page.
        self.assertIn("0 rewritten to NOOP", proc.stdout)
        self.assertEqual(readf(p), before)

    def test_marker_record_type_is_enforced(self):
        w = self.build_entry()
        # Corrupt the marker: make it an ordinary record instead.
        w.segs[1] = bytearray(w.seg_size)
        w.pos = w.seg_size
        w._init_page(1, 0, info_extra=0x0008)
        w.pos += 40
        w.add_record(blocks=[USER_REL + (1, b"z" * 64)])   # not the marker
        p0, p1 = self.write_segments(w)
        proc = self.filter_in_place([p1], PROTECT_ARGS, expect=1)
        self.assertIn("overwrite-contrecord", proc.stderr)


class TestRestoreCli(WfTempDir):
    """End-to-end through the real CLI: archive dir + state chaining."""

    def setUp(self):
        super().setUp()
        self.archive = os.path.join(self.dir, "archive")
        self.datadir = os.path.join(self.dir, "data")
        self.walout = os.path.join(self.dir, "pg_wal")
        os.makedirs(self.archive)
        os.makedirs(os.path.join(self.datadir, "global"))
        os.makedirs(self.walout)
        with open(os.path.join(self.datadir, "global", "pg_filenode.map"),
                  "wb") as f:
            f.write(make_filenode_map([(5036, 5036), (5101, 5101),
                                       (5103, 5103), (5106, 5106),
                                       (6092, 6092), (6093, 6093),
                                       (7139, 7139), (7140, 7140)]))

    def restore(self, name, expect=0):
        dest = os.path.join(self.walout, "RECOVERYXLOG")
        proc = run_wf(["restore", "--archive-dir", self.archive,
                       *dr_topology_args(), "-D", self.datadir, name, dest],
                      expect=expect)
        return dest, proc

    def test_restore_filters_and_chains(self):
        w = WalWriter(n_segments=2)
        while w.pos < w.seg_size - 3000:
            w.add_record(blocks=[USER_REL + (7, b"x" * 512)])
        spiller = w.add_record(blocks=[PROTECTED + (9, b"S" * 6000)])
        w.add_record(blocks=[USER_REL + (1, b"z" * 64)])
        for name, data in w.segments():
            with open(os.path.join(self.archive, name), "wb") as f:
                f.write(data)

        (n0, _), (n1, _) = w.segments()
        dest, _ = self.restore(n0)
        recs = reparse_all(readf(dest), n0)
        sp = [r for r in recs if r.lsn == spiller][0]
        self.assertEqual((sp.rmid, sp.info), (0, 0x20))

        # State must have been written for the follow-on segment.
        state = os.path.join(self.datadir, "gg_walfilter", "boundaries")
        self.assertTrue(os.path.exists(state))

        dest, _ = self.restore(n1)
        d1 = readf(dest)
        seg1 = Segment(d1, n1)
        # Continuation was zeroed: reassembled record passes its (new) CRC.
        self.assertEqual(
            seg1.read_content(seg1.first_content, seg1.first_rem_len,
                              expect_rem=seg1.first_rem_len).lstrip(b"\0"),
            b"")

        # A restartpoint-style refetch of n1 with the state wiped must give
        # the identical result via archive lookback.
        shutil.rmtree(os.path.join(self.datadir, "gg_walfilter"))
        dest, _ = self.restore(n1)
        self.assertEqual(readf(dest), d1)

    def test_lookback_archive_gap_passes_through(self):
        w = WalWriter(n_segments=2)
        while w.pos < w.seg_size - 3000:
            w.add_record(blocks=[USER_REL + (7, b"x" * 512)])
        w.add_record(blocks=[PROTECTED + (9, b"S" * 6000)])
        w.add_record(blocks=[USER_REL + (1, b"z" * 64)])
        (n0, _), (n1, d1) = w.segments()
        # Only segment 1 is archived: its predecessor is gone.
        with open(os.path.join(self.archive, n1), "wb") as f:
            f.write(d1)
        dest, proc = self.restore(n1)
        self.assertIn("not fetchable", proc.stderr)
        self.assertEqual(readf(dest), d1)   # passed through unmodified

    def test_missing_segment_exits_2(self):
        self.restore("0000000100000000000000FF", expect=2)

    def test_history_file_passthrough(self):
        with open(os.path.join(self.archive, "00000002.history"), "w") as f:
            f.write("1\t0/1000000\tno reason\n")
        dest = os.path.join(self.walout, "RECOVERYHISTORY")
        run_wf(["restore", "--archive-dir", self.archive, *dr_topology_args(),
                "-D", self.datadir, "00000002.history", dest], expect=0)
        self.assertIn("no reason", readf(dest, "r"))

    def test_forbidden_remap_halts_with_exit_3(self):
        w = WalWriter(n_segments=1)
        w.add_relmap_update(0, 1664, [(5036, 424242)])
        name, data = w.segments()[0]
        with open(os.path.join(self.archive, name), "wb") as f:
            f.write(data)
        _, proc = self.restore(name, expect=3)
        self.assertIn("rebuilt upstream", proc.stderr)
        # The halt is sticky.
        _, proc = self.restore(name, expect=3)
        self.assertIn("previously recorded", proc.stderr)


class TestFilterCli(WfTempDir):
    def test_offline_filter_in_place_multi(self):
        w = WalWriter(n_segments=2)
        while w.pos < w.seg_size - 3000:
            w.add_record(blocks=[USER_REL + (7, b"x" * 512)])
        spiller = w.add_record(blocks=[PROTECTED + (9, b"S" * 6000)])
        w.add_record(blocks=[USER_REL + (1, b"z" * 64)])
        paths = self.write_segments(w)
        run_wf(["filter"] + PROTECT_ARGS + paths, expect=0)
        (n0, _), (n1, _) = w.segments()
        recs = reparse_all(readf(paths[0]), n0)
        sp = [r for r in recs if r.lsn == spiller][0]
        self.assertEqual((sp.rmid, sp.info), (0, 0x20))
        seg1 = Segment(readf(paths[1]), n1)
        self.assertEqual(
            seg1.read_content(seg1.first_content, seg1.first_rem_len,
                              expect_rem=seg1.first_rem_len)
            .lstrip(b"\0"), b"")

    def test_no_rules_is_a_usage_error(self):
        w = WalWriter(n_segments=1)
        w.add_record(blocks=[USER_REL + (0, b"a" * 16)])
        p = self.write_segments(w)[0]
        run_wf(["filter", p], expect=1)


@unittest.skipUnless(PG_WALDUMP, "pg_waldump not found")
class TestPgWaldumpOracle(WfTempDir):
    """The installed pg_waldump (xlogreader-based) must decode a filtered
    segment end to end and show XLOG NOOP records at the rewritten LSNs.

    pg_waldump's page size is its build-time XLOG_BLCKSZ (gg_walfilter reads
    geometry from the segment instead), so probe for the matching size.
    """

    def test_waldump_decodes_filtered_segment(self):
        last = None
        for blcksz in (32768, 8192, 16384, 65536, 4096, 2048, 1024):
            w = WalWriter(n_segments=1, blcksz=blcksz)
            filtered = [w.add_record(blocks=[PROTECTED + (i, b"B" * 200)])
                        for i in range(3)]
            w.add_record(blocks=[USER_REL + (0, b"A" * 200)])
            end_lsn = w.lsn_of(w.pos)
            p = self.write_segments(w, "b%d" % blcksz)[0]
            self.filter_in_place([p], PROTECT_ARGS)
            proc = subprocess.run(
                [PG_WALDUMP, "-e", lsn_str(end_lsn), p],
                capture_output=True, text=True)
            last = proc
            if "XLOG_BLCKSZ" in proc.stderr:
                continue                     # wrong build page size: try next
            self.assertEqual(proc.returncode, 0,
                             "pg_waldump: %s" % proc.stderr)
            noops = [l for l in proc.stdout.splitlines()
                     if "NOOP" in l and "rmgr: XLOG" in l]
            self.assertEqual(len(noops), len(filtered), proc.stdout)
            return
        self.skipTest("no tested blcksz matches the pg_waldump build: %s"
                      % (last.stderr if last else ""))


if __name__ == "__main__":
    unittest.main()
