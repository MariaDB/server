#!/bin/sh
me=${0##*/}
die() {
    echo "$me: $*" >&2
    exit 1
}

# The wrapped commands can be overridden for testing, e.g.
#   MARIADB='mariadb --protocol=tcp', TAR=bsdtar.

# To run the prepare bootstrap under rr, include it in MARIADBD
# (rr's own _RR_TRACE_DIR controls where the trace is written).
# e.g.  _RR_TRACE_DIR=/dev/shm/rr MARIADBD='rr record mariadbd' ...
: "${MARIADB:=mariadb}"
: "${TAR:=tar}"

MODE=
TARGET_DIR=
DATADIR=
PARALLEL=
USE_MEMORY=
FORCE_NON_EMPTY=
EXPORT=
ROLLBACK_XA=
MARIADB_OPTS=
INNODB_OPTS=
MYSQLD_EXTRA=
PREPARE_DEFAULTS=
ARIA_LOG_DIR=
STREAM=

while [ $# -gt 0 ]; do
    case $1 in
    --backup)              MODE=backup ;;
    --prepare|--apply-log) MODE=prepare ;;
    --copy-back)           MODE=copy-back ;;
    --move-back)           MODE=move-back ;;

    --target-dir=*) TARGET_DIR=${1#*=} ;;
    --datadir=*)    DATADIR=${1#*=} ;;
    --aria-log-dir-path=*) ARIA_LOG_DIR=${1#*=} ;;
    --use-memory=*) USE_MEMORY=${1#*=} ;;
    --parallel=*)   PARALLEL=${1#*=} ;;

    --export)                      EXPORT=1 ;;
    --rollback-xa)                 ROLLBACK_XA=1 ;;
    --force-non-empty-directories) FORCE_NON_EMPTY=1 ;;

    --innodb|--innodb=*|--innodb-*|--innodb_*|--skip-innodb-*|--skip_innodb_*)
	INNODB_OPTS="$INNODB_OPTS $1" ;;
    --tmpdir=*) MYSQLD_EXTRA="$MYSQLD_EXTRA $1" ;;
    -t)  MYSQLD_EXTRA="$MYSQLD_EXTRA --tmpdir=$2"; shift ;;
    -t*) MYSQLD_EXTRA="$MYSQLD_EXTRA --tmpdir=${1#-t}" ;;
    --incremental-basedir=*|--incremental-dir=*)
        die "incremental backup/prepare is not supported" ;;
    --apply-log-only)
        die "--apply-log-only is not supported" ;;
    --databases=*|--databases-exclude=*|--tables=*|--tables-exclude=*|--tables-file=*)
        die "partial backup needs server-side backup_include/backup_exclude, which doesn't exist yet" ;;
    # BACKUP SERVER only ever produces tar
    --stream|--stream=*)                          STREAM=1 ;;
    --compress|--compress=*|--compress-threads=*) die "--compress is not supported" ;;
    --encrypt|--encrypt=*)                        die "--encrypt is not supported" ;;
    --innobackupex) die "innobackupex mode is not supported" ;;

    # Defaults files feed the backup client
    --defaults-file=*|--defaults-extra-file=*)
        MARIADB_OPTS="$MARIADB_OPTS $1"
        PREPARE_DEFAULTS="$PREPARE_DEFAULTS --defaults-extra-file=${1#*=}" ;;
    # Encryption options are forwarded to the prepare bootstrap.
    --file-key-management*|--plugin-load-add=*|--loose-file-key-management*|\
    --aria-encrypt-tables*|--encrypt-tmp-disk-tables*)
        PREPARE_DEFAULTS="$PREPARE_DEFAULTS $1" ;;

    --user=*|--password=*|--host=*|--port=*|--socket=*|\
    --defaults-group=*|\
    --secure-auth|--skip-secure-auth|--ssl|--ssl-verify-server-cert|\
    --ssl-ca=*|--ssl-capath=*|--ssl-cert=*|--ssl-cipher=*|\
    --ssl-crl=*|--ssl-crlpath=*|--ssl-key=*|--tls-version=*)
        MARIADB_OPTS="$MARIADB_OPTS $1" ;;
    -p)
        if [ -n "${2-}" ] && case $2 in -*) false ;; *) true ;; esac; then
            MARIADB_OPTS="$MARIADB_OPTS -p$2"; shift
        else
            MARIADB_OPTS="$MARIADB_OPTS -p"
        fi ;;
    -u|-P|-S) MARIADB_OPTS="$MARIADB_OPTS $1 $2"; shift ;;
    -H)       MARIADB_OPTS="$MARIADB_OPTS --host=$2"; shift ;;
    -p*|-u*|-P*|-S*) MARIADB_OPTS="$MARIADB_OPTS $1" ;;
    -H*)      MARIADB_OPTS="$MARIADB_OPTS --host=${1#-H}" ;;

    -h)  DATADIR=$2; shift ;;
    -h*) DATADIR=${1#-h} ;;

    # Everything else is accepted and ignored:
    *) ;;
    esac
    shift
