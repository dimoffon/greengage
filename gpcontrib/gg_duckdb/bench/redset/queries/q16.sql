-- Where the time goes: compilation, queueing and execution per statement type
SELECT query_type, count(*) AS queries,
       sum(compile_duration_ms) AS compile_ms, sum(queue_duration_ms) AS queue_ms,
       sum(execution_duration_ms) AS exec_ms,
       count(*) FILTER (WHERE queue_duration_ms > 0) AS queued,
       max(queue_duration_ms) AS max_queue_ms,
       count(*) FILTER (WHERE compile_duration_ms > execution_duration_ms) AS compile_dominated
FROM provisioned
GROUP BY 1
ORDER BY exec_ms DESC;
