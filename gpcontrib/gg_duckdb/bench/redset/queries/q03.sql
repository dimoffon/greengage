-- Table 4: plan operators per select statement by run-time class
SELECT CASE WHEN execution_duration_ms < 1000 THEN '(0s,1s]'
            WHEN execution_duration_ms < 60000 THEN '(1s,60s]'
            ELSE '(60s,inf)' END AS class,
       count(*) AS queries,
       avg(num_scans) AS scans, avg(num_joins) AS joins, avg(num_aggregations) AS aggregations,
       avg(num_permanent_tables_accessed) AS perm_tables,
       avg(num_external_tables_accessed) AS ext_tables,
       avg(num_system_tables_accessed) AS sys_tables
FROM provisioned
WHERE query_type = 'select'
GROUP BY 1
ORDER BY 1;
