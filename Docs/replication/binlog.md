# New binlog implementation

This document describes the new binlog implementation that is enabled using
the `--binlog-storage-engine` option.

The new binlog uses a more efficient on-disk format that is integrated with
the InnoDB write-ahead log. This provides two main benefits:

1. The binlog will always be recovered into a consistent state after a crash. This makes it possible to use the options `--innodb-flush-log-at-trx-commit=0` or `--innodb-flush-log-at-trx-commit=2`, which can give a huge performance improvement depending on disk speed and transaction concurrency.
2. When using the option `--innodb-flush-log-at-trx-commit=1`, commits are more efficient since there is no expensive two-phase commit between the binlog and the InnoDB storage  engine.

## Using the new binlog

To use the new binlog, configure the server with the following two options:
1. `log_bin`
2. `binlog_storage_engine=innodb`

Note that the `log_bin` option must be specified like that, without any argument; the option is not an on-off-switch.

Optionally, the directory in which to store binlog files can be specified with `binlog_directory=<DIR>`. By default, the data directory is used.

Note that using the new binlog is mutually exclusive with the traditional binlog format. Configuring an existing server to use the new binlog format will effectively ignore any old binlog files. This limitation may be relaxed in a future version of MariaDB.

## Replicating with the new binlog

Configuration of replication from a master using the new binlog is done in the usual way. Slaves must be configured to use Global Transaction ID (GTID) to connect to the master (this is the default). The old filename/offset-based replication position is not available when using the new binlog implementation on the master.

## Working with the binlog files

The binlog files will be written to the data directory (or to the directory configured with `--binlog-directory`). The files are named `binlog-000000.ibb`, `binlog-000001.ibb`, ... and so on.

The size of each binlog file is determined by the value of `max_binlog_size` (by default 1 GB). The binlog files are pre-allocated, so they will always have the configured size, with the last one or two files being possibly partially empty. The exception is when the command `FLUSH BINARY LOGS` is used; then the last active binlog file will be truncated to the used part of it, and binlog writes will continue in the following file.

The list of current binlog files can be obtained with the command `SHOW BINLOG EVENTS`. Note that there is no binlog index file (`.index`) like with the traditional binlog format, nor are there any GTID index files (`.idx`) or GTID state (`.state`) file (`.state`).

Instead of the GTID index and state files, the new binlog periodically writes GTID *state records* into the binlog containing the equivalent information. When a slave connects to the master, as well as when the server starts up, the binlog will be scanned from the last GTID state record to find or recover the correct GTID position. The `--innodb-binlog-state-interval` configuration option controls the interval (in bytes) between writing a state record. Thus, this option can be increased to reduce the overhead of state records, or decreased to speed up finding the initial GTID position at slave connect. The overhead however is small either way, and normally there is little reason to change the default. The status variables `binlog_gtid_index_hit` and `binlog_gtid_index_miss` are not used with the new binlog implementation.

Binlog files can be purged (removed) automatically after a configured time or disk space usage, provided they are no longer needed by active replication slaves or for possible crash recovery. This is configured using the options `binlog_expire_log_seconds`, `binlog_expire_log_days`, `max_binlog_total_size`, and `slave_connections_needed_for_purge`.

The contents of binlog files can be inspected in two ways:
1. From within the server, using the command `SHOW BINLOG EVENTS`.
2. Independent of the server, using the `mariadb-binlog` command-line program.

Unlike in the traditional binlog format, one binlog event can be stored in multiple different binlog files, and different parts of individual events can be interleaved with one another in each file. The `mariadb-binlog` program will coalesce the different parts of each event as necessary, so the output of the program is a consistent, non-interleaved stream of events. To obtain a correct seequnce of events across multiple binlog files, all binlog files should be passed to the `mariadb-binlog` program at once in correct order; this ensures that events that cross file boundaries are included in the output exactly once.

When using the `--start-position` and `--stop-position` options of `mariadb-binlog`, it is recommended to use GTID positions. The event file offsets used in the tranditional binlog format are not used in the new binlog, and will mostly be reported as zero.