done

# In stream mode --target-dir is optional:
# it is only a scratch directory for the per-stream tar parts
# (a mktemp dir is used when it is omitted).
[ -n "$STREAM" ] || [ -n "$TARGET_DIR" ] || die "--target-dir required"

# Run the client with the connection options we collected.
ask() { $MARIADB $MARIADB_OPTS -BN -e "$1" 2>/dev/null; }

# Print the backup-prepare.cnf contents to stdout.
# It captures everything --prepare's offline bootstrap needs:
# where mariadbd lives, the InnoDB parameters, and how to
# reload the encryption key plugin. Used both for a
# directory backup (written into the target dir)
# and a streamed backup (appended to the stream so it lands
# in the extracted directory).

write_prepare_cnf() {
    _mariadbd=
    _pidfile=$(ask "SELECT @@global.pid_file")
    if [ -n "$_pidfile" ] && [ -r "$_pidfile" ]; then
        _pid=$(cat "$_pidfile" 2>/dev/null)
        [ -n "$_pid" ] && _mariadbd=$(readlink -f "/proc/$_pid/exe" 2>/dev/null)
    fi
    if [ -z "$_mariadbd" ]; then
        _basedir=$(ask "SELECT @@global.basedir")
        for _c in "$_basedir/sbin/mariadbd" "$_basedir/bin/mariadbd" \
                  "$_basedir/sbin/mysqld"   "$_basedir/bin/mysqld"; do
            [ -x "$_c" ] && { _mariadbd=$_c; break; }
        done
    fi

    _page_size=$(ask      "SELECT @@global.innodb_page_size")
    _data_file_path=$(ask "SELECT @@global.innodb_data_file_path")
    _undo_ts=$(ask        "SELECT @@global.innodb_undo_tablespaces")
    _checksum=$(ask       "SELECT @@global.innodb_checksum_algorithm")
    _log_file_size=$(ask  "SELECT @@global.innodb_log_file_size")

    _enc=$(ask "SELECT LOWER(plugin_name) FROM information_schema.PLUGINS
               WHERE plugin_type='ENCRYPTION' AND plugin_status='ACTIVE' LIMIT 1")

    [ -n "$_mariadbd" ] && echo "# mariadbd=$_mariadbd"
    echo "[mariadbd]"
    [ -n "$_page_size" ]      && echo "innodb_page_size=$_page_size"
    [ -n "$_data_file_path" ] && echo "innodb_data_file_path=$_data_file_path"
    [ -n "$_undo_ts" ]        && echo "innodb_undo_tablespaces=$_undo_ts"
    [ -n "$_checksum" ]       && echo "innodb_checksum_algorithm=$_checksum"
    [ -n "$_log_file_size" ]  && echo "innodb_log_file_size=$_log_file_size"

    if [ -n "$_enc" ]; then
        echo "plugin-load-add=$_enc"
	_plugin_dir=$(ask "SELECT @@global.plugin_dir")
	[ -n "$_plugin_dir" ] && echo "plugin-dir=$_plugin_dir"
        case $(ask "SELECT @@global.innodb_encrypt_log") in
            1|ON) echo "innodb_encrypt_log=ON" ;;
        esac
        if [ "$_enc" = file_key_management ]; then
            echo "loose-file-key-management"
            ask "SHOW VARIABLES LIKE 'file_key_management%'" |
            while read -r _fkm_name _fkm_value; do
               [ -n "$_fkm_name" ] && echo "loose-$_fkm_name=$_fkm_value"
            done
        fi
    fi
}


