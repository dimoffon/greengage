/*-------------------------------------------------------------------------
 *
 * gp_topology_file.c
 *	  Reading and writing $PGDATA/gp_topology, the file-backed cluster
 *	  topology store.
 *
 * See common/gp_topology_file.h for the format and why it is text.  This file
 * is shared between the backend and frontends -- initdb writes the bootstrap
 * copy, gg_topology exports and repairs, the provider in
 * src/backend/cdb/cdbtopology_file.c reads and writes at runtime -- so it never
 * reports anything itself.  Every entry point returns a typed code and leaves
 * the elevel to whoever knows whether this is the postmaster refusing to start,
 * a backend failing a query, or a command-line tool printing to stderr.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/common/gp_topology_file.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <fcntl.h>
#include <unistd.h>

#include "common/file_perm.h"
#include "common/gp_topology_file.h"
#include "common/string.h"
#include "lib/stringinfo.h"
#include "port/pg_crc32c.h"

#ifndef FRONTEND
#include "storage/fd.h"
#else
#include "common/file_utils.h"
#endif

#define GP_TOPOLOGY_MAGIC		"GPTOPOLOGY"

/*
 * A generous ceiling on the whole file, so a reader cannot be made to allocate
 * arbitrarily by a corrupt or hostile one.  Ten thousand segments at the
 * ~300 bytes a long datadir costs is comfortably under this.
 */
#define GP_TOPOLOGY_MAX_SIZE	(16 * 1024 * 1024)

const char *
gp_topology_file_error_str(GpTopologyFileError err)
{
	switch (err)
	{
		case GP_TOPOFILE_OK:
			return "success";
		case GP_TOPOFILE_ENOENT:
			return "file does not exist";
		case GP_TOPOFILE_IO:
			return "I/O error";
		case GP_TOPOFILE_TRUNCATED:
			return "file is truncated";
		case GP_TOPOFILE_BAD_MAGIC:
			return "not a cluster topology file";
		case GP_TOPOFILE_BAD_VERSION:
			return "unsupported format version";
		case GP_TOPOFILE_BAD_CRC:
			return "checksum mismatch";
		case GP_TOPOFILE_BAD_COUNT:
			return "entry count does not match the entries present";
		case GP_TOPOFILE_BAD_HEADER:
			return "invalid header";
		case GP_TOPOFILE_BAD_SYNTAX:
			return "invalid entry";
		case GP_TOPOFILE_UNSERIALIZABLE:
			return "entry cannot be represented in the file format";
		case GP_TOPOFILE_SYSID_CONFLICT:
			return "file belongs to a different database system";
	}

	return "unknown error";
}

/*
 * Is this byte legal inside a hostname, address or data directory?
 *
 * Deliberately a byte test and not isspace(): the format is a byte string, so
 * a UTF-8 data directory has to round-trip, and ctype is locale-dependent
 * above 0x7F.
 */
static bool
topo_field_byte_ok(char c)
{
	return c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\0';
}

static bool
topo_string_ok(const char *s)
{
	if (s == NULL || *s == '\0')
		return false;

	for (; *s; s++)
	{
		if (!topo_field_byte_ok(*s))
			return false;
	}

	return true;
}

/*
 * Consume one line starting at *p, NUL-terminating it in place.
 *
 * Returns the start of the line and advances *p past its newline, or returns
 * NULL when there is no complete (newline-terminated) line left.  The buffer is
 * the caller's private copy, so writing into it is fine; an unterminated tail
 * is deliberately not a line, which is how truncation is detected.
 */
static char *
topo_next_line(char **p, const char *end)
{
	char	   *start = *p;
	char	   *nl;

	if (start >= end)
		return NULL;

	nl = memchr(start, '\n', end - start);
	if (nl == NULL)
		return NULL;

	*nl = '\0';
	*p = nl + 1;

	return start;
}

/*
 * Split a header line as "<keyword> <value>", returning the value or NULL.
 */
static char *
topo_header_value(char *line, const char *keyword)
{
	size_t		klen = strlen(keyword);

	if (line == NULL || strncmp(line, keyword, klen) != 0 || line[klen] != ' ')
		return NULL;

	return line + klen + 1;
}

static bool
topo_parse_uint64(const char *s, uint64 *out)
{
	char	   *endptr;

	if (s == NULL || *s == '\0')
		return false;

	errno = 0;
#if SIZEOF_LONG >= 8
	*out = strtoul(s, &endptr, 10);
#elif defined(HAVE_STRTOULL)
	*out = strtoull(s, &endptr, 10);
#else
#error "gp_topology requires 64-bit integer support"
#endif

	return errno == 0 && endptr != s && *endptr == '\0';
}

