-- Scan and spill volume by cluster size (provisioned clusters)
SELECT cluster_size, count(*) AS queries,
       sum(mbytes_scanned) AS mb_scanned, sum(mbytes_spilled) AS mb_spilled,
       count(*) FILTER (WHERE mbytes_spilled > 0) AS spilling,
       max(mbytes_spilled) AS max_spill_mb,
       sum(execution_duration_ms) FILTER (WHERE mbytes_spilled > 0) AS spilling_ms
FROM provisioned
WHERE query_type IN ('select', 'ctas', 'insert')
GROUP BY 1
ORDER BY 1;
