-- Aborted statements by type and run-time class, and the run time they wasted
SELECT query_type,
       CASE WHEN execution_duration_ms < 1000 THEN '(0s,1s]'
            WHEN execution_duration_ms < 60000 THEN '(1s,60s]'
            ELSE '(60s,inf)' END AS class,
       count(*) AS queries,
       count(*) FILTER (WHERE was_aborted) AS aborted,
       round(100.0 * count(*) FILTER (WHERE was_aborted) / count(*), 2) AS aborted_pct,
       sum(execution_duration_ms) FILTER (WHERE was_aborted) AS wasted_ms
FROM provisioned
GROUP BY 1, 2
ORDER BY 1, 2;