# prepare
if [ "$MODE" = prepare ]; then
    [ -z "$ROLLBACK_XA" ]          || die "--rollback-xa is not supported"
    [ -d "$TARGET_DIR" ]           || die "no such directory: $TARGET_DIR"
    [ -f "$TARGET_DIR/backup.cnf" ] || die "backup.cnf not found in $TARGET_DIR"

    cnf=$TARGET_DIR/backup-prepare.cnf
    [ -f "$cnf" ] || die "$cnf missing - was this backup made by the wrapper?"
    [ -z "$EXPORT" ] || echo "$me: --export not implemented, doing a plain recovery" >&2

    # Prefer the binary recorded at backup time
    # else, fall back to mariadbd on PATH only if the
    # recorded one is missing.
    # MARIADBD overrides the recorded/PATH binary
    if [ -n "${MARIADBD-}" ]; then
        mariadbd=$MARIADBD
    else
        mariadbd=$(sed -n 's/^# *mariadbd=//p' "$cnf" | tail -n1)
        [ -n "$mariadbd" ] && [ -x "$mariadbd" ] || mariadbd=mariadbd
    fi

    # backup.cnf tells us the LSN window recovery should replay.
    start=$(grep  '^innodb_log_recovery_start'  "$TARGET_DIR/backup.cnf" | cut -d= -f2 | tr -d ' ')
    target=$(grep '^innodb_log_recovery_target' "$TARGET_DIR/backup.cnf" | cut -d= -f2 | tr -d ' ')

    opts="--datadir=$TARGET_DIR --innodb=FORCE"
    [ -n "$start" ]      && opts="$opts --innodb-log-recovery-start=$start"
    [ -n "$target" ]     && opts="$opts --innodb-log-recovery-target=$target"
    [ -n "$USE_MEMORY" ] && opts="$opts --innodb-buffer-pool-size=$USE_MEMORY"
    opts="$opts$INNODB_OPTS$MYSQLD_EXTRA"

    # Recovery stops at the backup's end LSN but leaves the
    # archived log (ib_<lsn>.log) behind, which a normal server
    # won't boot from. After recovery we build a fresh ib_logfile0
    # (header + a checkpoint at the end LSN) and put it in place
    # of the archived log.
    input=/dev/null
    newlog=$TARGET_DIR/ib_logfile0.new
    if [ -n "$target" ]; then
        lsn=$(printf '%016x' "$target")
        rm -f "$newlog"
        input=$(mktemp)
        cat > "$input" <<EOF
set @lsn=x'$lsn';
set @header=concat('Phys',x'00000000',@lsn,repeat(x'00',492));
set @header=concat(@header,unhex(lpad(hex(crc32c(@header)),8,'0')),repeat(x'00',3584));
set @checkpoint=concat(@lsn,@lsn,repeat(x'00',44));
set @checkpoint=concat(@checkpoint,unhex(lpad(hex(crc32c(@checkpoint)),8,'0')),repeat(x'00',8128));
set @payload=concat(x'fa0000',@lsn);
set @payload=concat(@payload,x'01',unhex(lpad(hex(crc32c(@payload)),8,'0')));
select concat(@header,@checkpoint,@payload) into dumpfile '$newlog';
EOF
    fi

    $mariadbd --defaults-file="$cnf" $PREPARE_DEFAULTS $opts --bootstrap < "$input"
    rc=$?
    [ "$input" = /dev/null ] || rm -f "$input"
    [ $rc -eq 0 ] || die "recovery failed (mariadbd exited $rc)"

    if [ -n "$target" ]; then
        [ -f "$newlog" ] || die "could not build ib_logfile0 for LSN $target"
        rm -f "$TARGET_DIR"/ib_*.log
        mv "$newlog" "$TARGET_DIR/ib_logfile0"
    fi

    echo "Prepare completed: $TARGET_DIR" >&2
    exit 0
fi


