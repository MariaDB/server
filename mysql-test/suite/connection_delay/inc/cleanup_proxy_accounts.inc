disable_query_log;
disable_result_log;
# Revoke proxy grants
REVOKE PROXY ON proxied@localhost FROM u1@localhost, u2@localhost, u3@localhost;

# Drop proxy users
DROP USER u1@localhost, u2@localhost, u3@localhost;

# Drop proxied user
DROP USER proxied@localhost;

# Uninstall test_plugin_server
UNINSTALL PLUGIN test_plugin_server;
UNINSTALL PLUGIN cleartext_plugin_server_return_bad_credentials;

enable_result_log;
enable_query_log;
