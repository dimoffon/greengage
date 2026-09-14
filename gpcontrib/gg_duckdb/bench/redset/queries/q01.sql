-- Table 2: query types, their share of the statements and of the run time, with the paper's RO/RW/Sys classes
SELECT t.query_class, p.query_type,
       count(*) AS queries,
       sum(p.execution_duration_ms) AS exec_ms,
       avg(p.execution_duration_ms)::bigint AS avg_ms,
       count(*) FILTER (WHERE p.was_aborted) AS aborted,
       sum(p.mbytes_scanned) AS mb_scanned
FROM provisioned p JOIN redset.query_types t ON t.query_type = p.query_type
GROUP BY 1, 2
ORDER BY 3 DESC;
