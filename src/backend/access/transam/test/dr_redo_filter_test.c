#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"

#include "postgres.h"

/*
 * Unit under test.  Including the .c directly gives the test access to the
 * file-scope protected-set state, so the matching logic can be exercised
 * without going through the relmapper.
 */
#include "../dr_redo_filter.c"

/*
 * Preload the protected relfilenode set, bypassing
 * DRResolveProtectedRelfilenodes() (and thus the relmapper).
 */
static void
set_protected(const Oid *filenodes, int n)
{
	int			i;

	for (i = 0; i < n; i++)
		dr_protected_filenodes[i] = filenodes[i];
	dr_num_protected_filenodes = n;
	dr_protected_resolved = true;
}

/* Build a one-block WAL record referencing (spc, db, rel). */
static void
init_one_block(XLogReaderState *rec, Oid spc, Oid db, Oid rel)
{
	memset(rec, 0, sizeof(*rec));
	rec->blocks[0].in_use = true;
	rec->blocks[0].rnode.spcNode = spc;
	rec->blocks[0].rnode.dbNode = db;
	rec->blocks[0].rnode.relNode = rel;
}

/* ---- DRRelfilenodeIsProtected(): pure matching logic, no mocks ---- */

static void
test_protected_filenode_matches(void **state)
{
	Oid			set[] = {16384};
	RelFileNode rnode = {.spcNode = GLOBALTABLESPACE_OID,
		.dbNode = InvalidOid,.relNode = 16384};

	set_protected(set, 1);
	assert_true(DRRelfilenodeIsProtected(&rnode));
}

static void
test_unprotected_filenode_does_not_match(void **state)
{
	Oid			set[] = {16384};
	RelFileNode rnode = {.spcNode = GLOBALTABLESPACE_OID,
		.dbNode = InvalidOid,.relNode = 99999};

	set_protected(set, 1);
	assert_false(DRRelfilenodeIsProtected(&rnode));
}

/* A matching filenode outside the global tablespace must NOT be protected. */
static void
test_wrong_tablespace_not_protected(void **state)
{
	Oid			set[] = {16384};
	RelFileNode rnode = {.spcNode = DEFAULTTABLESPACE_OID,
		.dbNode = InvalidOid,.relNode = 16384};

	set_protected(set, 1);
	assert_false(DRRelfilenodeIsProtected(&rnode));
}

/* A matching filenode that belongs to a database (not shared) must NOT match. */
static void
test_with_database_not_protected(void **state)
{
	Oid			set[] = {16384};
	RelFileNode rnode = {.spcNode = GLOBALTABLESPACE_OID,
		.dbNode = 12345,.relNode = 16384};

	set_protected(set, 1);
	assert_false(DRRelfilenodeIsProtected(&rnode));
}

/* Before the set is resolved, nothing is protected (filter is a no-op). */
static void
test_unresolved_protects_nothing(void **state)
{
	RelFileNode rnode = {.spcNode = GLOBALTABLESPACE_OID,
		.dbNode = InvalidOid,.relNode = 16384};

	dr_protected_resolved = false;
	assert_false(DRRelfilenodeIsProtected(&rnode));
}

/* ---- DRRedoShouldFilter(): gate + per-record block iteration (IsDRReplicaMode mocked) ---- */

/* Not in DR mode: never filter, regardless of the record. */
static void
test_not_dr_mode_never_filters(void **state)
{
	XLogReaderState rec;
	Oid			set[] = {16384};

	set_protected(set, 1);
	init_one_block(&rec, GLOBALTABLESPACE_OID, InvalidOid, 16384);

	will_return(IsDRReplicaMode, false);
	assert_false(DRRedoShouldFilter(&rec));
}

/* DR mode + record touches only a protected catalog: filter it. */
static void
test_dr_mode_filters_protected_record(void **state)
{
	XLogReaderState rec;
	Oid			set[] = {16384};

	set_protected(set, 1);
	init_one_block(&rec, GLOBALTABLESPACE_OID, InvalidOid, 16384);

	will_return(IsDRReplicaMode, true);
	assert_true(DRRedoShouldFilter(&rec));
}

/* DR mode but the record touches a non-protected relation: apply it. */
static void
test_dr_mode_applies_nonprotected_record(void **state)
{
	XLogReaderState rec;
	Oid			set[] = {16384};

	set_protected(set, 1);
	init_one_block(&rec, GLOBALTABLESPACE_OID, InvalidOid, 99999);

	will_return(IsDRReplicaMode, true);
	assert_false(DRRedoShouldFilter(&rec));
}

/* A record touching both a protected and a non-protected block must be applied. */
static void
test_dr_mode_mixed_blocks_applies(void **state)
{
	XLogReaderState rec;
	Oid			set[] = {16384};

	set_protected(set, 1);
	memset(&rec, 0, sizeof(rec));
	rec.blocks[0].in_use = true;
	rec.blocks[0].rnode.spcNode = GLOBALTABLESPACE_OID;
	rec.blocks[0].rnode.dbNode = InvalidOid;
	rec.blocks[0].rnode.relNode = 16384;	/* protected */
	rec.blocks[1].in_use = true;
	rec.blocks[1].rnode.spcNode = GLOBALTABLESPACE_OID;
	rec.blocks[1].rnode.dbNode = InvalidOid;
	rec.blocks[1].rnode.relNode = 99999;	/* not protected */

	will_return(IsDRReplicaMode, true);
	assert_false(DRRedoShouldFilter(&rec));
}

/* A record with no block references is never filtered. */
static void
test_dr_mode_no_blocks_applies(void **state)
{
	XLogReaderState rec;
	Oid			set[] = {16384};

	set_protected(set, 1);
	memset(&rec, 0, sizeof(rec));	/* no blocks in use */

	will_return(IsDRReplicaMode, true);
	assert_false(DRRedoShouldFilter(&rec));
}

int
main(int argc, char *argv[])
{
	cmockery_parse_arguments(argc, argv);

	const		UnitTest tests[] = {
		unit_test(test_protected_filenode_matches),
		unit_test(test_unprotected_filenode_does_not_match),
		unit_test(test_wrong_tablespace_not_protected),
		unit_test(test_with_database_not_protected),
		unit_test(test_unresolved_protects_nothing),
		unit_test(test_not_dr_mode_never_filters),
		unit_test(test_dr_mode_filters_protected_record),
		unit_test(test_dr_mode_applies_nonprotected_record),
		unit_test(test_dr_mode_mixed_blocks_applies),
		unit_test(test_dr_mode_no_blocks_applies)
	};

	return run_tests(tests);
}
