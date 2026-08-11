/*-------------------------------------------------------------------------
 *
 * gg_walfilter.c
 *		Record-level filtering of physical WAL on the restore path.
 *
 * Rewrites selected WAL records into same-length XLOG_NOOP records (the
 * walbouncer technique, applied to archived segments instead of a
 * streaming proxy): the record keeps its position, xl_tot_len, xl_prev and
 * xl_xid, so the LSN space, record framing and standby xid tracking are
 * untouched, but its redo becomes a no-op.  The WAL archive itself is
 * never modified -- filtering happens on the fetch path (restore_command)
 * or on an explicit local copy.
 *
 * Subcommands
 *	   restore	 restore_command entry point: fetch a segment from the
 *				 archive (--fetch template or --archive-dir), filter it,
 *				 atomically install it at the target path.  Non-segment
 *				 files (.history, .backup, .partial) are fetched through
 *				 unfiltered.
 *	   filter	 filter local segment file(s) in place (or --output for
 *				 one) -- reusable offline.
 *	   inspect	 parse a segment and report, per record, what would be
 *				 filtered and why.  Read-only.
 *
 * A record is rewritten iff it has at least one block reference and EVERY
 * block reference matches some rule; records with no block references
 * always pass.  --halt-on-remap adds a guard that halts restore (exit 3 + a
 * sticky HALT sentinel) if production rebuilds one of the protected mapped
 * catalogs rather than merely writing to it.
 *
 * Exit status: 0 filtered + installed; 1 usage or WAL-format error (fail
 * closed: a segment we cannot parse is never passed through); 2 fetch
 * failed (the normal "segment not archived yet" polling signal); 3 halt.
 *
 * Portions Copyright (c) 2026, HashData Technology Limited.
 *
 * src/bin/gg_walfilter/gg_walfilter.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>

#include "access/rmgr.h"
#include "access/xlog_internal.h"
#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "getopt_long.h"

#include "walfilter.h"

/* Resource-manager names, by id, for inspect output. */
#define PG_RMGR(symname, name, redo, desc, identify, startup, cleanup, mask) name,
static const char *const rmgr_names[] = {
#include "access/rmgrlist.h"
};

WfErrorMode wf_error_mode = WF_ERRMODE_PLAIN;
const char *wf_halt_dir = NULL;

static const char *progname;
static char wf_cleanup_file[MAXPGPATH];

/* ---------------------------------------------------------------------------
 * Logging / terminal error sinks
 * ---------------------------------------------------------------------------
 */

