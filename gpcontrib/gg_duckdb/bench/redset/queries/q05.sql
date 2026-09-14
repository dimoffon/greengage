-- Figures 1 and 2: the daily load of every cluster, read-only versus writes, and its spikiness
-- (the root mean square of the day-to-day change, normalised by the cluster's busiest day)
WITH daily AS (
    SELECT instance_id, date_trunc('day', arrival_timestamp) AS day,
           count(*) FILTER (WHERE query_type = 'select') AS reads,
           count(*) FILTER (WHERE query_type IN ('insert', 'copy', 'delete', 'update', 'ctas', 'unload')) AS writes
    FROM provisioned
    GROUP BY 1, 2),
diffs AS (
    SELECT instance_id, day, reads, writes,
           reads + writes - lag(reads + writes) OVER (PARTITION BY instance_id ORDER BY day) AS delta,
           max(reads + writes) OVER (PARTITION BY instance_id) AS peak
    FROM daily)
SELECT instance_id, count(*) AS days, sum(reads) AS reads, sum(writes) AS writes,
       round(sqrt(avg(power(delta::numeric / peak, 2)))::numeric, 3) AS spikiness
FROM diffs
GROUP BY 1
ORDER BY spikiness DESC, instance_id;