static bool
topo_parse_int(const char *s, int *out, int lo, int hi)
{
	char	   *endptr;
	int			val;

	if (s == NULL || *s == '\0')
		return false;

	errno = 0;
	val = strtoint(s, &endptr, 10);
	if (errno != 0 || endptr == s || *endptr != '\0')
		return false;
	if (val < lo || val > hi)
		return false;

	*out = val;
	return true;
}

/*
 * Split an entry line into exactly GPSEGCONFIGNUMATTR single-space-separated
 * tokens, NUL-terminating each in place.
 *
 * Exactly, not at least: eleven tokens means a data directory contains a space,
 * and the one thing this format must never do is what the old
 * sscanf("%s") parser did with such a line, which was to keep the prefix and
 * carry on.
 */
static bool
topo_split_entry(char *line, char **tok)
{
	int			n;

	for (n = 0; n < GPSEGCONFIGNUMATTR - 1; n++)
	{
		char	   *sp = strchr(line, ' ');

		if (sp == NULL || sp == line)
			return false;		/* too few fields, or an empty one */

		*sp = '\0';
		tok[n] = line;
		line = sp + 1;
	}

	/*
	 * The last field runs to the end of the line, so an eleventh token means a
	 * space inside the data directory.  Splitting here instead would keep the
	 * prefix -- which is precisely what the old sscanf("%s") parser did.
	 */
	if (*line == '\0' || strchr(line, ' ') != NULL)
		return false;

	tok[GPSEGCONFIGNUMATTR - 1] = line;
	return true;
}

static char *
topo_strdup(const char *s)
{
#ifndef FRONTEND
	return pstrdup(s);
#else
	return pg_strdup(s);
#endif
}

static void *
topo_alloc0(size_t size)
{
#ifndef FRONTEND
	return palloc0(size);
#else
	return pg_malloc0(size);
#endif
}

static void
topo_free(void *p)
{
#ifndef FRONTEND
	pfree(p);
#else
	pg_free(p);
#endif
}

void
gp_topology_file_free(GpTopologyFile *topo)
{
	int			i;

	if (topo == NULL)
		return;

	for (i = 0; i < topo->nentries; i++)
	{
		GpSegConfigEntry *e = &topo->entries[i];

		if (e->hostname)
			topo_free(e->hostname);
		if (e->address)
			topo_free(e->address);
		if (e->datadir)
			topo_free(e->datadir);
	}

	if (topo->entries)
		topo_free(topo->entries);

	memset(topo, 0, sizeof(*topo));
}

/*
 * Parse a whole file image.
 *
 * Check order is magic, then CRC, then version.  CRC before version so that a
 * corrupt v1 file says "checksum" and a genuine v2 file says "unsupported
 * version", instead of a corrupt version byte sending the operator hunting for
 * a newer binary.
 */
