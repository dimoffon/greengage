-- A region reading a heap table directly sees what the executor's scan
-- sees: its transaction's snapshot, not rows committed later nor rows of a
-- transaction still in progress.
CREATE TABLE iso_ds (id int, v int) DISTRIBUTED BY (id);
INSERT INTO iso_ds SELECT i, i % 10 FROM generate_series(1, 100000) i;

1: SET gg_duckdb.mode = force;
1: BEGIN ISOLATION LEVEL REPEATABLE READ;
1: SELECT v, count(*) FROM iso_ds GROUP BY v ORDER BY v LIMIT 3;
-- committed after the snapshot (without the global deadlock detector a
-- delete or an update locks the table exclusively, so they come first)
DELETE FROM iso_ds WHERE v = 1;
UPDATE iso_ds SET v = 2 WHERE v = 0 AND id < 5000;
-- and a transaction still in progress
2: BEGIN;
2: INSERT INTO iso_ds SELECT i, 0 FROM generate_series(1, 1000) i;
-- the snapshot of the transaction: unchanged, as the executor sees it
1: SELECT v, count(*) FROM iso_ds GROUP BY v ORDER BY v LIMIT 3;
1: SET gg_duckdb.mode = off;
1: SELECT v, count(*) FROM iso_ds GROUP BY v ORDER BY v LIMIT 3;
1: COMMIT;
-- a new snapshot: the delete and the update, not the uncommitted insert
1: SET gg_duckdb.mode = force;
1: SELECT v, count(*) FROM iso_ds GROUP BY v ORDER BY v LIMIT 3;
2: ABORT;
1: SELECT v, count(*) FROM iso_ds GROUP BY v ORDER BY v LIMIT 3;
1: SET gg_duckdb.mode = off;
1: SELECT v, count(*) FROM iso_ds GROUP BY v ORDER BY v LIMIT 3;
1q:
2q:

DROP TABLE iso_ds;

-- the same for an append-optimized column table: its visibility map
CREATE TABLE iso_dsco (id int, v int) WITH (appendonly=true, orientation=column) DISTRIBUTED BY (id);
INSERT INTO iso_dsco SELECT i, i % 10 FROM generate_series(1, 100000) i;

1: SET gg_duckdb.mode = force;
1: BEGIN ISOLATION LEVEL REPEATABLE READ;
1: SELECT v, count(*) FROM iso_dsco GROUP BY v ORDER BY v LIMIT 3;
DELETE FROM iso_dsco WHERE v = 1;
UPDATE iso_dsco SET v = 2 WHERE v = 0 AND id < 5000;
2: BEGIN;
2: INSERT INTO iso_dsco SELECT i, 0 FROM generate_series(1, 1000) i;
1: SELECT v, count(*) FROM iso_dsco GROUP BY v ORDER BY v LIMIT 3;
1: COMMIT;
1: SELECT v, count(*) FROM iso_dsco GROUP BY v ORDER BY v LIMIT 3;
2: ABORT;
1: SET gg_duckdb.mode = off;
1: SELECT v, count(*) FROM iso_dsco GROUP BY v ORDER BY v LIMIT 3;
1q:
2q:

DROP TABLE iso_dsco;
