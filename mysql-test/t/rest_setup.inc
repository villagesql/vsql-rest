# Common setup: install extension, create test schema, enable server.
--disable_warnings
DROP DATABASE IF EXISTS test_rest;
CREATE DATABASE test_rest;
--enable_warnings

CREATE TABLE test_rest.customers (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100), email VARCHAR(100), age INT);
CREATE TABLE test_rest.orders (
    id INT PRIMARY KEY AUTO_INCREMENT,
    customer_id INT, total DECIMAL(10,2), status VARCHAR(20),
    FOREIGN KEY (customer_id) REFERENCES test_rest.customers(id));

INSERT INTO test_rest.customers VALUES
    (1,'Alice','alice@example.com',25),
    (2,'Bob','bob@example.com',35),
    (3,'Carol','carol@example.com',45);
INSERT INTO test_rest.orders VALUES
    (1,1,99.99,'pending'),(2,1,49.50,'shipped'),
    (3,2,200.00,'pending'),(4,2,15.00,'cancelled'),(5,3,75.00,'pending');

DROP FUNCTION IF EXISTS test_rest.add_numbers;
CREATE FUNCTION test_rest.add_numbers(a INT, b INT) RETURNS INT DETERMINISTIC RETURN a + b;

DROP PROCEDURE IF EXISTS test_rest.mark_shipped;
delimiter //;
CREATE PROCEDURE test_rest.mark_shipped(IN order_id INT)
BEGIN
  UPDATE test_rest.orders SET status='shipped' WHERE id=order_id;
END//
delimiter ;//

# DIAG: probe state at each step to localize CI hang.
--echo DIAG: before INSTALL preview=`SELECT @@vsql_allow_preview_extensions` port=`SELECT @@port`
INSTALL EXTENSION vsql_rest;
--echo DIAG: after INSTALL
--disable_query_log
--let $REST_PORT = `SELECT @@port + 100`
--let $REST_SSL_PORT = `SELECT @@port + 200`
--eval SET GLOBAL vsql_rest.port = $REST_PORT
--enable_query_log
--echo DIAG: after set port
SET GLOBAL vsql_rest.schema = 'test_rest';
SET GLOBAL vsql_rest.schema_ttl = 2;
--echo DIAG: before enable
SET GLOBAL vsql_rest.vsql_rest_enabled = ON;
--echo DIAG: after enable

# Wait for server to start
--sleep 1.5
--echo DIAG: after sleep
