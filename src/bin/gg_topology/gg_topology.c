/*-------------------------------------------------------------------------
 *
 * gg_topology.c
 *	  Read, create and replace $PGDATA/gp_topology.
 *
 * The cluster topology store is a file, so the tool that inspects and repairs
 * it is a frontend program rather than a SQL function.  That is not only
 * convenience: the situations this exists for -- a store nobody has migrated
 * into, a node built from a base backup that carries another cluster's
 * topology, a checksum that does not verify -- are situations where the
 * postmaster refuses to start, so nothing SQL-shaped can reach them.
 *
 * It also keeps one implementation of the format.  gpMgmt cannot write the
 * file itself: Python's zlib.crc32 is CRC-32, and this format uses CRC-32C.
 *
 * Portions Copyright (c) 2026-Present, Greengage contributors.
 *
 * IDENTIFICATION
 *	  src/bin/gg_topology/gg_topology.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <sys/stat.h>
#include <unistd.h>

#include "common/controldata_utils.h"
#include "common/gp_topology_file.h"
#include "common/logging.h"
#include "getopt_long.h"

static const char *progname;

static void
usage(void)
{
	printf(_("%s reads and writes a Greengage cluster topology store.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s dump      -D DATADIR\n"), progname);
	printf(_("  %s bootstrap -D DATADIR [--force]\n"), progname);
	printf(_("  %s write     -D DATADIR [-f FILE] [--force] [--generation N]\n"), progname);
	printf(_("\nCommands:\n"));
	printf(_("  dump       print the store, one entry per line\n"));
	printf(_("  bootstrap  write an empty store, as initdb does\n"));
	printf(_("  write      replace the store from FILE, or from stdin\n"));
	printf(_("\nOptions:\n"));
	printf(_("  -D, --pgdata=DATADIR   data directory holding the store\n"));
	printf(_("  -f, --file=FILE        read entries from FILE (\"-\" means stdin)\n"));
	printf(_("      --force            overwrite a store belonging to another cluster\n"));
	printf(_("      --generation=N     record generation N instead of one past the current\n"));
	printf(_("  -V, --version          output version information, then exit\n"));
	printf(_("  -?, --help             show this help, then exit\n"));
	printf(_("\nAn entry line has the ten fields dump prints, separated by single\n"
			 "spaces:  dbid content role preferred_role mode status port\n"
			 "hostname address datadir\n"));
}

static void
die_topofile(const char *what, const char *datadir, GpTopologyFileError err,
			 int errline)
{
	if (errline > 0)
		pg_log_error("%s \"%s/%s\": %s at line %d", what, datadir,
					 GP_TOPOLOGY_FILENAME, gp_topology_file_error_str(err),
					 errline);
	else
		pg_log_error("%s \"%s/%s\": %s", what, datadir, GP_TOPOLOGY_FILENAME,
					 gp_topology_file_error_str(err));
	exit(1);
}

/*
 * Refuse to write while a postmaster owns the data directory.
 *
 * This, and not the generation compare-and-set, is what keeps two writers
 * apart.  A CAS only excludes writers that can see each other's compare and
 * set; a frontend holds no lock the backend can observe, so both could read
 * generation N and both write N+1.  Every legitimate frontend write -- initdb,
 * building a replica, a restore -- happens with the postmaster down, so this is
 * a precondition rather than a restriction, and --force does not lift it.
 */
static void
require_no_postmaster(const char *datadir)
{
	char		path[MAXPGPATH];
	struct stat st;

	snprintf(path, sizeof(path), "%s/postmaster.pid", datadir);
	if (stat(path, &st) == 0)
	{
		pg_log_error("lock file \"%s\" exists", path);
		pg_log_info("Is a server running in \"%s\"?  Stop it before writing the "
					"cluster topology store.", datadir);
		exit(1);
	}
}

static uint64
read_system_identifier(const char *datadir)
{
	ControlFileData *cf;
	bool		crc_ok;
	uint64		sysid;

	cf = get_controlfile(datadir, &crc_ok);
	if (cf == NULL || !crc_ok)
	{
		pg_log_error("could not read \"%s/global/pg_control\"", datadir);
		exit(1);
	}

	sysid = cf->system_identifier;
	pg_free(cf);

	return sysid;
}

