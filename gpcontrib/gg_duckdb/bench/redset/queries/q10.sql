-- Figure 7: the time between repeats of the same select statement, its median and p90 per cluster
WITH gaps AS (
    SELECT instance_id,
           arrival_timestamp - lag(arrival_timestamp) OVER (PARTITION BY instance_id, feature_fingerprint
                                                            ORDER BY arrival_timestamp) AS gap
    FROM provisioned
    WHERE query_type = 'select' AND feature_fingerprint IS NOT NULL)
SELECT instance_id, count(*) AS repeats,
       percentile_cont(0.5) WITHIN GROUP (ORDER BY gap) AS median_gap,
       percentile_cont(0.9) WITHIN GROUP (ORDER BY gap) AS p90_gap
FROM gaps
WHERE gap IS NOT NULL
GROUP BY 1
ORDER BY repeats DESC, instance_id;
