# Create test_restore, a user that can import a mariadb-dump --system=all dump.
# The dump grants PROXY ON ''@'%', which needs the PROXY grant option.
CREATE USER test_restore IDENTIFIED BY 'r';
GRANT ALL ON *.* TO test_restore WITH GRANT OPTION;
GRANT PROXY ON ''@'%' TO test_restore WITH GRANT OPTION;
