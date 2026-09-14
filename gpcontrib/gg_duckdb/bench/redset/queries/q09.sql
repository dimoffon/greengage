-- Figure 6: the share of repeating statements by run-time bucket (a statement's bucket is its longest run)
WITH q AS (
    SELECT instance_id, feature_fingerprint, count(*) AS runs, max(execution_duration_ms) AS max_ms
    FROM provisioned
    WHERE feature_fingerprint IS NOT NULL
    GROUP BY 1, 2)
SELECT CASE WHEN max_ms < 10 THEN '1 <10ms' WHEN max_ms < 100 THEN '2 <100ms' WHEN max_ms < 1000 THEN '3 <1s'
            WHEN max_ms < 10000 THEN '4 <10s' WHEN max_ms < 60000 THEN '5 <1min' WHEN max_ms < 600000 THEN '6 <10min'
            WHEN max_ms < 3600000 THEN '7 <1h' WHEN max_ms < 36000000 THEN '8 <10h' ELSE '9 >=10h' END AS bucket,
       count(*) AS distinct_queries,
       sum(runs) AS runs,
       round(100.0 * sum(runs) FILTER (WHERE runs > 1) / sum(runs), 1) AS repeating_pct,
       count(DISTINCT instance_id) AS clusters
FROM q
GROUP BY 1
ORDER BY 1;
