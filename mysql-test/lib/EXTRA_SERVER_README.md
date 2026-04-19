# Extra Server Script for MySQL Test Framework

## Overview

This script allows you to dynamically start additional MariaDB server instances during test execution while `mysql-test-run` is already running. This is useful for testing scenarios that require multiple independent server instances.

## Features

- **Dynamic server creation**: Start servers on-demand during test execution
- **Automatic port allocation**: Non-conflicting ports (base_port + 10 + N)
- **Automatic socket allocation**: Unique socket paths per server
- **Data directory management**: Copies from existing `var/install.db`
- **Connection info export**: Provides host, port, socket, datadir, PID

## Files

- `lib/start_extra_server.pl` - Perl script that starts the extra server
- `include/start_extra_server.inc` - Test include file to start server
- `include/stop_extra_server.inc` - Test include file to stop server
- `main/extra_server_example.test` - Example test demonstrating usage

## Usage

### Starting an Extra Server

```sql
# Set the server number (must be unique)
--let $extra_server_num= 1

# Optional: specify custom port
# --let $extra_server_port= 13307

# Optional: specify custom socket
# --let $extra_server_socket= /path/to/socket

# Start the server
--source include/start_extra_server.inc
```

After starting, the following variables are available:
- `$EXTRA_SERVER_PORT` - Port number
- `$EXTRA_SERVER_SOCKET` - Socket path
- `$EXTRA_SERVER_DATADIR` - Data directory
- `$EXTRA_SERVER_PID` - Process ID

### Connecting to the Extra Server

```sql
--connect (conn_name, 127.0.0.1, root, , test, $EXTRA_SERVER_PORT)
SELECT "Connected!" AS status;
# ... perform operations ...
--disconnect conn_name
```

### Stopping the Extra Server

```sql
--let $extra_server_num= 1
--source include/stop_extra_server.inc
```

## Port Allocation

Ports are automatically calculated to avoid conflicts:
- Master servers: `base_port + 0`, `base_port + 1`
- Slave servers: `base_port + 2`, `base_port + 3`, `base_port + 4`
- Extra servers: `base_port + 10 + server_num`

Default `base_port` is typically 10000 (or `MASTER_MYPORT` if set).

## Data Directory

The script copies `var/install.db` to `var/extra_server_N/data`, so:
- No bootstrap needed (system tables already exist)
- Fast startup
- Clean slate for each server

## Server Configuration

The extra server is started with:
- `--skip-grant-tables` (for easy test access)
- `--default-storage-engine=myisam`
- `--loose-skip-innodb` (for faster startup)
- Minimal memory settings

## Example Test

See `main/extra_server_example.test` for a complete working example.

## Invocation from mysqltest.cc

The script can be invoked using the `--exec` command in test files:

```sql
--exec perl $MYSQL_TEST_DIR/lib/start_extra_server.pl 1
```

Or more conveniently via the include files as shown above.

## Troubleshooting

### Server fails to start

Check the log file: `var/log/extra_server_N.err`

### Port conflicts

Specify a custom port:
```sql
--let $extra_server_port= 15000
```

### Connection issues

Verify the server is running:
```sql
--exec ps aux | grep extra_server
```

Check the info file:
```sql
--exec cat $MYSQLTEST_VARDIR/tmp/extra_server_1.info
```

## Limitations

- Servers run with `--skip-grant-tables` (no authentication)
- InnoDB is disabled by default (use `--loose-innodb` if needed)
- No automatic cleanup on test failure (use `--force` in mysql-test-run)

## Future Enhancements

- Support for custom mysqld options
- Better error handling and diagnostics
- Automatic cleanup on test failure
- Support for replication setup between extra servers
