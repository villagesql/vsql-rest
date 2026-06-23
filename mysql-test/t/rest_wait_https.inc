# Wait for the REST HTTPS listener to bind, then capture the actual
# (OS-assigned) port into $REST_SSL_PORT. See rest_wait_http.inc.
--disable_query_log
let $wait_condition= SELECT VARIABLE_VALUE > 0 FROM performance_schema.global_status WHERE VARIABLE_NAME = 'vsql_rest.https_port';
--source include/wait_condition.inc
--let $REST_SSL_PORT = `SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = 'vsql_rest.https_port'`
--enable_query_log
