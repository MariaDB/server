# mariadb-backup-server.sh — a BACKUP SERVER wrapper

`mariadb-backup-server.sh` makes the server-side `BACKUP SERVER` command look
like the old `mariadb-backup` tool. You call it with the familiar
`--backup` / `--prepare` / `--copy-back` options; under the hood
it just runs `BACKUP SERVER TO '<dir>'` over a normal `mariadb`
connection and lets the server do the work. It's plain POSIX `sh`,
so it runs anywhere `mariadb-backup` does.

You need a server that supports `BACKUP SERVER`, the `mariadb` client on
`PATH`, and an account allowed to run `BACKUP SERVER`. The parent of the
target directory has to exist and be writable.

## Installing / enabling it

The wrapper is experimental and off by default. Build with
`-DWITH_MARIABACKUP_WRAPPER=ON` and it installs next to the real
binaries, under its own names so it never shadows them:

```
/usr/bin/mariadb-backup          # the C++ binary, unchanged
/usr/bin/mariadb-backup-server   # this wrapper
/usr/bin/mbstream                # the C++ binary, unchanged
/usr/bin/mbstream-server         # the tar-based mbstream shim (for --stream)
```

To send your existing `mariadb-backup` (and `mbstream`) calls through the
wrapper instead, point the names at them yourself — an alias, or a symlink
earlier in `PATH`:

```sh
alias mariadb-backup=mariadb-backup-server
alias mbstream=mbstream-server
# or
ln -s /usr/bin/mariadb-backup-server ~/bin/mariadb-backup
ln -s /usr/bin/mbstream-server       ~/bin/mbstream
```