static void
do_dump(const char *datadir)
{
	GpTopologyFile topo;
	GpTopologyFileError err;
	int			errline = 0;
	int			i;

	err = gp_topology_read_file(datadir, &topo, &errline);
	if (err != GP_TOPOFILE_OK)
		die_topofile("could not read", datadir, err, errline);

	printf("version %d\n", topo.version);
	printf("system_identifier " UINT64_FORMAT "\n", topo.system_identifier);
	printf("generation " UINT64_FORMAT "\n", topo.generation);
	printf("nentries %d\n", topo.nentries);

	for (i = 0; i < topo.nentries; i++)
	{
		const GpSegConfigEntry *e = &topo.entries[i];

		printf("%d %d %c %c %c %c %d %s %s %s\n",
			   e->dbid, e->segindex, e->role, e->preferred_role, e->mode,
			   e->status, e->port, e->hostname, e->address, e->datadir);
	}

	gp_topology_file_free(&topo);
}

/*
 * Read entry lines from a stream.
 *
 * Deliberately the same ten-field grammar the store itself uses, so that
 * "psql -A -t -F' ' -c 'SELECT ... FROM gp_segment_configuration' | gg_topology
 * write -" is the migration path, and a data directory containing a space is
 * an eleven-token error here rather than a truncation later.
 */
/*
 * Split one line into exactly GPSEGCONFIGNUMATTR single-space-separated
 * tokens, in place.
 *
 * Not sscanf: "%s" has no bound, so reading a field into a fixed buffer is a
 * stack overflow waiting for a long hostname.  Splitting in place has no
 * buffer to overflow, and it enforces the format's own rule -- the last field
 * runs to end of line, so an eleventh token is a space inside a data
 * directory rather than something to truncate.
 */
static bool
split_entry(char *line, char **tok)
{
	int			n;

	for (n = 0; n < GPSEGCONFIGNUMATTR - 1; n++)
	{
		char	   *sp = strchr(line, ' ');

		if (sp == NULL || sp == line)
			return false;

		*sp = '\0';
		tok[n] = line;
		line = sp + 1;
	}

	if (*line == '\0' || strchr(line, ' ') != NULL)
		return false;

	tok[GPSEGCONFIGNUMATTR - 1] = line;
	return true;
}

static int
parse_field_int(const char *s, const char *what, int lineno, int lo, int hi)
{
	char	   *endptr;
	long		val;

	errno = 0;
	val = strtol(s, &endptr, 10);
	if (errno != 0 || endptr == s || *endptr != '\0' || val < lo || val > hi)
	{
		pg_log_error("line %d: %s \"%s\" is not an integer between %d and %d",
					 lineno, what, s, lo, hi);
		exit(1);
	}

	return (int) val;
}

static char
parse_field_char(const char *s, const char *what, int lineno)
{
	if (strlen(s) != 1)
	{
		pg_log_error("line %d: %s \"%s\" must be a single character",
					 lineno, what, s);
		exit(1);
	}

	return s[0];
}

static void
read_entries(FILE *fp, GpTopologyFile *topo)
{
	char		line[MAXPGPATH * 4];
	int			nalloc = 16;
	int			lineno = 0;

	topo->entries = pg_malloc0(sizeof(GpSegConfigEntry) * nalloc);
	topo->nentries = 0;

	while (fgets(line, sizeof(line), fp) != NULL)
	{
		GpSegConfigEntry *e;
		char	   *tok[GPSEGCONFIGNUMATTR];
		size_t		len = strlen(line);

		lineno++;

		if (len == sizeof(line) - 1 && line[len - 1] != '\n')
		{
			pg_log_error("line %d is too long", lineno);
			exit(1);
		}

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';
		if (len == 0)
			continue;			/* blank lines are ignored, not an error */

		if (!split_entry(line, tok))
		{
			pg_log_error("line %d: expected %d fields separated by single spaces",
						 lineno, GPSEGCONFIGNUMATTR);
			exit(1);
		}

		if (topo->nentries == nalloc)
		{
			nalloc *= 2;
			topo->entries = pg_realloc(topo->entries,
									   sizeof(GpSegConfigEntry) * nalloc);
			memset(&topo->entries[topo->nentries], 0,
				   sizeof(GpSegConfigEntry) * (nalloc - topo->nentries));
		}

		e = &topo->entries[topo->nentries++];
		memset(e, 0, sizeof(*e));
		e->dbid = (int16) parse_field_int(tok[0], "dbid", lineno, 1, PG_INT16_MAX);
		e->segindex = (int16) parse_field_int(tok[1], "content", lineno, -1, PG_INT16_MAX);
		e->role = parse_field_char(tok[2], "role", lineno);
		e->preferred_role = parse_field_char(tok[3], "preferred_role", lineno);
		e->mode = parse_field_char(tok[4], "mode", lineno);
		e->status = parse_field_char(tok[5], "status", lineno);
		e->port = parse_field_int(tok[6], "port", lineno, 1, 65535);
		e->hostname = pg_strdup(tok[7]);
		e->address = pg_strdup(tok[8]);
		e->datadir = pg_strdup(tok[9]);
	}
}

