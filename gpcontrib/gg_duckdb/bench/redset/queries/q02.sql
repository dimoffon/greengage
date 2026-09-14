-- Table 3: statements and their run time by run-time bucket
SELECT CASE WHEN execution_duration_ms < 10 THEN '1 (0s,10ms]'
            WHEN execution_duration_ms < 100 THEN '2 (10ms,100ms]'
            WHEN execution_duration_ms < 1000 THEN '3 (100ms,1s]'
            WHEN execution_duration_ms < 10000 THEN '4 (1s,10s]'
            WHEN execution_duration_ms < 60000 THEN '5 (10s,1min]'
            WHEN execution_duration_ms < 600000 THEN '6 (1min,10min]'
            WHEN execution_duration_ms < 3600000 THEN '7 (10min,1h]'
            WHEN execution_duration_ms < 36000000 THEN '8 (1h,10h]'
            ELSE '9 >=10h' END AS bucket,
       count(*) AS queries,
       round(100.0 * count(*) / sum(count(*)) OVER (), 2) AS pct_queries,
       sum(execution_duration_ms) AS exec_ms,
       round(100.0 * sum(execution_duration_ms) / sum(sum(execution_duration_ms)) OVER (), 2) AS pct_runtime
FROM provisioned
GROUP BY 1
ORDER BY 1;
