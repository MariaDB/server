# Start capturing the statements the remote server receives.
connection slave;
TRUNCATE TABLE mysql.general_log;
SET global general_log=1;
connection master;
