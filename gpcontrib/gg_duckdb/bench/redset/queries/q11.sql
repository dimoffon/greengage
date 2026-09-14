-- Section 4.5: the sets of tables select statements read, the hundred most common ones
SELECT instance_id, read_table_ids, count(*) AS queries,
       sum(execution_duration_ms) AS exec_ms, avg(mbytes_scanned)::bigint AS avg_mb
FROM provisioned
WHERE query_type = 'select' AND read_table_ids IS NOT NULL
GROUP BY 1, 2
ORDER BY queries DESC, instance_id, read_table_ids
LIMIT 100;
