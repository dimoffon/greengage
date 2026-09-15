--
-- Heap tables a region reads directly (gg_rel): the region computes the
-- sequential scan's target list and filter itself, over the table's pages.
-- Every check compares the region's result with the standard executor's:
-- "missing | extra" rows, both 0.
--
CREATE TABLE r_t (id int, qty int, amount numeric(12,2), note text, big text, d date, gone int, flag bool)
    DISTRIBUTED BY (id);
ALTER TABLE r_t ALTER COLUMN big SET STORAGE EXTERNAL;
INSERT INTO r_t
    SELECT i, i % 50, (i % 997) / 8.0,
           CASE WHEN i % 13 = 0 THEN NULL ELSE 'note ' || (i % 101) END,
           CASE WHEN i % 500 = 1 THEN repeat(md5(i::text), 400) END,
           date '2024-01-01' + i % 400, i, i % 3 = 0
    FROM generate_series(1, 30000) i;
-- a dropped column, and one added with a default the old rows do not store
ALTER TABLE r_t DROP COLUMN gone;
ALTER TABLE r_t ADD COLUMN extra int DEFAULT 7;
INSERT INTO r_t (id, qty, amount, note, d, flag, extra)
    SELECT i, i % 50, i / 4.0, 'late ' || i, date '2025-01-01', true, i % 5
    FROM generate_series(30001, 32000) i;
-- dead tuples and new versions on the pages
DELETE FROM r_t WHERE id % 10 = 0;
UPDATE r_t SET qty = qty + 1, note = note || '*' WHERE id % 7 = 0;
CREATE TABLE r_dim (k int, label text) DISTRIBUTED BY (k);
INSERT INTO r_dim SELECT i, 'k' || i FROM generate_series(0, 60) i;
CREATE TABLE r_empty (a int, b numeric(10,2)) DISTRIBUTED BY (a);
CREATE TABLE r_part (id int, k int, v numeric(10,2)) DISTRIBUTED BY (id)
    PARTITION BY RANGE (k) (START (0) END (40) EVERY (10));
INSERT INTO r_part SELECT i, i % 40, i / 3.0 FROM generate_series(1, 20000) i;
ANALYZE r_t;
ANALYZE r_dim;
ANALYZE r_part;

CREATE OR REPLACE FUNCTION r_check(q text) RETURNS text LANGUAGE plpgsql AS $$
DECLARE missing bigint; extra bigint;
BEGIN
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'CREATE TEMP TABLE r_base AS ' || q;
  SET LOCAL gg_duckdb.mode = force;
  EXECUTE 'CREATE TEMP TABLE r_duck AS ' || q;
  SET LOCAL gg_duckdb.mode = off;
  EXECUTE 'SELECT count(*) FROM (TABLE r_base EXCEPT ALL TABLE r_duck) x' INTO missing;
  EXECUTE 'SELECT count(*) FROM (TABLE r_duck EXCEPT ALL TABLE r_base) x' INTO extra;
  DROP TABLE r_base; DROP TABLE r_duck;
  RETURN missing || ' | ' || extra;
END $$;

-- the scan's filter and target list move into the region's query
SET gg_duckdb.mode = force;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT qty, count(*), sum(amount) FROM r_t WHERE flag AND qty > 3 GROUP BY qty;
-- a filter the region cannot compute keeps the scan with the executor
EXPLAIN (COSTS OFF)
SELECT qty, count(*) FROM r_t WHERE md5(note) > 'a' GROUP BY qty;
-- and so does turning direct scans off
SET gg_duckdb.direct_scans = off;
EXPLAIN (COSTS OFF)
SELECT qty, count(*), sum(amount) FROM r_t WHERE flag AND qty > 3 GROUP BY qty;
RESET gg_duckdb.direct_scans;
RESET gg_duckdb.mode;

SELECT r_check($q$ SELECT qty, count(*) AS n, sum(amount) AS s, count(note) AS cn, min(d) AS md
                   FROM r_t WHERE flag AND qty > 3 GROUP BY qty $q$);
SELECT r_check($q$ SELECT qty, count(*) AS n FROM r_t WHERE note IS NULL OR amount > 100 GROUP BY qty $q$);
-- the added column: its default for old rows, stored values for new ones
SELECT r_check($q$ SELECT extra, count(*) AS n, sum(qty) AS s FROM r_t GROUP BY extra $q$);
-- toasted values
SELECT r_check($q$ SELECT id % 7 AS g, count(big) AS n, max(big COLLATE "C") AS m FROM r_t GROUP BY 1 $q$);
-- no column at all
SELECT r_check($q$ SELECT count(*) AS n FROM r_t $q$);
SELECT r_check($q$ SELECT a, sum(b) AS s FROM r_empty GROUP BY a $q$);
-- two direct scans joined
SELECT r_check($q$ SELECT d.label, count(*) AS n, sum(t.amount) AS s
                   FROM r_t t JOIN r_dim d ON t.qty = d.k WHERE t.note LIKE 'note 1%' GROUP BY d.label $q$);
-- rescanned: a correlated subquery
SELECT r_check($q$ SELECT k, (SELECT count(*) FROM (SELECT id FROM r_t WHERE qty = r_dim.k AND flag GROUP BY id) s) AS n
                   FROM r_dim $q$);
-- the partitions of a partitioned table
SELECT r_check($q$ SELECT k % 4 AS g, count(*) AS n, sum(v) AS s FROM r_part WHERE k < 30 GROUP BY 1 $q$);

-- a parameter in the filter
PREPARE r_p(int) AS SELECT qty, count(*), sum(amount) FROM r_t WHERE id > $1 GROUP BY qty ORDER BY qty LIMIT 4;
SET gg_duckdb.mode = off;
EXECUTE r_p(31000);
SET gg_duckdb.mode = force;
EXECUTE r_p(31000);
EXECUTE r_p(0);
SET gg_duckdb.mode = off;
EXECUTE r_p(0);
RESET gg_duckdb.mode;
DEALLOCATE r_p;

DROP FUNCTION r_check(text);
DROP TABLE r_t, r_dim, r_empty, r_part;
