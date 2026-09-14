-- Table 6: the run-time distribution over all statements (median, p90, p99, p99.9)
SELECT count(*) AS queries,
       percentile_cont(ARRAY[0.5, 0.9, 0.99, 0.999]) WITHIN GROUP (ORDER BY execution_duration_ms) AS runtime_ms
FROM provisioned;