# copy-back / move-back
if [ "$MODE" = copy-back ] || [ "$MODE" = move-back ]; then
    [ -d "$TARGET_DIR" ]            || die "no such directory: $TARGET_DIR"
    [ -f "$TARGET_DIR/backup.cnf" ] || die "backup.cnf not found in $TARGET_DIR"
    [ -n "$DATADIR" ]              || die "--datadir required for --$MODE"

    # mariadb-backup creates the datadir if it's missing; do the same.
    [ -d "$DATADIR" ] || mkdir -p "$DATADIR" || die "cannot create datadir: $DATADIR"

    # Refuse a non-empty datadir (dotfiles included) unless told otherwise.
    if [ -z "$FORCE_NON_EMPTY" ]; then
        for f in "$DATADIR"/* "$DATADIR"/.[!.]* "$DATADIR"/..?*; do
            { [ -e "$f" ] || [ -L "$f" ]; } &&
                die "datadir not empty: $DATADIR (pass --force-non-empty-directories)"
        done
    fi

    if [ "$MODE" = copy-back ]; then
        echo "$me: copying $TARGET_DIR -> $DATADIR" >&2
        cp -R "$TARGET_DIR"/. "$DATADIR"/ || die "copy-back failed"
    else
        echo "$me: moving $TARGET_DIR -> $DATADIR" >&2
        for f in "$TARGET_DIR"/* "$TARGET_DIR"/.[!.]* "$TARGET_DIR"/..?*; do
            { [ -e "$f" ] || [ -L "$f" ]; } || continue
            mv "$f" "$DATADIR"/ || die "move-back failed"
        done
    fi

    # Aria logs live in --aria-log-dir-path when set; 
    # backup placed them at the datadir root,
    # so relocate them there and create the directory the server
    # expects on restart.
    if [ -n "$ARIA_LOG_DIR" ] && [ "$ARIA_LOG_DIR" != "$DATADIR" ]; then
        mkdir -p "$ARIA_LOG_DIR" || die "cannot create aria-log-dir-path: $ARIA_LOG_DIR"
        for f in "$DATADIR"/aria_log_control "$DATADIR"/aria_log.*; do
            [ -e "$f" ] && { mv "$f" "$ARIA_LOG_DIR"/ || die "aria log relocate failed"; }
        done
    fi

    echo "Restore completed: $DATADIR" >&2
    echo "$me: now run: chown -R mysql:mysql $DATADIR && start the server" >&2
    exit 0
fi


# backup (streaming)
# "BACKUP SERVER WITH [N CONCURRENT] '<command>'" runs <command>
# inside the server feeding that stream's tar to the command's stdin.
if [ -n "$STREAM" ]; then
    if [ -n "$TARGET_DIR" ]; then
        scratch=$TARGET_DIR
        mkdir -p "$scratch" || die "cannot create scratch dir: $scratch"
        keep_scratch=1
    else
        scratch=$(mktemp -d "${TMPDIR:-/tmp}/mariabackup-stream.XXXXXX") \
            || die "cannot create scratch directory"
        keep_scratch=
    fi

    cleanup_stream() {
        rm -f "$scratch"/.mariabackup-stream.sh "$scratch"/*.tar \
              "$scratch"/backup-prepare.cnf
        [ -n "$keep_scratch" ] || rmdir "$scratch" 2>/dev/null
    }

    # The server appends the stream index as $1 and writes that stream's tar
    # to our stdin; we drop each one into the scratch dir under <index>.tar.
    helper=$scratch/.mariabackup-stream.sh
    cat > "$helper" <<EOF
#!/bin/sh
exec cat > "$scratch/\$1.tar"
EOF

    # Invoke via "/bin/sh <helper>" so the script needs no execute bit
    # (scratch may sit on a noexec filesystem such as /dev/shm).
    sql="BACKUP SERVER WITH"
    case $PARALLEL in           # --parallel=N -> "N CONCURRENT" (1 is default)
        ''|*[!0-9]*) ;;
        *) [ "$PARALLEL" -gt 1 ] && sql="$sql $PARALLEL CONCURRENT" ;;
    esac
    sql="$sql '/bin/sh $helper'"

    # Keep stdout clean for the tar: send any client output to stderr.
    $MARIADB $MARIADB_OPTS -e "$sql" >&2 \
        || { cleanup_stream; die "BACKUP SERVER failed"; }

    # Concatenate the per-stream tars to stdout in index order,
    # then append backup-prepare.cnf as one more tar archive
    # so it lands in the extracted directory alongside the
    # server's backup.cnf. The per-stream tars carry no
    # end-of-archive marker; only this trailing
    # backup-prepare.cnf adds the single end marker,
    # so the whole stream extracts with a plain "tar -x".
    n=1
    while [ -f "$scratch/$n.tar" ]; do
        cat "$scratch/$n.tar" || { cleanup_stream; die "streaming to stdout failed"; }
        n=$((n + 1))
    done

    echo "$me: appending backup-prepare.cnf to the stream" >&2
    write_prepare_cnf > "$scratch/backup-prepare.cnf" \
        || { cleanup_stream; die "could not build backup-prepare.cnf"; }
    $TAR -C "$scratch" -cf - backup-prepare.cnf \
        || { cleanup_stream; die "could not append backup-prepare.cnf to the stream"; }

    cleanup_stream
    echo "$me: streamed $((n - 1)) tar stream(s) plus backup-prepare.cnf to stdout" >&2
    exit 0
fi


# --- backup (directory) -----------------------------------------------------
parent=$(dirname "$TARGET_DIR")
[ -d "$parent" ] || die "parent directory does not exist: $parent"
[ -w "$parent" ] || die "parent directory is not writable: $parent"

sql="BACKUP SERVER TO '$TARGET_DIR'"
case $PARALLEL in           # --parallel=N -> "N CONCURRENT" (1 is the default)
    ''|*[!0-9]*) ;;
    *) [ "$PARALLEL" -gt 1 ] && sql="$sql $PARALLEL CONCURRENT" ;;
esac
$MARIADB $MARIADB_OPTS -e "$sql" || die "BACKUP SERVER failed"

# Create backup-prepare.cnf with everything --prepare's offline
# bootstrap needs: where mariadbd is, the InnoDB parameters,
# and how to load the key plugin again.
[ -f "$TARGET_DIR/backup.cnf" ] || exit 0

echo "$me: writing backup-prepare.cnf" >&2
write_prepare_cnf > "$TARGET_DIR/backup-prepare.cnf"
