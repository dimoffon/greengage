-- Section 3.1, live dashboards: short statements over one cluster's last day or hour (timed together)
SELECT query_type, count(*) AS queries, avg(execution_duration_ms)::bigint AS avg_ms
FROM provisioned
WHERE instance_id = 6 AND arrival_timestamp >= '2024-05-30'
GROUP BY 1 ORDER BY 1;
SELECT count(*) AS queries, sum(mbytes_scanned) AS mb_scanned, max(execution_duration_ms) AS max_ms
FROM provisioned
WHERE instance_id = 6 AND arrival_timestamp >= '2024-05-30 12:00' AND arrival_timestamp < '2024-05-30 13:00';
SELECT user_id, count(*) AS queries
FROM provisioned
WHERE instance_id = 2 AND arrival_timestamp >= '2024-05-29'
GROUP BY 1 ORDER BY 2 DESC, 1 LIMIT 10;
SELECT query_id, arrival_timestamp, execution_duration_ms, query_type
FROM provisioned
WHERE instance_id = 6 AND query_id = 1000000;
SELECT count(*) FILTER (WHERE was_aborted) AS aborted, count(*) AS queries
FROM provisioned
WHERE instance_id = 42 AND arrival_timestamp >= '2024-05-24';
