disable_query_log;
disable_result_log;

# Install test_plugin_server
eval INSTALL PLUGIN test_plugin_server SONAME '$PLUGIN_AUTH';

# Create proxied@localhost
CREATE USER proxied@localhost IDENTIFIED BY 'proxied_password';

# Create u1@localhost, u2@localhost, u3@localhst
CREATE USER u1@localhost IDENTIFIED WITH test_plugin_server AS 'proxied';
CREATE USER u2@localhost IDENTIFIED WITH test_plugin_server AS 'proxied';
CREATE USER u3@localhost IDENTIFIED WITH test_plugin_server AS 'proxied';

GRANT PROXY ON proxied@localhost TO u1@localhost, u2@localhost, u3@localhost;
enable_result_log;
enable_query_log;