The `--binlog-checksum` option is no longer used with the new binlog implementation. The binlog files are always checksummed, with a CRC32 at the end of each page. To have checksum of the data sent on the network between the master and the slave (in addition to the normal TCP checksums), use the `MASTER_SSL` option for `CHANGE MASTER` to make the connection use SSL.

## Using the new binlog with mariadb-backup

The `mariadb-backup` program will by default back up the binlog files together with the rest of the server data. This fixes a long-standing limitation of the old binlog that it is missing from backups made with `mariadb-backup`.

The binlog files are backed up in a transactionally consistent way, just like other InnoDB data. This means that a restored backup can be used to setup a new slave simply by using the `MASTER_DEMOTE_TO_SLAVE=1` option of `CHANGE MASTER`.

The server being backed up is not blocked during the copy of the binlog files; only `RESET MASTER`, `PURGE BINARY LOGS` and `FLUSH BINARY LOGS` are blocked by default. This blocking can be disabled with the option `--no-lock` option.

To omit the binlog files from the backup (ie. to save space in the backup when the binlog files are known to be not needed), use the `--skip-binlog` option on both the `mariadb-backup --backup` and `mariadb-backup --prepare` step. Note that when binlog files are omitted from the backup, the restored server will behave as if `RESET MASTER` was run on it just at the point of the backup. Also note that any transactions that were prepared, but not yet committed, at the time of the backup will be rolled back when the restored server starts up for the first time.

## `FLUSH BINARY LOGS`

Binlog files are pre-allocated for efficiency. When binlog file N is filled up, any remainder event data continues in file N+1, and and empty file N+2 is pre-allocated in the background. This means that binlog files are always exactly `--max-binlog-size` bytes long; and if the server restarts, binlog writing continues at the point reached before shutdown.

The exception is when the `FLUSH BINARY LOGS` command is run. This pads the current binlog file up to the next page boundary, truncates the file, and switches to the next file. This can thus leave the binlog file smaller than `--max-binlog-size` (but always a multiple of the binlog page size).