/*
 * The checks that belong to a topology rather than to the file format.
 *
 * Done here, where an operator is standing in front of the tool, rather than
 * left to the provider's startup() to discover at the next start.  The format
 * itself refuses unrepresentable fields -- that check lives in the serializer,
 * where every writer reaches it.
 */
static void
validate_topology(const GpTopologyFile *topo)
{
	int			i,
				j;
	int			ncoordinator = 0;
	int			nstandby = 0;

	for (i = 0; i < topo->nentries; i++)
	{
		const GpSegConfigEntry *a = &topo->entries[i];

		for (j = i + 1; j < topo->nentries; j++)
		{
			const GpSegConfigEntry *b = &topo->entries[j];

			if (a->dbid == b->dbid)
			{
				pg_log_error("duplicate dbid %d", a->dbid);
				exit(1);
			}
			if (a->segindex == b->segindex &&
				a->preferred_role == b->preferred_role)
			{
				pg_log_error("duplicate preferred role '%c' for content %d",
							 a->preferred_role, a->segindex);
				exit(1);
			}
		}

		if (a->segindex == -1)
		{
			if (a->preferred_role == 'p')
				ncoordinator++;
			else
				nstandby++;
		}
	}

	if (topo->nentries > 0 && ncoordinator != 1)
	{
		pg_log_error("topology has %d coordinators, expected exactly one",
					 ncoordinator);
		exit(1);
	}

	if (nstandby > 0)
	{
		pg_log_error("topology has a standby coordinator, which the file store "
					 "cannot support");
		pg_log_info("The file store is not WAL-logged, so a standby's copy of "
					"the topology could not be kept in step.  A node started "
					"with gp_topology_source=file and this topology would "
					"refuse to start.");
		exit(1);
	}
}

static void
do_write(const char *datadir, const char *infile, bool force,
		 const char *generation)
{
	GpTopologyFile topo;
	GpTopologyFile cur;
	GpTopologyFileError err;
	FILE	   *fp;
	int			errentry = 0;

	require_no_postmaster(datadir);

	memset(&topo, 0, sizeof(topo));
	topo.version = GP_TOPOLOGY_FORMAT_VERSION;
	topo.system_identifier = read_system_identifier(datadir);

	if (infile == NULL || strcmp(infile, "-") == 0)
		fp = stdin;
	else if ((fp = fopen(infile, "r")) == NULL)
	{
		pg_log_error("could not open file \"%s\": %m", infile);
		exit(1);
	}

	read_entries(fp, &topo);

	if (fp != stdin)
		fclose(fp);

	validate_topology(&topo);

	/*
	 * The generation must never go backwards for the life of a data directory:
	 * a writer that restarted the count would make a stale generation held by
	 * some backend match again by coincidence.  So continue from what is
	 * already recorded, whether or not we can otherwise read the store.
	 */
	if (generation != NULL)
		topo.generation = strtoul(generation, NULL, 10);
	else if (gp_topology_read_file(datadir, &cur, NULL) == GP_TOPOFILE_OK)
	{
		topo.generation = cur.generation + 1;
		gp_topology_file_free(&cur);
	}
	else
		topo.generation = 1;

	if (topo.generation == 0 && topo.nentries > 0)
	{
		pg_log_error("generation 0 is reserved for a store that has never been "
					 "written");
		exit(1);
	}

	err = gp_topology_write_file(datadir, &topo, force, &errentry);
	if (err == GP_TOPOFILE_SYSID_CONFLICT)
	{
		pg_log_error("\"%s/%s\" belongs to a different database system",
					 datadir, GP_TOPOLOGY_FILENAME);
		pg_log_info("Use --force to replace it.");
		exit(1);
	}
	if (err == GP_TOPOFILE_UNSERIALIZABLE)
	{
		pg_log_error("entry %d cannot be written: a hostname, address or data "
					 "directory contains whitespace, or a field is out of range",
					 errentry + 1);
		exit(1);
	}
	if (err != GP_TOPOFILE_OK)
		die_topofile("could not write", datadir, err, 0);

	printf(_("wrote %d entries at generation " UINT64_FORMAT "\n"),
		   topo.nentries, topo.generation);

	gp_topology_file_free(&topo);
}

