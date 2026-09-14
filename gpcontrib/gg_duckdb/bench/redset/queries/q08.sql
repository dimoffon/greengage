-- Section 4.4: how repetitive every cluster's statements are over the three months
-- (exact repeats of the fingerprint: literals included)
SELECT instance_id,
       count(*) AS queries,
       count(DISTINCT feature_fingerprint) AS distinct_queries,
       round(100.0 * (count(*) - count(DISTINCT feature_fingerprint)) / count(*), 1) AS repetition_pct
FROM provisioned
WHERE feature_fingerprint IS NOT NULL
GROUP BY 1
ORDER BY repetition_pct DESC, instance_id;