`mbstream-server` is only needed for `--stream` backups (it extracts the
wrapper's tar stream).

## Backing up

```sh
mariadb-backup-server --backup --target-dir=/backup/full
```

That runs `BACKUP SERVER TO '/backup/full'`. Use `--parallel=N` to ask for N
concurrent streams (`... N CONCURRENT`; N=1 is the default and changes
nothing).

Connection options:
`--user`, `--password`, `--host`, `--port`, `--socket`, `--defaults-file`,
`--defaults-extra-file` and their short forms are passed straight to
the `mariadb` client.

`--throttle`, `--no-lock` and `--safe-slave-backup` are accepted and ignored;

When the backup finishes the wrapper drops a `backup-prepare.cnf` into the
target dir, next to the server's own `backup.cnf`. It records where `mariadbd`
lives and the InnoDB layout, so `--prepare` can recover the backup later
without you respelling all of that.

### Streaming to stdout

```sh
mariadb-backup-server --backup --stream=tar > /backup/full.tar
```

`--stream` runs `BACKUP SERVER WITH [N CONCURRENT] '<command>'` instead of
`... TO '<dir>'`. The server hands each stream's tar to `<command>`, the wrapper
collects the parts and writes them to its stdout, so you can redirect to a file
or pipe onwards.

Two things follow from how `BACKUP SERVER` streams, and both differ from
`mariadb-backup`:

- **Local only.** The stream command runs *inside the server*, so its output
lands on the server's filesystem. The wrapper can only pick it up when it runs
on the same host (a shared filesystem), as the user the server writes as
(typically `mysql`), or with a `--target-dir` the server can write. A remote
`--host` cannot stream this way.

- **tar only.** The server emits tar, never `xbstream`. Any `--stream=<format>`
value (including `xbstream`) is accepted but the output is always tar.

`--target-dir` is optional here; when given it is just the scratch directory for
the per-stream parts (otherwise a `mktemp` dir is used).

The output is the per-stream tar entries concatenated, with
`backup-prepare.cnf` appended as the trailing archive.

```sh
mkdir restore && tar -xf /backup/full.tar -C restore
```

After extraction the directory holds the data files, the server's `backup.cnf`
*and* the wrapper's `backup-prepare.cnf`, so it can be prepared exactly like a
directory backup:

```sh
mariadb-backup-server --prepare --target-dir=restore
```

### `mbstream.sh` — the extraction shim

The real `mbstream`/`xbstream` binary cannot read the wrapper's stream, because
that stream is plain tar, not the `xbstream` format. So a companion shim,
`mbstream.sh`, ships next to `mariadb-backup-server.sh` and maps the `mbstream` CLI onto
`tar`, letting existing pipelines (and tests) that call `mbstream` keep working
unchanged:

```sh
mbstream-server -x -C restore < /backup/full.tar     # tar -x -C restore
mbstream-server -c -C dir file1 file2                # tar -c -C dir file1 file2
```

Notes:

- **Extraction is a plain `tar -x`** — the stream has a single
end-of-archive marker

- **mbstream-only options are accepted and ignored** —
`-p` / `--parallel`, compression flags, `-v` : So legacy
invocations don't break.

- It understands `-x` (extract, from stdin) and `-c` (create, to stdout),
with `-C <dir>`; anything else is treated as a file operand or ignored.

- It is a thin tar wrapper, so it only understands the wrapper's tar streams —
do not point it at a real `xbstream` archive, and do not feed the wrapper's
output to the real `mbstream`.

## Preparing

```sh
mariadb-backup-server --prepare --target-dir=/backup/full
```

Prepare makes the backup bootable: it starts `mariadbd --bootstrap` on the
target directory, replays the archived redo up to the backup's end LSN, then
replaces the archived log with a fresh `ib_logfile0` so a normal server can
start on it. Both `backup.cnf` and `backup-prepare.cnf` have to be there (they
are, if this wrapper took the backup).

Options:

- `--use-memory=N` — buffer pool size during recovery.
- `--innodb-*` and `--tmpdir` are forwarded to the bootstrap server.
- `--defaults-file` / `--defaults-extra-file`, and encryption options such as
  `--file-key-management*` / `--loose-file-key-management*` / `--plugin-load-add`,
  are layered onto the bootstrap (as an extra defaults file / extra options) so
  you can supply anything `backup-prepare.cnf` did not record.

The `mariadbd` used for the bootstrap is the path recorded in
`backup-prepare.cnf` at backup time if that binary exists; otherwise `mariadbd`
from `PATH`. (Recorded-first matters: it is the same version that took the
backup, so it can always parse the backed-up tablespace format.)

## Restoring

With the backup prepared and the server stopped, put it into the datadir:

```sh
mariadb-backup-server --copy-back --target-dir=/backup/full --datadir=/var/lib/mysql
mariadb-backup-server --move-back --target-dir=/backup/full --datadir=/var/lib/mysql
```

`--copy-back` leaves the backup where it is; `--move-back` renames the files
across, which is quicker on the same filesystem but consumes the backup. Either
way the datadir is created if missing, and the wrapper won't write into a
non-empty datadir unless you add `--force-non-empty-directories`.

If the source server kept its Aria logs outside the datadir, pass the same
`--aria-log-dir-path=<dir>` you use on the server. The wrapper creates that
directory and moves the restored `aria_log_control` / `aria_log.*` files into
it, so the server finds them on restart:

```sh
mariadb-backup-server --copy-back --target-dir=/backup/full --datadir=/var/lib/mysql \
  --aria-log-dir-path=/var/lib/aria_logs
```

Neither fixes ownership, so finish up with:

```sh
chown -R mysql:mysql /var/lib/mysql
systemctl start mariadb
```

## What it doesn't do

These stop with an error instead of quietly producing an incomplete backup:

- incremental backup/prepare: `--incremental-basedir`, `--incremental-dir`, `--apply-log-only`
- partial backup: `--databases`, `--tables`, `--tables-file`. This needs server-side
`backup_include`/`backup_exclude`, which don't exist yet
- compression and encryption of the output: `--compress`, `--encrypt` (error out)
- `--rollback-xa`: not supported (errors out)

`--stream` is supported with the caveats above (local only, tar only, extract
with a plain `tar -x`);

`--export` is accepted but not implemented: it warns and does a plain recovery
(no per-table `.cfg` files for IMPORT TABLESPACE).

## Environment overrides

The wrapped commands can be overridden via environment variables,
mainly for testing:

- `MARIADB`: the client used to talk to the server (default `mariadb`),
e.g. `MARIADB='mariadb --protocol=tcp'`.

- `MARIADBD`: the server binary the `--prepare` bootstrap runs (by default the
path recorded in `backup-prepare.cnf`, else `mariadbd` on `PATH`). When set it
overrides that resolution. To run it under `rr`, put `rr` in `MARIADBD` and let
`rr`'s own `_RR_TRACE_DIR` choose where the trace goes, e.g.
`_RR_TRACE_DIR=/dev/shm/rr MARIADBD='rr record mariadbd' ...`.

- `TAR`: the tar implementation (default `tar`),
e.g. `TAR=bsdtar` (libarchive-tools).
Used by `--stream` and by `mbstream-server`.

## The two .cnf files

`backup.cnf` is written by the server and tells `--prepare` what parts of redo to replay:

```ini
[server]
# checkpoint=54088
innodb_log_recovery_start=54088     # recovery starts scanning here
innodb_log_recovery_target=56337    # the backup's end LSN; recovery stops here
```

`backup-prepare.cnf` is written by the wrapper and handed to the prepare
bootstrap as its defaults file:

```ini
# mariadbd=/usr/sbin/mariadbd
[mariadbd]
innodb_page_size=16384
innodb_data_file_path=ibdata1:12M:autoextend
innodb_undo_tablespaces=3
innodb_checksum_algorithm=full_crc32
innodb_log_file_size=100663296
```

If the server is encrypted it also records how to load the key-management
plugin again, so an encrypted backup can be prepared without extra input.
For `file_key_management` it captures every plugin variable (the same way
`mariadb-backup` writes them into `backup-my.cnf`) 

```ini
plugin-load-add=file_key_management
innodb_encrypt_log=ON
loose-file-key-management
loose-file_key_management_filename=/etc/mysql/keys.enc
loose-file_key_management_filekey=FILE:/etc/mysql/keyfile.key
loose-file_key_management_encryption_algorithm=aes_cbc
loose-file_key_management_digest=sha1
loose-file_key_management_use_pbkdf2=1
```
