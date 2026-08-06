/*-------------------------------------------------------------------------
 *
 * state.c
 *		gg_walfilter's boundary-continuation state file.
 *
 * When a filtered record spills past segment end its CRC was computed over
 * the full replacement content, so the leading continuation bytes of the
 * NEXT segment(s) must be overwritten with the replacement's tail.  The
 * per-boundary state is tiny (a short non-zero prefix plus the total spill
 * length); it is persisted keyed by the NEXT segment's name, and saved
 * BEFORE the filtered segment is installed so that a crash in between
 * refilters the refetched file to the identical result.
 *
 * Format (native-endian; the file is host-local):
 *		uint32 magic ("GWFS")  uint32 version  uint32 nentries  uint32 crc
 * followed by nentries of
 *		char segname[24]  uint32 rem_len  uint32 prefix_len  uint8 prefix[]
 * where the CRC covers all entry bytes.  A missing or unreadable file is
 * never an error: the archive lookback re-derives any boundary state
 * deterministically (that also makes an upgrade from the old Python tool's
 * state.json self-healing -- its file is simply ignored).
 *
 * Portions Copyright (c) 2026, HashData Technology Limited.
 *
 * src/bin/gg_walfilter/state.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/file_utils.h"

#include "walfilter.h"

#define WF_STATE_MAGIC		0x47574653	/* "GWFS" */
#define WF_STATE_VERSION	1
#define WF_STATE_FILENAME	"boundaries"

static void
state_path(const WfState *state, char *out, const char *suffix)
{
	snprintf(out, MAXPGPATH, "%s/%s%s", state->dir, WF_STATE_FILENAME, suffix);
}

void
wf_state_load(WfState *state, const char *state_dir)
{
	char		path[MAXPGPATH];
	uint8	   *raw;
	size_t		len;
	size_t		pos;
	uint32		magic,
				version,
				nentries,
				file_crc;
	pg_crc32c	crc;
	const char *bad = NULL;
	int			i;

	memset(state, 0, sizeof(*state));
	if (state_dir == NULL || state_dir[0] == '\0')
		return;
	strlcpy(state->dir, state_dir, sizeof(state->dir));

	state_path(state, path, "");
	raw = wf_read_file(path, &len);
	if (raw == NULL)
		return;					/* no state yet: normal */

	if (len < 16)
		bad = "truncated header";
	else
	{
		memcpy(&magic, raw, 4);
		memcpy(&version, raw + 4, 4);
		memcpy(&nentries, raw + 8, 4);
		memcpy(&file_crc, raw + 12, 4);
		if (magic != WF_STATE_MAGIC)
			bad = "bad magic";
		else if (version != WF_STATE_VERSION)
			bad = "unknown version";
		else if (nentries > WF_STATE_KEEP)
			bad = "implausible entry count";
		else
		{
			INIT_CRC32C(crc);
			COMP_CRC32C(crc, raw + 16, len - 16);
			FIN_CRC32C(crc);
			if (crc != file_crc)
				bad = "CRC mismatch";
		}
	}

	pos = 16;
	for (i = 0; bad == NULL && i < (int) nentries; i++)
	{
		WfStateEntry *ent = &state->entries[i];

		if (pos + 24 + 8 > len)
		{
			bad = "truncated entry";
			break;
		}
		memcpy(ent->segname, raw + pos, 24);
		ent->segname[24] = '\0';
		memcpy(&ent->spill.length, raw + pos + 24, 4);
		memcpy(&ent->spill.prefix_len, raw + pos + 28, 4);
		pos += 32;
		if (ent->spill.prefix_len > WF_SPILL_PREFIX_MAX ||
			pos + ent->spill.prefix_len > len)
		{
			bad = "oversized entry";
			break;
		}
		memcpy(ent->spill.prefix, raw + pos, ent->spill.prefix_len);
		pos += ent->spill.prefix_len;
		state->nentries = i + 1;
	}
	if (bad == NULL && pos != len)
		bad = "trailing garbage";

	if (bad != NULL)
	{
		wf_log("warning: unreadable state file %s (%s); relying on archive lookback",
			   path, bad);
		state->nentries = 0;
	}
	pg_free(raw);
}

