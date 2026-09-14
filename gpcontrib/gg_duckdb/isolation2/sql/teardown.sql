DROP TABLE iso_t;
!\retcode gpconfig -c shared_preload_libraries -v "$(psql -At -c "SELECT array_to_string(array_remove(string_to_array(current_setting('shared_preload_libraries'), ','), 'gg_duckdb'), ',')" postgres)";
!\retcode gpstop -raq -M fast;
