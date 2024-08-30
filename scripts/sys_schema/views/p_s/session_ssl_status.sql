--
-- View: session_ssl_status
--
-- Shows SSL version, cipher and the count of re-used SSL sessions per connection
--
-- mysql> select * from session_ssl_status;
-- +-----------+-------------+--------------------+---------------------+
-- | thread_id | ssl_version | ssl_cipher         | ssl_sessions_reused |
-- +-----------+-------------+--------------------+---------------------+
-- |        26 | TLSv1       | DHE-RSA-AES256-SHA | 0                   |
-- |        27 | TLSv1       | DHE-RSA-AES256-SHA | 0                   |
-- |        28 | TLSv1       | DHE-RSA-AES256-SHA | 0                   |
-- +-----------+-------------+--------------------+---------------------+
-- 3 rows in set (0.00 sec)
--

CREATE OR REPLACE
  ALGORITHM = MERGE
  DEFINER = 'mariadb.sys'@'localhost'
  SQL SECURITY INVOKER
VIEW session_ssl_status (
  thread_id,
  ssl_version,
  ssl_cipher,
  ssl_sessions_reused
) AS
SELECT sslver.thread_id, 
       sslver.variable_value ssl_version, 
       sslcip.variable_value ssl_cipher,
       sslreuse.variable_value ssl_sessions_reused
  FROM performance_schema.status_by_thread sslver 
  LEFT JOIN performance_schema.status_by_thread sslcip 
    ON (sslcip.thread_id=sslver.thread_id and sslcip.variable_name='Ssl_cipher')
  LEFT JOIN performance_schema.status_by_thread sslreuse 
    ON (sslreuse.thread_id=sslver.thread_id and sslreuse.variable_name='Ssl_sessions_reused') 
 WHERE sslver.variable_name='Ssl_version';
