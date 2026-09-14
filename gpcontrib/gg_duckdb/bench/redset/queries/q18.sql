-- Table 9 and Figure 8: writes per table from the comma-separated table lists, the fifty most written
SELECT instance_id, t.table_id, count(*) AS writes,
       count(*) FILTER (WHERE query_type = 'delete') AS deletes,
       count(*) FILTER (WHERE query_type IN ('insert', 'copy', 'ctas')) AS inserts
FROM provisioned, unnest(string_to_array(write_table_ids, ',')) AS t(table_id)
WHERE write_table_ids IS NOT NULL
GROUP BY 1, 2
ORDER BY writes DESC, instance_id, table_id
LIMIT 50;