void
wf_log(const char *fmt,...)
{
	va_list		ap;

	fprintf(stderr, "gg_walfilter: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/*
 * A WAL-format (or I/O) problem: fail closed.  `restore` makes the
 * consequence explicit -- the segment is NOT passed through.
 */
void
wf_format_error(const char *fmt,...)
{
	va_list		ap;

	if (wf_error_mode == WF_ERRMODE_RESTORE)
		fprintf(stderr, "gg_walfilter: error: ");
	else
		fprintf(stderr, "gg_walfilter: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (wf_error_mode == WF_ERRMODE_RESTORE)
		fprintf(stderr, " -- failing closed (NOT passing the segment through)");
	fputc('\n', stderr);
	exit(1);
}

/*
 * A protected mapped catalog changed relfilenode upstream.  `restore`
 * records the sticky HALT sentinel and exits 3 so that every following
 * restore_command invocation halts too; `filter` just reports and exits 1.
 */
void
wf_forbidden_remap(const char *fmt,...)
{
	char		msg[2048];
	va_list		ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (wf_error_mode == WF_ERRMODE_RESTORE)
	{
		wf_log("HALT: %s", msg);
		if (wf_halt_dir != NULL)
		{
			char		dir[MAXPGPATH];
			char		path[MAXPGPATH];
			FILE	   *f;

			strlcpy(dir, wf_halt_dir, sizeof(dir));
			(void) pg_mkdir_p(dir, pg_dir_create_mode);
			snprintf(path, sizeof(path), "%s/%s", wf_halt_dir, WF_HALT_FILE);
			f = fopen(path, "w");
			if (f != NULL)
			{
				fprintf(f, "%s\n", msg);
				fclose(f);
			}
		}
		exit(3);
	}
	fprintf(stderr, "gg_walfilter filter: HALT: %s\n", msg);
	exit(1);
}

static void
wf_cleanup_atexit(void)
{
	if (wf_cleanup_file[0] != '\0')
		(void) unlink(wf_cleanup_file);
}

/* ---------------------------------------------------------------------------
 * CLI plumbing
 * ---------------------------------------------------------------------------
 */

typedef struct WfCliOpts
{
	WfRules		rules;
	const char *datadir;
	const char *state_dir;
	bool		verbose;
	const char *fetch;			/* restore */
	const char *archive_dir;	/* restore */
	const char *output;			/* filter */
	bool		filtered_only;	/* inspect */
} WfCliOpts;

enum
{
	OPT_EXCLUDE_RELFILENODE = 1000,
	OPT_EXCLUDE_DATABASE,
	OPT_EXCLUDE_TABLESPACE,
	OPT_PROTECT_MAPPED_OID,
	OPT_HALT_ON_REMAP,
	OPT_STATE_DIR,
	OPT_FETCH,
	OPT_ARCHIVE_DIR,
	OPT_FILTERED_ONLY,
};

#define WF_COMMON_LONG_OPTIONS \
	{"exclude-relfilenode", required_argument, NULL, OPT_EXCLUDE_RELFILENODE}, \
	{"exclude-database", required_argument, NULL, OPT_EXCLUDE_DATABASE}, \
	{"exclude-tablespace", required_argument, NULL, OPT_EXCLUDE_TABLESPACE}, \
	{"protect-mapped-oid", required_argument, NULL, OPT_PROTECT_MAPPED_OID}, \
	{"halt-on-remap", no_argument, NULL, OPT_HALT_ON_REMAP}, \
	{"datadir", required_argument, NULL, 'D'}, \
	{"state-dir", required_argument, NULL, OPT_STATE_DIR}, \
	{"verbose", no_argument, NULL, 'v'}

static void
usage(void)
{
	printf("%s filters physical WAL: selected records are rewritten into same-length\n"
		   "XLOG_NOOP records; framing, LSNs and xids stay untouched.\n\n"
		   "Usage:\n"
		   "  gg_walfilter restore [OPTIONS] WALFILE DEST     restore_command entry point\n"
		   "  gg_walfilter filter  [OPTIONS] SEGMENT...       filter local files in place\n"
		   "  gg_walfilter inspect [OPTIONS] SEGMENT          dry run: per-record report\n"
		   "\nOptions must precede the positional arguments.\n"
		   "\nFilter rules (a record is rewritten iff it has block references and every\n"
		   "one matches some rule):\n"
		   "  --exclude-relfilenode SPC/DB/REL  literal relfilenode triple\n"
		   "  --exclude-database OID            any relation of that database\n"
		   "  --exclude-tablespace OID          any relation in that tablespace\n"
		   "  --protect-mapped-oid OID          shared mapped catalog, resolved via\n"
		   "                                    <datadir>/global/pg_filenode.map\n"
		   "  --halt-on-remap                   halt (exit 3) if production rebuilds a\n"
		   "                                    protected mapped catalog instead of\n"
		   "                                    writing to it\n"
		   "\nCommon options:\n"
		   "  -D, --datadir DIR                 data directory (default: $PGDATA, else\n"
		   "                                    the current directory -- the server runs\n"
		   "                                    restore_command from the data directory)\n"
		   "  --state-dir DIR                   override <datadir>/gg_walfilter\n"
		   "  -v, --verbose                     log fetches and per-segment results\n"
		   "  -V, --version                     output version information, then exit\n"
		   "  -?, --help                        show this help, then exit\n"
		   "\nrestore options:\n"
		   "  --fetch CMD                       fetch command template with %%f/%%p\n"
		   "                                    placeholders (write them as %%%%f/%%%%p\n"
		   "                                    inside restore_command)\n"
		   "  --archive-dir DIR                 shorthand for --fetch 'cp DIR/%%f %%p'\n"
		   "\nfilter options:\n"
		   "  -o, --output FILE                 write the (single) result here instead\n"
		   "                                    of in place\n"
		   "\ninspect options:\n"
		   "  --filtered-only                   report only records that would be\n"
		   "                                    rewritten\n"
		   "\nExit status: 0 ok; 1 usage or WAL-format error (fail closed); 2 fetch\n"
		   "failed (normal \"not archived yet\" polling signal); 3 halt -- a protected\n"
		   "catalog was rebuilt upstream (re-create + re-seed this node).\n",
		   progname);
}

static Oid
parse_oid_arg(const char *what, const char *arg)
{
	unsigned long v;
	char	   *end;

	errno = 0;
	v = strtoul(arg, &end, 10);
	if (errno != 0 || *end != '\0' || end == arg || v > 0xFFFFFFFFUL)
	{
		fprintf(stderr, "gg_walfilter: bad %s \"%s\"\n", what, arg);
		exit(1);
	}
	return (Oid) v;
}

static void
add_relfilenode_rule(WfRules *rules, const char *spec)
{
	unsigned int spc,
				db,
				rel;
	int			pos = -1;

	if (sscanf(spec, "%u/%u/%u%n", &spc, &db, &rel, &pos) != 3 ||
		pos < 0 || spec[pos] != '\0')
	{
		fprintf(stderr, "gg_walfilter: bad --exclude-relfilenode \"%s\" (want SPC/DB/REL)\n",
				spec);
		exit(1);
	}
	rules->relfilenodes = pg_realloc(rules->relfilenodes,
									 (rules->nrelfilenodes + 1) * sizeof(RelFileNode));
	rules->relfilenodes[rules->nrelfilenodes].spcNode = spc;
	rules->relfilenodes[rules->nrelfilenodes].dbNode = db;
	rules->relfilenodes[rules->nrelfilenodes].relNode = rel;
	rules->nrelfilenodes++;
}

static void
add_oid_rule(Oid **arr, int *n, Oid oid)
{
	*arr = pg_realloc(*arr, (*n + 1) * sizeof(Oid));
	(*arr)[(*n)++] = oid;
}

/* Handle an option code shared by all subcommands; false if not common. */
static bool
handle_common_opt(WfCliOpts *opts, int c)
{
	switch (c)
	{
		case OPT_EXCLUDE_RELFILENODE:
			add_relfilenode_rule(&opts->rules, optarg);
			return true;
		case OPT_EXCLUDE_DATABASE:
			add_oid_rule(&opts->rules.databases, &opts->rules.ndatabases,
						 parse_oid_arg("--exclude-database", optarg));
			return true;
		case OPT_EXCLUDE_TABLESPACE:
			add_oid_rule(&opts->rules.tablespaces, &opts->rules.ntablespaces,
						 parse_oid_arg("--exclude-tablespace", optarg));
			return true;
		case OPT_PROTECT_MAPPED_OID:
			wf_rules_add_mapped_oid(&opts->rules,
									parse_oid_arg("--protect-mapped-oid", optarg));
			return true;
		case OPT_HALT_ON_REMAP:
			opts->rules.remap_guard = true;
			return true;
		case 'D':
			opts->datadir = optarg;
			return true;
		case OPT_STATE_DIR:
			opts->state_dir = optarg;
			return true;
		case 'v':
			opts->verbose = true;
			return true;
		default:
			return false;
	}
}

static const char *
effective_datadir(const WfCliOpts *opts)
{
	const char *env;

	/*
	 * restore_command runs with the data directory as its working directory,
	 * so the default needs no configuration at all.
	 */
	if (opts->datadir != NULL)
		return opts->datadir;
	env = getenv("PGDATA");
	if (env != NULL && env[0] != '\0')
		return env;
	return ".";
}

static void
state_dir_for(const WfCliOpts *opts, char *out)
{
	if (opts->state_dir != NULL)
		strlcpy(out, opts->state_dir, MAXPGPATH);
	else
		snprintf(out, MAXPGPATH, "%s/gg_walfilter", effective_datadir(opts));
}

/*
 * Resolve mapped OIDs through pg_filenode.map (the Python tool's
 * rules_from_args()).
 */
static void
finalize_rules(WfCliOpts *opts)
{
	if (opts->rules.nmapped > 0)
		wf_rules_resolve_mapped(&opts->rules, effective_datadir(opts));
}

static const char *
base_name(const char *path)
{
	const char *s = strrchr(path, '/');

	return s != NULL ? s + 1 : path;
}

/* ---------------------------------------------------------------------------
 * restore
 * ---------------------------------------------------------------------------
 */

typedef struct LookbackCtx
{
	const char *template;
	const char *scratch;
	bool		verbose;
} LookbackCtx;

static uint8 *
lookback_fetch_cb(const char *name, size_t *len, void *arg)
{
	LookbackCtx *ctx = arg;

	return wf_fetch_lookback(ctx->template, ctx->scratch, name, len,
							 ctx->verbose);
}

static int
cmd_restore(int argc, char **argv)
{
	static const struct option long_options[] = {
		WF_COMMON_LONG_OPTIONS,
		{"fetch", required_argument, NULL, OPT_FETCH},
		{"archive-dir", required_argument, NULL, OPT_ARCHIVE_DIR},
		{NULL, 0, NULL, 0}
	};
	WfCliOpts	opts;
	int			c;
	const char *walfile;
	const char *dest;
	char		template_buf[MAXPGPATH + 16];
	const char *template;
	char		state_dir[MAXPGPATH];
	char		halt_path[MAXPGPATH];
	char		tmp[MAXPGPATH];
	char		scratch[MAXPGPATH];
	char		nextname[25];
	uint8	   *buf;
	size_t		len;
	WfSegment	seg;
	WfState		state;
	WfSpill		entry;
	bool		have_entry = false;
	WfFilterResult res;
	FILE	   *haltf;

	memset(&opts, 0, sizeof(opts));
	wf_rules_init(&opts.rules);
	while ((c = getopt_long(argc, argv, "D:v", long_options, NULL)) != -1)
	{
		if (handle_common_opt(&opts, c))
			continue;
		switch (c)
		{
			case OPT_FETCH:
				opts.fetch = optarg;
				break;
			case OPT_ARCHIVE_DIR:
				opts.archive_dir = optarg;
				break;
			default:
				fprintf(stderr, "Try \"%s --help\" for more information.\n",
						progname);
				exit(1);
		}
	}
	if (argc - optind != 2)
	{
		fprintf(stderr, "gg_walfilter restore: expected WALFILE and DEST arguments\n");
		exit(1);
	}
	walfile = argv[optind];
	dest = argv[optind + 1];

	template = opts.fetch;
	if (template == NULL && opts.archive_dir != NULL)
	{
		snprintf(template_buf, sizeof(template_buf), "cp %s/%%f %%p",
				 opts.archive_dir);
		template = template_buf;
	}
	if (template == NULL)
	{
		fprintf(stderr, "gg_walfilter restore: need --fetch or --archive-dir\n");
		exit(1);
	}

	state_dir_for(&opts, state_dir);
	wf_error_mode = WF_ERRMODE_RESTORE;
	wf_halt_dir = state_dir;

	snprintf(halt_path, sizeof(halt_path), "%s/%s", state_dir, WF_HALT_FILE);
	haltf = fopen(halt_path, "r");
	if (haltf != NULL)
	{
		char		msg[2048];
		size_t		n = fread(msg, 1, sizeof(msg) - 1, haltf);

		fclose(haltf);
		msg[n] = '\0';
		while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == ' '))
			msg[--n] = '\0';
		wf_log("HALT (previously recorded): %s", msg);
		exit(3);
	}

	/* Anything that is not a plain WAL segment passes straight through. */
	if (!IsXLogFileName(walfile))
		exit(wf_run_fetch(template, walfile, dest, opts.verbose) ? 0 : 2);

	snprintf(tmp, sizeof(tmp), "%s.ggwf.fetch", dest);
	strlcpy(wf_cleanup_file, tmp, sizeof(wf_cleanup_file));
	if (!wf_run_fetch(template, walfile, tmp, opts.verbose))
		exit(2);				/* not archived yet: the server polls again */

	finalize_rules(&opts);
	wf_state_load(&state, state_dir);
	buf = wf_read_file(tmp, &len);
	if (buf == NULL)
		wf_format_error("could not read fetched segment %s: %s",
						tmp, strerror(errno));
	wf_seg_open(&seg, buf, len, walfile);

	if (seg.first_rem_len > 0)
	{
		WfStateLookup st = wf_state_get(&state, walfile, &entry);

		if (st == WF_STATE_SPILL)
		{
			if (entry.length == seg.first_rem_len)
				have_entry = true;
			else
			{
				wf_log("warning: stale boundary state for %s; re-deriving from the archive",
					   walfile);
				wf_state_delete(&state, walfile);
				st = WF_STATE_MISSING;
			}
		}
		if (!have_entry && st == WF_STATE_MISSING)
		{
			LookbackCtx ctx;

			strlcpy(scratch, tmp, sizeof(scratch));
			if (strrchr(scratch, '/') != NULL)
				*strrchr(scratch, '/') = '\0';
			else
				strlcpy(scratch, ".", sizeof(scratch));
			ctx.template = template;
			ctx.scratch = scratch;
			ctx.verbose = opts.verbose;
			have_entry = wf_resolve_entry_spill(&seg, &opts.rules,
												lookback_fetch_cb, &ctx,
												&entry);
		}
	}

	wf_filter_segment(&seg, &opts.rules, have_entry ? &entry : NULL,
					  NULL, NULL, false, &res);

	/*
	 * Persist the boundary state BEFORE installing the segment: if we crash
	 * in between, the server refetches this file and we refilter it to the
	 * identical result.
	 */
	wf_seg_next_name(walfile, seg.seg_size, nextname);
	wf_state_set(&state, nextname, res.has_exit_spill ? &res.exit_spill : NULL);
	wf_state_save(&state);
	wf_write_and_install(seg.buf, seg.seg_size, dest);

	if (opts.verbose || res.rewritten > 0)
	{
		char	   *at = pg_malloc(res.rewritten * 24 + 8);
		size_t		o = 0;
		uint64		i;

		at[0] = '\0';
		if (res.rewritten > 0)
		{
			o += snprintf(at + o, 8, " at ");
			for (i = 0; i < res.rewritten; i++)
				o += snprintf(at + o, 24, "%s%s", i ? ", " : "",
							  wf_lsn_str(res.rewritten_lsns[i]));
		}
		wf_log("%s: %llu record(s), %llu rewritten to NOOP%s", walfile,
			   (unsigned long long) res.records,
			   (unsigned long long) res.rewritten, at);
		pg_free(at);
	}
	return 0;					/* atexit removes the fetch temp file */
}

/* ---------------------------------------------------------------------------
 * filter
 * ---------------------------------------------------------------------------
 */

static int
path_basename_cmp(const void *a, const void *b)
{
	return strcmp(base_name(*(char *const *) a), base_name(*(char *const *) b));
}

static int
cmd_filter(int argc, char **argv)
{
	static const struct option long_options[] = {
		WF_COMMON_LONG_OPTIONS,
		{"output", required_argument, NULL, 'o'},
		{NULL, 0, NULL, 0}
	};
	WfCliOpts	opts;
	int			c;
	char	  **paths;
	int			npaths;
	int			i;
	WfState		state;
	char		state_dir[MAXPGPATH];
	WfSpill		entry;
	bool		have_entry = false;
	char		prev_name[25] = "";

	memset(&opts, 0, sizeof(opts));
	wf_rules_init(&opts.rules);
	while ((c = getopt_long(argc, argv, "D:vo:", long_options, NULL)) != -1)
	{
		if (handle_common_opt(&opts, c))
			continue;
		switch (c)
		{
			case 'o':
				opts.output = optarg;
				break;
			default:
				fprintf(stderr, "Try \"%s --help\" for more information.\n",
						progname);
				exit(1);
		}
	}
	npaths = argc - optind;
	if (npaths < 1)
	{
		fprintf(stderr, "gg_walfilter filter: no segment files given\n");
		exit(1);
	}
	paths = argv + optind;
	if (opts.output != NULL && npaths != 1)
	{
		fprintf(stderr, "gg_walfilter filter: --output only works with one segment\n");
		exit(1);
	}
	for (i = 0; i < npaths; i++)
	{
		if (!IsXLogFileName(base_name(paths[i])))
		{
			fprintf(stderr, "gg_walfilter filter: %s is not a WAL segment name\n",
					paths[i]);
			exit(1);
		}
	}
	finalize_rules(&opts);
	if (wf_rules_empty(&opts.rules))
	{
		fprintf(stderr, "gg_walfilter filter: no filter rules given\n");
		exit(1);
	}
	qsort(paths, npaths, sizeof(char *), path_basename_cmp);

	/*
	 * Chain boundary state across the given files; persist it for the
	 * follow-on `restore` invocations when a state location is available.
	 */
	if (opts.state_dir != NULL || opts.datadir != NULL)
		state_dir_for(&opts, state_dir);
	else
		state_dir[0] = '\0';
	wf_state_load(&state, state_dir);

	for (i = 0; i < npaths; i++)
	{
		const char *name = base_name(paths[i]);
		uint8	   *buf;
		size_t		len;
		WfSegment	seg;
		WfFilterResult res;
		char		nextname[25];

		buf = wf_read_file(paths[i], &len);
		if (buf == NULL)
			wf_format_error("cannot read %s: %s", paths[i], strerror(errno));
		wf_seg_open(&seg, buf, len, name);

		if (prev_name[0] != '\0')
			wf_seg_next_name(prev_name, seg.seg_size, nextname);
		if (prev_name[0] == '\0' || strcmp(name, nextname) != 0)
		{
			/* not consecutive: try saved state */
			WfSpill		saved;
			WfStateLookup st = wf_state_get(&state, name, &saved);

			have_entry = false;
			if (st != WF_STATE_MISSING && saved.length == seg.first_rem_len &&
				saved.length > 0)
			{
				entry = saved;
				have_entry = true;
			}
		}

		wf_filter_segment(&seg, &opts.rules,
						  (seg.first_rem_len > 0 && have_entry) ? &entry : NULL,
						  NULL, NULL, false, &res);
		wf_write_and_install(seg.buf, seg.seg_size,
							 opts.output != NULL ? opts.output : paths[i]);
		wf_seg_next_name(name, seg.seg_size, nextname);
		wf_state_set(&state, nextname,
					 res.has_exit_spill ? &res.exit_spill : NULL);
		if (res.has_exit_spill)
		{
			entry = res.exit_spill;
			have_entry = true;
		}
		else
			have_entry = false;
		strlcpy(prev_name, name, sizeof(prev_name));
		printf("%s: %llu record(s), %llu rewritten to NOOP\n", name,
			   (unsigned long long) res.records,
			   (unsigned long long) res.rewritten);
		pg_free(seg.buf);
		if (res.rewritten_lsns != NULL)
			pg_free(res.rewritten_lsns);
	}
	wf_state_save(&state);
	return 0;
}

/* ---------------------------------------------------------------------------
 * inspect
 * ---------------------------------------------------------------------------
 */

typedef struct InspectCtx
{
	bool		filtered_only;
} InspectCtx;

static void
inspect_report(WfSegment *seg, WfRecord *rec, bool matched, void *arg)
{
	InspectCtx *ctx = arg;
	char		blocks[8192];
	size_t		o = 0;
	char		rmgrbuf[16];
	const char *rmgr;
	int			i;

	if (ctx->filtered_only && !matched)
		return;
	blocks[0] = '\0';
	for (i = 0; i < rec->nblocks && o < sizeof(blocks) - 1; i++)
		o += snprintf(blocks + o, sizeof(blocks) - o, "%s%u/%u/%u blk %u",
					  i > 0 ? " " : "",
					  rec->blocks[i].rnode.spcNode,
					  rec->blocks[i].rnode.dbNode,
					  rec->blocks[i].rnode.relNode,
					  rec->blocks[i].blkno);
	if (rec->nblocks == 0)
	{
		/*
		 * An smgr truncate names its relation in the payload, not in a block
		 * reference, so without this the most interesting record the DR preset
		 * filters would print with an empty relation column -- and `inspect`
		 * exists to explain *why* a record is filtered.
		 */
		RelFileNode trnode;
		BlockNumber tblkno;

		if (wf_truncate_target(seg, rec, &trnode, &tblkno))
			snprintf(blocks, sizeof(blocks), "%u/%u/%u truncate to %u blk (payload)",
					 trnode.spcNode, trnode.dbNode, trnode.relNode, tblkno);
	}
	if (rec->rmid < lengthof(rmgr_names))
		rmgr = rmgr_names[rec->rmid];
	else
	{
		snprintf(rmgrbuf, sizeof(rmgrbuf), "rmgr%d", rec->rmid);
		rmgr = rmgrbuf;
	}
	printf("%-6s %s %-16s info=0x%02X len=%-6u xid=%-10u %s\n",
		   matched ? "FILTER" : "keep", wf_lsn_str(rec->lsn), rmgr,
		   rec->info, rec->tot_len, rec->xid, blocks);
}

static int
cmd_inspect(int argc, char **argv)
{
	static const struct option long_options[] = {
		WF_COMMON_LONG_OPTIONS,
		{"filtered-only", no_argument, NULL, OPT_FILTERED_ONLY},
		{NULL, 0, NULL, 0}
	};
	WfCliOpts	opts;
	int			c;
	const char *path;
	const char *name;
	uint8	   *buf;
	size_t		len;
	WfSegment	seg;
	WfFilterResult res;
	InspectCtx	ctx;
	char		remnote[64];

	memset(&opts, 0, sizeof(opts));
	wf_rules_init(&opts.rules);
	while ((c = getopt_long(argc, argv, "D:v", long_options, NULL)) != -1)
	{
		if (handle_common_opt(&opts, c))
			continue;
		switch (c)
		{
			case OPT_FILTERED_ONLY:
				opts.filtered_only = true;
				break;
			default:
				fprintf(stderr, "Try \"%s --help\" for more information.\n",
						progname);
				exit(1);
		}
	}
	if (argc - optind != 1)
	{
		fprintf(stderr, "gg_walfilter inspect: expected one SEGMENT argument\n");
		exit(1);
	}
	path = argv[optind];
	name = base_name(path);

	finalize_rules(&opts);
	opts.rules.remap_guard = false; /* never halt during a dry run */

	buf = wf_read_file(path, &len);
	if (buf == NULL)
		wf_format_error("cannot read %s: %s", path, strerror(errno));
	wf_seg_open(&seg, buf, len, name);

	remnote[0] = '\0';
	if (seg.first_rem_len > 0)
		snprintf(remnote, sizeof(remnote), " (starts mid-record, rem_len=%u)",
				 seg.first_rem_len);
	printf("%s: seg_size=%u blcksz=%u tli=%u start=%s%s\n", name,
		   seg.seg_size, seg.blcksz, seg.tli, wf_lsn_str(seg.start_lsn),
		   remnote);

	ctx.filtered_only = opts.filtered_only;
	wf_filter_segment(&seg, &opts.rules, NULL, inspect_report, &ctx, true,
					  &res);
	printf("total: %llu record(s), %llu would be rewritten\n",
		   (unsigned long long) res.records,
		   (unsigned long long) res.rewritten);
	return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

int
main(int argc, char **argv)
{
	pg_logging_init(argv[0]);
	progname = get_progname(argv[0]);
	atexit(wf_cleanup_atexit);

	if (argc < 2)
	{
		fprintf(stderr, "%s: no subcommand given (restore, filter or inspect)\n",
				progname);
		fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
		exit(1);
	}
	if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
	{
		usage();
		exit(0);
	}
	if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
	{
		puts("gg_walfilter (Greengage) " PG_VERSION);
		exit(0);
	}

	if (strcmp(argv[1], "restore") == 0)
		exit(cmd_restore(argc - 1, argv + 1));
	if (strcmp(argv[1], "filter") == 0)
		exit(cmd_filter(argc - 1, argv + 1));
	if (strcmp(argv[1], "inspect") == 0)
		exit(cmd_inspect(argc - 1, argv + 1));

	fprintf(stderr, "%s: unknown subcommand \"%s\"\n", progname, argv[1]);
	fprintf(stderr, "Try \"%s --help\" for more information.\n", progname);
	exit(1);
}