GpTopologyFileError
gp_topology_parse(const char *buf, size_t len, GpTopologyFile *out, int *errline)
{
	char	   *copy;
	char	   *p;
	char	   *end;
	char	   *line;
	char	   *value;
	char	   *crc_line;		/* the crc32c line, within `copy` */
	pg_crc32c	crc;
	uint32		recorded_crc;
	uint64		u64;
	int			version;
	int			nentries;
	int			lineno = 0;
	int			i;
	GpTopologyFile topo;

	memset(out, 0, sizeof(*out));
	memset(&topo, 0, sizeof(topo));
	if (errline)
		*errline = 0;

	/*
	 * Work on a private, NUL-terminated copy: the line splitter terminates in
	 * place, and the caller must be free to release buf the moment we return.
	 */
	copy = topo_alloc0(len + 1);
	memcpy(copy, buf, len);
	p = copy;
	end = copy + len;

#define PARSE_FAIL(code) \
	do { \
		topo_free(copy); \
		gp_topology_file_free(&topo); \
		return (code); \
	} while (0)

	/* magic */
	line = topo_next_line(&p, end);
	lineno++;
	if (line == NULL)
		PARSE_FAIL(GP_TOPOFILE_TRUNCATED);
	value = topo_header_value(line, GP_TOPOLOGY_MAGIC);
	if (value == NULL)
		PARSE_FAIL(GP_TOPOFILE_BAD_MAGIC);

	/*
	 * Find and verify the checksum before believing anything else.  The
	 * covered region is every byte up to and including the newline that ends
	 * the last entry, which is simply "the file minus its final line".
	 */
	{
		const char *last_nl;
		size_t		covered;

		if (len == 0 || buf[len - 1] != '\n')
			PARSE_FAIL(GP_TOPOFILE_TRUNCATED);

		last_nl = memrchr(buf, '\n', len - 1);
		if (last_nl == NULL)
			PARSE_FAIL(GP_TOPOFILE_TRUNCATED);

		covered = (size_t) (last_nl + 1 - buf);
		crc_line = copy + covered;

		if (strncmp(buf + covered, "crc32c ", 7) != 0)
			PARSE_FAIL(GP_TOPOFILE_TRUNCATED);

		if (sscanf(buf + covered + 7, "%8x", &recorded_crc) != 1)
			PARSE_FAIL(GP_TOPOFILE_BAD_CRC);

		INIT_CRC32C(crc);
		COMP_CRC32C(crc, buf, covered);
		FIN_CRC32C(crc);

		if ((uint32) crc != recorded_crc)
			PARSE_FAIL(GP_TOPOFILE_BAD_CRC);
	}

	/* version, now that the bytes are known good */
	if (!topo_parse_int(value, &version, 1, GP_TOPOLOGY_FORMAT_VERSION))
		PARSE_FAIL(GP_TOPOFILE_BAD_VERSION);
	topo.version = version;

	/* system_identifier */
	line = topo_next_line(&p, end);
	lineno++;
	value = topo_header_value(line, "system_identifier");
	if (value == NULL || !topo_parse_uint64(value, &u64))
		PARSE_FAIL(GP_TOPOFILE_BAD_HEADER);
	topo.system_identifier = u64;

	/* generation */
	line = topo_next_line(&p, end);
	lineno++;
	value = topo_header_value(line, "generation");
	if (value == NULL || !topo_parse_uint64(value, &u64))
		PARSE_FAIL(GP_TOPOFILE_BAD_HEADER);
	topo.generation = u64;

	/* nentries */
	line = topo_next_line(&p, end);
	lineno++;
	value = topo_header_value(line, "nentries");
	if (value == NULL || !topo_parse_int(value, &nentries, 0, PG_INT16_MAX))
		PARSE_FAIL(GP_TOPOFILE_BAD_HEADER);

	/*
	 * generation 0 is initdb's "nobody has ever written this" marker, so it
	 * cannot carry entries.  Only a hand edit or a broken writer produces this.
	 */
	if (u64 == 0 && nentries > 0)
		PARSE_FAIL(GP_TOPOFILE_BAD_HEADER);

	if (nentries > 0)
		topo.entries = topo_alloc0(sizeof(GpSegConfigEntry) * nentries);

	for (i = 0; i < nentries; i++)
	{
		GpSegConfigEntry *e = &topo.entries[i];
		char	   *tok[GPSEGCONFIGNUMATTR];
		int			ival;

		line = topo_next_line(&p, end);
		lineno++;
		if (line == NULL || line == crc_line)
		{
			if (errline)
				*errline = lineno;
			PARSE_FAIL(GP_TOPOFILE_BAD_COUNT);
		}

		topo.nentries = i;		/* so a failure frees only what was filled */

		if (!topo_split_entry(line, tok))
		{
			if (errline)
				*errline = lineno;
			PARSE_FAIL(GP_TOPOFILE_BAD_SYNTAX);
		}

		if (!topo_parse_int(tok[0], &ival, 1, PG_INT16_MAX))
		{
			if (errline)
				*errline = lineno;
			PARSE_FAIL(GP_TOPOFILE_BAD_SYNTAX);
		}
		e->dbid = (int16) ival;

		if (!topo_parse_int(tok[1], &ival, -1, PG_INT16_MAX))
		{
			if (errline)
				*errline = lineno;
			PARSE_FAIL(GP_TOPOFILE_BAD_SYNTAX);
		}
		e->segindex = (int16) ival;

		if (strlen(tok[2]) != 1 || strlen(tok[3]) != 1 ||
			strlen(tok[4]) != 1 || strlen(tok[5]) != 1)
		{
			if (errline)
				*errline = lineno;
			PARSE_FAIL(GP_TOPOFILE_BAD_SYNTAX);
		}
		e->role = tok[2][0];
		e->preferred_role = tok[3][0];
		e->mode = tok[4][0];
		e->status = tok[5][0];

		if (!topo_parse_int(tok[6], &ival, 1, 65535))
		{
			if (errline)
				*errline = lineno;
			PARSE_FAIL(GP_TOPOFILE_BAD_SYNTAX);
		}
		e->port = ival;

		e->hostname = topo_strdup(tok[7]);
		e->address = topo_strdup(tok[8]);
		e->datadir = topo_strdup(tok[9]);

		/* hostip/hostaddrs are read-side scratch; already zeroed */
		topo.nentries = i + 1;
	}

	/*
	 * The next line must be the checksum.  If it is another entry, the header
	 * undercounts -- which the CRC cannot catch, because a tool that recomputes
	 * the checksum after a hand edit leaves nentries alone.
	 */
	line = topo_next_line(&p, end);
	if (line == NULL || line != crc_line)
		PARSE_FAIL(GP_TOPOFILE_BAD_COUNT);

#undef PARSE_FAIL

	topo_free(copy);
	*out = topo;
	return GP_TOPOFILE_OK;
}

