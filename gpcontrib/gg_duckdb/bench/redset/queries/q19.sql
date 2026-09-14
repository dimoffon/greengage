-- The heavy users of every cluster and how many databases they touch
SELECT instance_id, user_id, count(*) AS queries, count(DISTINCT database_id) AS databases,
       sum(execution_duration_ms) AS exec_ms, sum(mbytes_scanned) AS mb_scanned,
       count(*) FILTER (WHERE query_type = 'select') AS selects
FROM provisioned
GROUP BY 1, 2
HAVING count(*) >= 1000
ORDER BY exec_ms DESC, instance_id, user_id
LIMIT 50;
