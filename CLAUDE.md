# CLAUDE.md

Guidance for Claude Code (claude.ai/code) working in this repository.

## What this is

Greengage is an MPP analytical database continuing the Greenplum lineage on PostgreSQL. A
**coordinator** holds only metadata; N **primary segments** (usually mirrored) hold all user
data; clients connect only to the coordinator, which splits each query into fragments and
dispatches them to the segments.

**Single-node PostgreSQL reasoning is unsafe here.** Every table has a distribution policy,
the planner inserts Motion nodes that usually dominate query cost, there are two optimizers
(GPORCA and the PostgreSQL planner) whose costs are not comparable, and a change that is
correct on one backend can desynchronize a cluster.

## Which branch line am I on

They differ in ways that break commands. Check `configure.in`:

| | `6.x` | `7.x` |
|---|---|---|
| `PG_PACKAGE_VERSION` | 9.4.26 | 12.22 |
| Terminology | master, `MASTER_DATA_DIRECTORY` | coordinator, `COORDINATOR_DATA_DIRECTORY` |
| `gp_toolkit` | `src/backend/catalog/gp_toolkit.sql` | the `gpcontrib/gp_toolkit` extension |

`main` is a stale mirror of the 6.x line — target `6.x` or `7.x`, never `main`, despite what
`README.md` says about submitting against main.

## Build and test

```sh
git submodule update --init
./configure --with-perl --with-python --with-libxml --with-gssapi --prefix=/usr/local/gpdb
make -j8 && make -j8 install
source /usr/local/gpdb/greengage_path.sh     # not greenplum_path.sh
make create-demo-cluster
source gpAux/gpdemo/gpdemo-env.sh            # sets PGPORT and the data directory
make installcheck-world
```

On 6.x the README build path is `make GPROOT=~/build PARALLEL_MAKE_OPTS=-j8 devel -C gpAux`
instead; `devel` is the debug build and regression tests require it.

**`make check` and plain `make installcheck` do not work.** `check` never builds a cluster,
and `installcheck` includes tests known to fail — `make -C src/test/regress installcheck-good`
is the schedule that excludes them. Add new tests to `greengage_schedule`, never to the
upstream-inherited schedules. Tests need a UTF-8 locale (`export LANG=en_US.UTF-8`).

Local CI reproduction lives in `ci/readme.md` (docker images, behave, ORCA linter). Pull
request CI is GitHub Actions delegating to reusable workflows in `greengagedb/greengage-ci`.

## Reviewing a pull request

When asked to review a pull request, delegate the analysis to
**`@agent-greengage:greengage-mpp-reviewer`**. Read the diff with `gh pr diff` rather than
local git — CI checks this repository out shallow, and its history is large.

Post each finding as an inline comment on the line it applies to, then one summary comment.
The reviewer subagent is read-only by design, so it reports findings and you post them. You
cannot submit a formal GitHub review or approve a pull request — say "review comments
posted", not "approved".

Say plainly when a change needs no MPP review (documentation, comments, `gpMgmt` Python with
no backend interaction) rather than manufacturing findings.

## Deeper knowledge

The [`greengage` plugin](https://github.com/GreengageDB/gg-agent) carries skills for this
codebase — `greengage-internals` (MPP architecture and the recurring bug classes),
`greengage-build`, `greengage-testing`, `greengage-answer-files`, `greengage-debug`,
`greengage-ci`, `greengage-contribute`, plus operational skills for schema design, query
performance, loading, workload management and backup. Use them instead of re-deriving how
this codebase works.