char *
gp_topology_serialize(const GpTopologyFile *topo, size_t *len,
					  GpTopologyFileError *err, int *errentry)
{
	StringInfoData buf;
	pg_crc32c	crc;
	int			i;

	if (err)
		*err = GP_TOPOFILE_OK;
	if (errentry)
		*errentry = 0;

	/*
	 * Refuse anything that could not be read back.  This is the one validation
	 * every writer reaches: GpTopoValidate() lives in the backend and
	 * ereports, so initdb and the frontend tools can never call it, and the
	 * tier of it that holds the whitespace rule must not run on the catalog
	 * provider's write path at all.  This check is about the format -- "this
	 * cannot survive a round trip" -- not about the topology.
	 */
	for (i = 0; i < topo->nentries; i++)
	{
		const GpSegConfigEntry *e = &topo->entries[i];

		if (e->dbid < 1 || e->segindex < -1 ||
			e->port < 1 || e->port > 65535 ||
			!topo_field_byte_ok(e->role) ||
			!topo_field_byte_ok(e->preferred_role) ||
			!topo_field_byte_ok(e->mode) ||
			!topo_field_byte_ok(e->status) ||
			!topo_string_ok(e->hostname) ||
			!topo_string_ok(e->address) ||
			!topo_string_ok(e->datadir))
		{
			if (err)
				*err = GP_TOPOFILE_UNSERIALIZABLE;
			if (errentry)
				*errentry = i;
			return NULL;
		}
	}

	initStringInfo(&buf);

	appendStringInfo(&buf, "%s %d\n", GP_TOPOLOGY_MAGIC,
					 GP_TOPOLOGY_FORMAT_VERSION);
	appendStringInfo(&buf, "system_identifier " UINT64_FORMAT "\n",
					 topo->system_identifier);
	appendStringInfo(&buf, "generation " UINT64_FORMAT "\n", topo->generation);
	appendStringInfo(&buf, "nentries %d\n", topo->nentries);

	for (i = 0; i < topo->nentries; i++)
	{
		const GpSegConfigEntry *e = &topo->entries[i];

		appendStringInfo(&buf, "%d %d %c %c %c %c %d %s %s %s\n",
						 e->dbid, e->segindex,
						 e->role, e->preferred_role, e->mode, e->status,
						 e->port, e->hostname, e->address, e->datadir);
	}

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf.data, buf.len);
	FIN_CRC32C(crc);

	appendStringInfo(&buf, "crc32c %08x\n", (uint32) crc);

	if (len)
		*len = buf.len;

	return buf.data;
}

static void
topo_file_path(char *path, size_t pathlen, const char *datadir)
{
	snprintf(path, pathlen, "%s/%s", datadir, GP_TOPOLOGY_FILENAME);
}

