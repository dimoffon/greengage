-- Section 6: a profile of every cluster and the busyness score Redset was sampled by
-- (statements and run time, each normalised by the busiest cluster)
SELECT instance_id, cluster_size, queries, exec_ms, max_ms, mb_scanned, mb_spilled,
       round(queries / max(queries) OVER ()::numeric + exec_ms / max(exec_ms) OVER ()::numeric, 3) AS busyness
FROM (SELECT instance_id, max(cluster_size) AS cluster_size, count(*) AS queries,
             sum(execution_duration_ms) AS exec_ms, max(execution_duration_ms) AS max_ms,
             sum(mbytes_scanned) AS mb_scanned, sum(mbytes_spilled) AS mb_spilled
      FROM provisioned
      GROUP BY 1) s
ORDER BY busyness DESC, instance_id;