static void
do_bootstrap(const char *datadir, bool force)
{
	GpTopologyFile topo;
	GpTopologyFile cur;
	GpTopologyFileError err;

	require_no_postmaster(datadir);

	/*
	 * This is the repair for a store that was deleted, so it must not quietly
	 * undo one that exists.  Resetting a live store to generation 0 would be
	 * worse than losing it: the generation has to be monotone for the life of a
	 * data directory, or a stale generation held by some backend starts
	 * matching again by coincidence and its compare-and-set stops meaning
	 * anything.
	 */
	if (!force &&
		gp_topology_read_file(datadir, &cur, NULL) == GP_TOPOFILE_OK &&
		cur.generation > 0)
	{
		uint64		gen = cur.generation;
		int			n = cur.nentries;

		gp_topology_file_free(&cur);
		pg_log_error("\"%s/%s\" already holds %d entries at generation "
					 UINT64_FORMAT, datadir, GP_TOPOLOGY_FILENAME, n, gen);
		pg_log_info("Use \"gg_topology write\" to replace it, or --force to "
					"reset it to empty.");
		exit(1);
	}

	memset(&topo, 0, sizeof(topo));
	topo.version = GP_TOPOLOGY_FORMAT_VERSION;
	topo.system_identifier = read_system_identifier(datadir);
	topo.generation = 0;
	topo.nentries = 0;

	err = gp_topology_write_file(datadir, &topo, force, NULL);
	if (err == GP_TOPOFILE_SYSID_CONFLICT)
	{
		pg_log_error("\"%s/%s\" belongs to a different database system",
					 datadir, GP_TOPOLOGY_FILENAME);
		pg_log_info("Use --force to replace it.");
		exit(1);
	}
	if (err != GP_TOPOFILE_OK)
		die_topofile("could not write", datadir, err, 0);

	printf(_("wrote an empty cluster topology store\n"));
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"pgdata", required_argument, NULL, 'D'},
		{"file", required_argument, NULL, 'f'},
		{"force", no_argument, NULL, 1},
		{"generation", required_argument, NULL, 2},
		{NULL, 0, NULL, 0}
	};
	const char *datadir = NULL;
	const char *infile = NULL;
	const char *generation = NULL;
	const char *command;
	bool		force = false;
	int			c;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("gg_topology"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("gg_topology (Greengage Database) " PG_VERSION);
			exit(0);
		}
	}

	if (argc < 2)
	{
		pg_log_error("no command specified");
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	command = argv[1];
	optind = 2;

	while ((c = getopt_long(argc, argv, "D:f:", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'D':
				datadir = optarg;
				break;
			case 'f':
				infile = optarg;
				break;
			case 1:
				force = true;
				break;
			case 2:
				generation = optarg;
				break;
			default:
				fprintf(stderr, _("Try \"%s --help\" for more information.\n"),
						progname);
				exit(1);
		}
	}

	if (datadir == NULL)
		datadir = getenv("PGDATA");

	if (datadir == NULL)
	{
		pg_log_error("no data directory specified");
		pg_log_info("Use -D, or set PGDATA.");
		exit(1);
	}

	if (strcmp(command, "dump") == 0)
		do_dump(datadir);
	else if (strcmp(command, "bootstrap") == 0)
		do_bootstrap(datadir, force);
	else if (strcmp(command, "write") == 0)
		do_write(datadir, infile, force, generation);
	else
	{
		pg_log_error("unrecognized command \"%s\"", command);
		fprintf(stderr, _("Try \"%s --help\" for more information.\n"), progname);
		exit(1);
	}

	return 0;
}
