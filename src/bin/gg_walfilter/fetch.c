/*-------------------------------------------------------------------------
 *
 * fetch.c
 *		gg_walfilter's archive fetches (restore_command-style templates)
 *		and durable file installation.
 *
 * Portions Copyright (c) 2026, HashData Technology Limited.
 *
 * src/bin/gg_walfilter/fetch.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/file_utils.h"

#include "walfilter.h"

/* Read a whole file; NULL (with errno set) when it cannot be read. */
uint8 *
wf_read_file(const char *path, size_t *len)
{
	int			fd;
	struct stat st;
	uint8	   *buf;
	size_t		done = 0;

	fd = open(path, O_RDONLY | PG_BINARY);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st) != 0)
	{
		close(fd);
		return NULL;
	}
	buf = pg_malloc((size_t) st.st_size + 1);
	while (done < (size_t) st.st_size)
	{
		ssize_t		r = read(fd, buf + done, (size_t) st.st_size - done);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
		{
			pg_free(buf);
			close(fd);
			if (r == 0)
				errno = EIO;
			return NULL;
		}
		done += r;
	}
	close(fd);
	*len = done;
	return buf;
}

/*
 * Expand a fetch template: %f -> filename, %p -> dest, %% -> % (like the
 * server's own restore_command expansion).
 */
char *
wf_expand_fetch(const char *template, const char *filename, const char *dest)
{
	size_t		alloc = strlen(template) + 1;
	char	   *out;
	size_t		o = 0;
	size_t		i;

	for (i = 0; template[i]; i++)
		if (template[i] == '%')
			alloc += strlen(filename) + strlen(dest);
	out = pg_malloc(alloc);

	for (i = 0; template[i];)
	{
		if (template[i] == '%' && template[i + 1] != '\0')
		{
			char		n = template[i + 1];

			if (n == 'f' || n == 'p' || n == '%')
			{
				const char *sub = (n == 'f') ? filename :
					(n == 'p') ? dest : "%";

				strcpy(out + o, sub);
				o += strlen(sub);
				i += 2;
				continue;
			}
		}
		out[o++] = template[i++];
	}
	out[o] = '\0';
	return out;
}

/* Run the fetch template.  True on a zero exit status. */
bool
wf_run_fetch(const char *template, const char *filename, const char *dest,
			 bool verbose)
{
	char	   *cmd = wf_expand_fetch(template, filename, dest);
	int			rc;

	if (verbose)
		wf_log("fetch: %s", cmd);
	fflush(NULL);
	rc = system(cmd);
	pg_free(cmd);
	return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

static void
unlink_quiet(const char *path)
{
	(void) unlink(path);
}

/*
 * Fetch a preceding segment for the lookback path into a scratch temp file,
 * read it, and clean up.  NULL when the archive does not have it (the
 * caller then passes the continuation through).
 */
uint8 *
wf_fetch_lookback(const char *template, const char *scratch_dir,
				  const char *name, size_t *len, bool verbose)
{
	char		tmp[MAXPGPATH];
	uint8	   *buf;

	snprintf(tmp, sizeof(tmp), "%s/%s.ggwf.lookback", scratch_dir, name);
	if (!wf_run_fetch(template, name, tmp, verbose))
	{
		unlink_quiet(tmp);
		return NULL;
	}
	buf = wf_read_file(tmp, len);
	unlink_quiet(tmp);
	if (buf == NULL)
		wf_format_error("could not read fetched segment %s: %s",
						tmp, strerror(errno));
	return buf;
}

/*
 * Write data to dest atomically and durably: temp file in the same
 * directory, then durable_rename() (which fsyncs the file and the parent
 * directory).
 */
void
wf_write_and_install(const uint8 *data, size_t len, const char *dest)
{
	char		tmp[MAXPGPATH];
	int			fd;
	size_t		done = 0;

	snprintf(tmp, sizeof(tmp), "%s.ggwf", dest);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY,
			  pg_file_create_mode);
	if (fd < 0)
		wf_format_error("could not create %s: %s", tmp, strerror(errno));
	while (done < len)
	{
		ssize_t		w = write(fd, data + done, len - done);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			wf_format_error("could not write %s: %s", tmp,
							errno ? strerror(errno) : "short write");
		done += w;
	}
	if (close(fd) != 0)
		wf_format_error("could not close %s: %s", tmp, strerror(errno));
	if (durable_rename(tmp, dest) != 0)
		wf_format_error("could not install %s", dest);
}
