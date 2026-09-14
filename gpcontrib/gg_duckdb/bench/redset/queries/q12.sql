-- Section 4.6: result-cache hits joined to the statement that filled the cache: the run time they saved
SELECT c.instance_id, count(*) AS hits,
       sum(s.execution_duration_ms) AS saved_ms,
       avg(s.execution_duration_ms)::bigint AS avg_source_ms,
       sum(s.mbytes_scanned) AS saved_mb
FROM provisioned c
JOIN provisioned s ON s.instance_id = c.instance_id AND s.query_id = c.cache_source_query_id
WHERE c.was_cached
GROUP BY 1
ORDER BY hits DESC, c.instance_id;
