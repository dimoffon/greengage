-- Figure 3: the load per hour of the day of every cluster
SELECT instance_id, extract(hour FROM arrival_timestamp)::int AS hour,
       count(*) AS queries,
       count(*) FILTER (WHERE query_type <> 'select') AS writes,
       sum(execution_duration_ms) AS exec_ms
FROM provisioned
GROUP BY 1, 2
ORDER BY 1, 2;