GpTopologyFileError
gp_topology_read_file(const char *datadir, GpTopologyFile *out, int *errline)
{
	char		path[MAXPGPATH];
	char	   *buf;
	size_t		len = 0;
	int			fd;
	GpTopologyFileError err;

	memset(out, 0, sizeof(*out));
	topo_file_path(path, sizeof(path), datadir);

	/*
	 * BasicOpenFile, not OpenTransientFile: the provider's startup() reads this
	 * from the postmaster, which never runs InitFileAccess().  ReadControlFile()
	 * is on the same footing for the same reason.
	 */
#ifndef FRONTEND
	fd = BasicOpenFile(path, O_RDONLY | PG_BINARY);
#else
	fd = open(path, O_RDONLY | PG_BINARY, 0);
#endif
	if (fd < 0)
		return errno == ENOENT ? GP_TOPOFILE_ENOENT : GP_TOPOFILE_IO;

	buf = topo_alloc0(GP_TOPOLOGY_MAX_SIZE);

	/* A short read is not EOF.  Loop until read() actually returns zero. */
	for (;;)
	{
		ssize_t		nread;

		if (len >= GP_TOPOLOGY_MAX_SIZE)
		{
			topo_free(buf);
			close(fd);
			return GP_TOPOFILE_IO;
		}

		nread = read(fd, buf + len, GP_TOPOLOGY_MAX_SIZE - len);
		if (nread < 0)
		{
			if (errno == EINTR)
				continue;
			topo_free(buf);
			close(fd);
			return GP_TOPOFILE_IO;
		}
		if (nread == 0)
			break;

		len += (size_t) nread;
	}

	close(fd);

	err = gp_topology_parse(buf, len, out, errline);
	topo_free(buf);

	return err;
}

GpTopologyFileError
gp_topology_write_file(const char *datadir, const GpTopologyFile *topo,
					   bool force, int *errentry)
{
	char		path[MAXPGPATH];
	char		tmppath[MAXPGPATH];
	char	   *buf;
	size_t		len = 0;
	int			fd;
	int			save_errno;
	GpTopologyFileError err = GP_TOPOFILE_OK;

	topo_file_path(path, sizeof(path), datadir);

	/*
	 * A per-pid temp name, not one shared name.  Two writers sharing a name
	 * interleave their write()s; the rename that follows is atomic but the
	 * bytes are not, and the CRC then turns that into a clean refusal to start
	 * -- after it has replaced a live cluster's topology.
	 */
	snprintf(tmppath, sizeof(tmppath), "%s/%s.%d", datadir,
			 GP_TOPOLOGY_TMP_PREFIX, (int) getpid());

	/*
	 * Refuse to write over another database system's topology.  Only checked
	 * one way: a disaster-recovery replica shares production's identifier, so a
	 * match proves nothing -- but a mismatch means this data directory is not
	 * the one whose topology is being written.
	 */
	if (!force)
	{
		GpTopologyFile cur;

		if (gp_topology_read_file(datadir, &cur, NULL) == GP_TOPOFILE_OK)
		{
			bool		conflict = (cur.system_identifier != 0 &&
									topo->system_identifier != 0 &&
									cur.system_identifier != topo->system_identifier);

			gp_topology_file_free(&cur);
			if (conflict)
				return GP_TOPOFILE_SYSID_CONFLICT;
		}
	}

	buf = gp_topology_serialize(topo, &len, &err, errentry);
	if (buf == NULL)
		return err;

#ifndef FRONTEND
	fd = OpenTransientFile(tmppath, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY);
#else
	fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY,
			  pg_file_create_mode);
#endif
	if (fd < 0)
	{
		topo_free(buf);
		return GP_TOPOFILE_IO;
	}

	if (write(fd, buf, len) != (ssize_t) len)
	{
		/* if write didn't set errno, assume it was out of space */
		if (errno == 0)
			errno = ENOSPC;
		goto io_error;
	}

#ifndef FRONTEND
	if (pg_fsync(fd) != 0)
		goto io_error;
	if (CloseTransientFile(fd) != 0)
	{
		fd = -1;
		goto io_error;
	}
#else
	if (fsync(fd) != 0)
		goto io_error;
	if (close(fd) != 0)
	{
		fd = -1;
		goto io_error;
	}
#endif
	fd = -1;
	topo_free(buf);

	/*
	 * durable_rename fsyncs the old file, the new one and the parent
	 * directory.  The backend flavour takes an elevel and is given LOG rather
	 * than ERROR on purpose: this file never longjmps, so both builds return
	 * the same code and the provider raises the error with a message about
	 * topology rather than about rename(2).
	 */
#ifndef FRONTEND
	if (durable_rename(tmppath, path, LOG) != 0)
#else
	if (durable_rename(tmppath, path) != 0)
#endif
	{
		unlink(tmppath);
		return GP_TOPOFILE_IO;
	}

	return GP_TOPOFILE_OK;

io_error:
	save_errno = errno;
	if (fd >= 0)
	{
#ifndef FRONTEND
		CloseTransientFile(fd);
#else
		close(fd);
#endif
	}
	unlink(tmppath);
	topo_free(buf);
	errno = save_errno;
	return GP_TOPOFILE_IO;
}