static WfStateEntry *
state_find(WfState *state, const char *segname)
{
	int			i;

	for (i = 0; i < state->nentries; i++)
		if (strcmp(state->entries[i].segname, segname) == 0)
			return &state->entries[i];
	return NULL;
}

WfStateLookup
wf_state_get(WfState *state, const char *segname, WfSpill *out)
{
	WfStateEntry *ent = state_find(state, segname);

	if (ent == NULL)
		return WF_STATE_MISSING;
	*out = ent->spill;
	return ent->spill.length == 0 ? WF_STATE_CLEAN : WF_STATE_SPILL;
}

/* spill == NULL records a known-clean boundary. */
void
wf_state_set(WfState *state, const char *segname, const WfSpill *spill)
{
	WfStateEntry *ent = state_find(state, segname);

	if (ent == NULL)
	{
		ent = &state->entries[state->nentries++];
		strlcpy(ent->segname, segname, sizeof(ent->segname));
	}
	if (spill != NULL)
		ent->spill = *spill;
	else
		memset(&ent->spill, 0, sizeof(ent->spill));

	/* keep only the newest WF_STATE_KEEP boundaries */
	while (state->nentries > WF_STATE_KEEP)
	{
		int			min_i = 0;
		int			i;

		for (i = 1; i < state->nentries; i++)
			if (strcmp(state->entries[i].segname,
					   state->entries[min_i].segname) < 0)
				min_i = i;
		memmove(&state->entries[min_i], &state->entries[min_i + 1],
				(state->nentries - min_i - 1) * sizeof(WfStateEntry));
		state->nentries--;
	}
}

void
wf_state_delete(WfState *state, const char *segname)
{
	WfStateEntry *ent = state_find(state, segname);

	if (ent == NULL)
		return;
	memmove(ent, ent + 1,
			(state->nentries - (ent - state->entries) - 1) * sizeof(WfStateEntry));
	state->nentries--;
}

void
wf_state_save(WfState *state)
{
	char		path[MAXPGPATH];
	char		tmp[MAXPGPATH];
	char		dir[MAXPGPATH];
	uint8	   *buf;
	size_t		len;
	size_t		pos;
	uint32		v;
	pg_crc32c	crc;
	int			fd;
	int			i;

	if (state->dir[0] == '\0')
		return;

	strlcpy(dir, state->dir, sizeof(dir));
	if (pg_mkdir_p(dir, pg_dir_create_mode) != 0 && errno != EEXIST)
		wf_format_error("could not create state directory %s: %s",
						state->dir, strerror(errno));

	len = 16;
	for (i = 0; i < state->nentries; i++)
		len += 32 + state->entries[i].spill.prefix_len;
	buf = pg_malloc0(len);
	pos = 16;
	for (i = 0; i < state->nentries; i++)
	{
		WfStateEntry *ent = &state->entries[i];

		memcpy(buf + pos, ent->segname, 24);
		memcpy(buf + pos + 24, &ent->spill.length, 4);
		memcpy(buf + pos + 28, &ent->spill.prefix_len, 4);
		memcpy(buf + pos + 32, ent->spill.prefix, ent->spill.prefix_len);
		pos += 32 + ent->spill.prefix_len;
	}
	v = WF_STATE_MAGIC;
	memcpy(buf, &v, 4);
	v = WF_STATE_VERSION;
	memcpy(buf + 4, &v, 4);
	v = state->nentries;
	memcpy(buf + 8, &v, 4);
	INIT_CRC32C(crc);
	COMP_CRC32C(crc, buf + 16, len - 16);
	FIN_CRC32C(crc);
	memcpy(buf + 12, &crc, 4);

	state_path(state, path, "");
	state_path(state, tmp, ".tmp");
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY,
			  pg_file_create_mode);
	if (fd < 0)
		wf_format_error("could not create %s: %s", tmp, strerror(errno));
	errno = 0;
	if (write(fd, buf, len) != (ssize_t) len)
		wf_format_error("could not write %s: %s", tmp,
						errno ? strerror(errno) : "short write");
	if (close(fd) != 0)
		wf_format_error("could not close %s: %s", tmp, strerror(errno));
	if (durable_rename(tmp, path) != 0)
		wf_format_error("could not install state file %s", path);
	pg_free(buf);
}
