-- Table 2 for both tiers: the provisioned and the serverless clusters side by side
SELECT tier, query_type, count(*) AS queries,
       sum(execution_duration_ms) AS exec_ms, sum(mbytes_scanned) AS mb_scanned,
       count(*) FILTER (WHERE was_aborted) AS aborted, count(*) FILTER (WHERE was_cached) AS cached
FROM (SELECT 'provisioned' AS tier, query_type, execution_duration_ms, mbytes_scanned, was_aborted, was_cached
      FROM provisioned
      UNION ALL
      SELECT 'serverless', query_type, execution_duration_ms, mbytes_scanned, was_aborted, was_cached
      FROM serverless) u
GROUP BY 1, 2
ORDER BY 1, 3 DESC;