The `FLUSH BINARY LOGS DELETE_DOMAIN_ID=N` can be used to remove an old domain id from the `@@gtid_binlog_pos`. This requires that the domain is not in use in any existing binlog files; a combination of running `FLUSH BINARY LOGS` and `PURGE BINARY LOGS TO` can help ensure this. If the domain id N is already deleted, a warning is issues but the `FLUSH BINARY LOGS` operation is still run (this is for consistency, but is different from the old binlog implementation, where the `FLUSH` is skipped if the domain id was already deleted.

## Upgrading

When switching an existing server to use the new binlog format, the old binlog files will not be available after the switch, as the two formats are mutually exclusive.

If the old binlog files are not needed after the transition, no special actions are needed. Just stop the server and restart with the configuration `--log-bin --binlog-storage-engine=innodb`. The new binlog will start empty. The old binlog files can be removed manually afterwards.

Optionally, note down the value of `@@binlog_gtid_state` and execute `SET GLOBAL binlog_gtid_state=<old value>` as the first thing after starting up, to preserve the GTID state. This can be used to migrate a replication setup. First stop all writes to the master, and let all slaves catch up. Then note down the value of `@@binlog_gtid_state`, restart the master with `--binlog-storage-engine=innodb`, and restore `@@binlog_gtid_state`. Then the slaves will be able to connect and continue from where they left off.

Alternatively, live migration can be done by switching a slave first. Restart a slave with the new `binlog-storage-engine=innodb` option and let the slave replicate for a while until it has sufficient binlog data in the new binlog format. Then promote the slave as the new master. The other slaves can then be stopped, switched to the new binlog, and restarted, as convenient.

When using the new binlog format for a new installation, nothing special is needed. Just configure `--binlog-storage-engine=innodb` on the new server installation.

When the new binlog format is enabled on a master, the slaves should be upgraded to at least MariaDB version 12.3 first. The slave can be switched to the new binlog format without upgrading the master first. The master and slave can use the old or the new binlog format independently of one another.

## Using the new binlog with 3rd-party programs

The new binlog uses a different on-disk format than the traditional binlog. The format of individual replication events is the same; however the files stored on disk are page-based, and each page has some encapsulation of event data to support splitting events in multiple pieces etc.

This means that existing 3rd-party programs that read the binlog files directly will need to be modified to support the new format. Until then, such programs will require using the traditional binlog format.

The protocol for reading binlog data from a running server (eg. for a connecting slave) is however mostly unchanged. This means existing programs that read binlog events from a running server may be able to function unmodified with the new binlog. Similarly, `mariadb-binlog` with the `--read-from-remote-server` option works as usual.

A difference is that file offsets and file bondaries are no longer meaningful and no longer reported to the connecting client. There are no rotate events at the end of a file to specify the name of the following file, nor is there a new format description event at the start of each new file. Effectively, the binlog appears as a single unbroken stream of events to clients. The position from which to start receiving binlog events from the server should be specified using a GTID position; specifying a filename and file offset is not available.

### Documentation of the binlog file format

A binlog file consists of a sequence of pages. The page size is currently fixed at 16kByte. The size of a binlog file is set with the `--max-binlog-size` option. Each page has a CRC32 in the last 4 bytes; all remaining bytes are used for data.

Numbers are stored in little-endian format. Some numbers are stored as compressed integers. A compressed integer consists of 1-9 bytes. The lower 3 bits determine the number of bytes used. The number of bytes is one more than the value in the lower 3 bits, except that a value of 7 means that 9 bytes are used. The value of the number stored is the little-endian value of the used bytes, right-shifted by 3.

The first page in each binlog file is a file header page, with the following format:

| Offset | Size | Description |
| -----: | ---: | :---------- |
|      0 |    4 | 4-byte MAGIC value 0x010dfefe to identify the file as a binlog file. |
|      4 |    4 | The log-2 of the page size, currently fixed at 14 for a 16kByte page size. |
|      8 |    4 | Major file version, currently 1. A new major version is not readable by older server versions. |
|     12 |    4 | Minor file version, currently 0. New minor versions are backwards-compatible with older server versions. |
|     16 |    8 | The file number (same as the number in the `binlog-NNNNNN.ibb` file name), for consistency check. |
|     24 |    8 |  The size of the file, in pages. |
|     32 |    8 |  The InnoDB LSN corresponding to the start of the file, used for crash recovery. |
|     40 |    8 | The value of `--innodb-binlog-state-interval` used in the file. |
|     48 |    8 |  The file number of the earliest file into which this file may contain references into (such references occur when a large event group is split into multiple pieces, called out-of-band or oob records, and are used to locate all the pieces of event data from the final commit record). Used to prevent purging binlog files that contain data that may still be needed. |
|     56 |    8 | The file number of the earliest file containing pending XA transactions that may still be active. |
|     64 |  448 | Unused. |
|    512 |    4 | Extra CRC32. This is used for future expansion to support configurable binlog page size. The header page can be read and checksummed as a 512-byte page, after which the real page size can be determined from the value at offset 4. |

Remaining pages in the file are data pages. Data is structured as a sequence of *records*; each record consists of one or more chunks. A page contains one or more chunks; a record can span multiple pages, but a chunk always fits within one page. Chunks are a minumum of 4 bytes long; any remaining 1-3 bytes of data in a page are filled with the byte 0xff. Unused bytes in a page are set to 0x00.

A chunk consists of a *type byte*, two *length bytes* (little endian), and the *data bytes*. The length bytes count only the data bytes, so if the length bytes are 0x0001, then the total size of the chunk is 4 bytes.

The high two bits of the type byte are used to collect chunks into records:
- Bit 7 is clear for the first chunk in a record, and set for all following chunks in the record.
- Bit 6 is set for the last chunk in a record, and clear for any prior chunks.

These are the chunk types used:

| Type | Description |
| ---: | :---------- |
|    0 | (not a real type, 0 is an unused byte and denotes end-of-file in the current binlog file). |
|    1 | A commit record, containing binlog event data. First a compressed integer of the number of oob records referenced by the commit record, if any; then if non-zero, four more compressed integers of the file number and offset of the first and last such reference. This is followed by another similar 1 or 5 compressed integers, only used in the special case where transactional and non-transactional updates are mixed in a single event group. The remainder bytes are the payload data. |
|    2 | This is a GTID state record, written every `--innodb-binlog-state-interval` bytes. It consists of a sequence of compressed integers. The first is the number of GTIDs in the GTID state stored in the record. The second is one more than the earliest file number containing possibly still active XA transactions (used for crash recovery), or 0 for none. After this comes N*3 compressed integers, each representing a GTID in the GTID state. |
|    3 | This is an out-of-band (oob) data record. It is used to split large event groups into smaller pieces, organized as a forest of perfect binary trees. The record starts with 5 compressed integers: A node index (starts at 0 and increments for each oob record in an event group); the file number and offset of the left child oob node; and the file number and offset of the right child oob node. Remainder bytes are the payload event data. |
|    4 | This is a filler record, it is used to pad out the last page of a binlog file which is truncated due to `FLUSH BINARY LOGS`. |
|    5 | This is an XA PREPARE record, used for consistent crash recovery of user XA transactions. It starts with 1 byte counting the number of storage engines participating in the transaction. Then follows the XID (4 byte formatID; 1 byte gtrid length; 1 byte bqual length; and the characters of the XID name). Finally 5 compressed integers: the number of referenced oob nodes; the file number and offset of the first one; and the file number and offset of the last one. |
|    6 | This is an XA complete record, used for recovery purposes for internal 2-phase commit transactions and user XA. The first byte is a type byte, which is 0 for commit and 1 for XA rollback. Then follows 6-134 bytes of the XID, in the same format as for the XA PREPARE record. |

## Not supported

A few things are not supported with the new binlog implementation. Some of these should be supported in a later version of MariaDB.

- Old-style filename/offset replication positions are not available with the new binlog. Slaves must be configured to use GTID (this is the default). Event offsets are generally reported as zero. `MASTER_POS_WAIT()` is not available, `MASTER_GTID_WAIT()` should be used instead. Similarly, `BINLOG_GTID_POS()` is not available.
- Semi-synchronous replication is not supported in the first version. It will be supported as normal eventually using the `AFTER_COMMIT` option. The `AFTER_SYNC` option cannot be supported, as the expensive two-phase commit between binlog and engine is no longer needed (`AFTER_SYNC` waits for slave acknowledgement in the middle of the two-phase commit). Likewise, `--init-rpl-role` is not supported.
- The new binlog implementation cannot be used with Galera.
- In the initial version, only InnoDB is available as an engine for the binlog (`--binlog-storage-engine=innodb`). It the future, other transactional storage engines could implement storing the binlog themselves (performance is best when the binlog is implemented in the same engine as the tables that are updated).
- The `sync_binlog` option is no longer needed and is effectively ignored. Since the binlog files are now crash-safe without needing any syncing. The durability of commits is now controlled solely by the `--innodb-flush-log-at-trx-commit` option, which now applies to both binlog files and InnoDB table data.
- The command `RESET MASTER TO` is not available with the new binlog.
- The `--tc-heuristic-recover` option is not needed with the new binlog and cannot be used. Any pending prepared transactions will always be rolled back or committed to be consistent with the binlog. If the binlog is empty (ie. has been deleted manually), pending transactions will be rolled back.
- Binlog encryption is not available. It is suggested to use filesystem-level encryption facilities of the operating system instead, and/or use SSL for the slave's connection to the master.
- SHOW BINLOG EVENTS FROM will not give an error for a position that starts in the middle of an event group. Instead, it will start from the first GTID event following the position (or return empty, if the position is past the end).
