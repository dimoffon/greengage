-- Put gg_duckdb into shared_preload_libraries on every node and restart, as
-- gg_wait_sampling's tests do.  The plan node it produces is resolved by name
-- on the QE that receives the plan, so LOAD in one session is not enough.
--start_ignore
\! gpconfig -c shared_preload_libraries -v "$(psql -At -c "SELECT array_to_string(array_append(array_remove(string_to_array(current_setting('shared_preload_libraries'), ','), 'gg_duckdb'), 'gg_duckdb'), ',')" postgres)"
\! gpstop -raq -M fast
--end_ignore
