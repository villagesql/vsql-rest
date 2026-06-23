# Wait for the REST HTTP listener to bind, then capture the actual
# (OS-assigned) port into $REST_PORT. A port=0 listener rebinds to a new port
# on every enable, so re-source this after re-enabling the server.
--disable_query_log
let $wait_condition= SELECT VARIABLE_VALUE > 0 FROM performance_schema.global_status WHERE VARIABLE_NAME = 'vsql_rest.http_port';
--source include/wait_condition.inc
--let $REST_PORT = `SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = 'vsql_rest.http_port'`
--enable_query_log
