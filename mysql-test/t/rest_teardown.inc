# Teardown: disable server, uninstall, drop schema.
SET GLOBAL vsql_rest.vsql_rest_enabled = OFF;
--sleep 0.5
UNINSTALL EXTENSION vsql_rest;
DROP DATABASE IF EXISTS test_rest;
