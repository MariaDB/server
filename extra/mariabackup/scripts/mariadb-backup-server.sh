#!/bin/sh
me=${0##*/}
die() {
    echo "$me: $*" >&2
    exit 1
}

# Overridable for testing:
# MARIADB='mariadb --protocol=tcp', TAR=bsdtar,
# MARIADBD='rr record mariadbd'.
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

    --defaults-file=*|--defaults-extra-file=*)
        MARIADB_OPTS="$MARIADB_OPTS $1"
        PREPARE_DEFAULTS="$PREPARE_DEFAULTS --defaults-extra-file=${1#*=}" ;;
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

# Streaming spools nothing, so --target-dir is optional there.
[ -n "$STREAM" ] || [ -n "$TARGET_DIR" ] || die "--target-dir required"

ask() { $MARIADB $MARIADB_OPTS -BN -e "$1" 2>/dev/null; }

# mariadb-backup runs a dedicated log_copying_thread()
# besides the --parallel data-copy threads,
# so --parallel=N is N+1 workers, at least 2, at most 256.
concurrent_clause() {
    _p=$PARALLEL
    while :; do
        case $_p in 0?*) _p=${_p#0} ;; *) break ;; esac
    done
    case $_p in
    ''|*[!0-9]*) _c=2 ;;
    ????*)       _c=256 ;;
    *)           _c=$((_p + 1))
                 [ "$_c" -ge 2 ]   || _c=2
                 [ "$_c" -le 256 ] || _c=256 ;;
    esac
    printf ' %s CONCURRENT' "$_c"
}

# Print the first mariadbd/mysqld found under basedir $1,
# else return 1. CMake substitutes @sbindir@/@bindir@;
# unsubstituted they are not absolute, so the case skips them.
find_mariadbd() {
    for _d in "${1-}/libexec" "${1-}/sbin" "${1-}/bin" '@sbindir@' '@bindir@'
    do
        case $_d in /*) ;; *) continue ;; esac
        for _n in mariadbd mysqld; do
            [ -x "$_d/$_n" ] && { printf '%s\n' "$_d/$_n"; return 0; }
        done
    done
    return 1
}

# Print what --prepare's offline bootstrap needs:
# the mariadbd path, the InnoDB parameters, and
# how to reload the encryption key plugin.
write_prepare_cnf() {
    _mariadbd=
    _pidfile=$(ask "SELECT @@global.pid_file")
    if [ -n "$_pidfile" ] && [ -r "$_pidfile" ]; then
        _pid=$(cat "$_pidfile" 2>/dev/null)
        [ -n "$_pid" ] && _mariadbd=$(readlink -f "/proc/$_pid/exe" 2>/dev/null)
    fi
    [ -n "$_mariadbd" ] && [ -x "$_mariadbd" ] || _mariadbd=
    [ -n "$_mariadbd" ] ||
        _mariadbd=$(find_mariadbd "$(ask "SELECT @@global.basedir")") ||
        _mariadbd=

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
    [ -z "$ROLLBACK_XA" ] || die "--rollback-xa is not supported"
    [ -d "$TARGET_DIR" ]  || die "no such directory: $TARGET_DIR"
    [ -f "$TARGET_DIR/backup.cnf" ] || die "backup.cnf not found in $TARGET_DIR"

    cnf=$TARGET_DIR/backup-prepare.cnf
    [ -f "$cnf" ] || die "$cnf missing - was this backup made by the wrapper?"
    [ -z "$EXPORT" ] || echo "$me: --export not implemented, doing a plain recovery" >&2

    if [ -n "${MARIADBD-}" ]; then
        mariadbd=$MARIADBD
    else
        mariadbd=$(sed -n 's/^# *mariadbd=//p' "$cnf" | tail -n1)
        if [ -z "$mariadbd" ] || [ ! -x "$mariadbd" ]; then
            mariadbd=$(find_mariadbd "") ||
                die "cannot locate the server binary for recovery;\
		     set MARIADBD=/path/to/mariadbd"
        fi
    fi

    # the LSN window recovery must replay
    start=$(grep  '^innodb_log_recovery_start'  "$TARGET_DIR/backup.cnf" | cut -d= -f2 | tr -d ' ')
    target=$(grep '^innodb_log_recovery_target' "$TARGET_DIR/backup.cnf" | cut -d= -f2 | tr -d ' ')

    opts="--datadir=$TARGET_DIR --innodb=FORCE"
    [ -n "$start" ] && opts="$opts --innodb-log-recovery-start=$start"
    [ -n "$target" ] && opts="$opts --innodb-log-recovery-target=$target"
    [ -n "$USE_MEMORY" ] && opts="$opts --innodb-buffer-pool-size=$USE_MEMORY"
    opts="$opts$INNODB_OPTS$MYSQLD_EXTRA"

    # Recovery leaves the archived log (ib_<lsn>.log) behind,
    # which a normal server won't boot from, so build a
    # fresh ib_logfile0 (header plus a checkpoint at the end LSN)
    # to replace it.
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

    # dotfiles included
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

    # backup put the Aria logs at the datadir root;
    # --aria-log-dir-path means the server will look
    # elsewhere on restart.
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
# The server feeds this stream's tar to a helper,
# which writes it into a fifo that we drain to stdout,
# so nothing lands on local disk.
#
# CONCURRENT picks the number of sinks as well as workers,
# and N separate tars cannot be interleaved on one stdout, 
# so --parallel cannot be honoured here.
if [ -n "$STREAM" ]; then
    case $PARALLEL in
        ''|*[!0-9]*|0|1) ;;
        *) echo "$me: ignoring --parallel=$PARALLEL:" \
                "a single stream needs a single writer" >&2 ;;
    esac

    if [ -n "$TARGET_DIR" ]; then
        scratch=$TARGET_DIR
        mkdir -p "$scratch" || die "cannot create scratch dir: $scratch"
        keep_scratch=1
    else
        scratch=$(mktemp -d "${TMPDIR:-/tmp}/mariabackup-stream.XXXXXX") \
            || die "cannot create scratch directory"
        keep_scratch=
    fi

    helper=$scratch/.mariabackup-stream.sh
    fifo=$scratch/.mariabackup-stream.fifo
    drain=

    cleanup_stream() {
        # unlinking does not release a reader still blocked on open(2)
        [ -z "$drain" ] || kill "$drain" 2>/dev/null
        rm -f "$helper" "$fifo" "$scratch/backup-prepare.cnf"
        [ -n "$keep_scratch" ] || rmdir "$scratch" 2>/dev/null
    }

    mkfifo -m 600 "$fifo" || { cleanup_stream; die "cannot create fifo: $fifo"; }

    # the stream index the server appends is ignored: there is
    # only one stream
    cat > "$helper" <<EOF
#!/bin/sh
exec cat > "$fifo"
EOF

    # before the writer: opening a fifo for writing
    # blocks until a reader does
    cat "$fifo" &
    drain=$!

    # "/bin/sh <helper>" needs no execute bit, which /dev/shm
    # may forbid. Client output goes to stderr to keep stdout
    # clean for the tar.
    $MARIADB $MARIADB_OPTS -e "BACKUP SERVER WITH '/bin/sh $helper'" >&2 \
        || { cleanup_stream; die "BACKUP SERVER failed"; }

    # the tar must be complete on stdout before anything
    # is appended to it
    wait "$drain" || { drain=; cleanup_stream; die "streaming to stdout failed"; }
    drain=

    # Append backup-prepare.cnf as one more tar so it extracts
    # alongside backup.cnf. The server writes no end-of-archive
    # marker; this trailing archive supplies the only one,
    # so plain "tar -x" reads the whole stream.
    echo "$me: appending backup-prepare.cnf to the stream" >&2
    write_prepare_cnf > "$scratch/backup-prepare.cnf" \
        || { cleanup_stream; die "could not build backup-prepare.cnf"; }
    $TAR -C "$scratch" -cf - backup-prepare.cnf \
        || { cleanup_stream; die "could not append backup-prepare.cnf to the stream"; }

    cleanup_stream
    echo "$me: streamed the backup plus backup-prepare.cnf to stdout" >&2
    exit 0
fi


# backup (directory)
parent=$(dirname "$TARGET_DIR")
[ -d "$parent" ] || die "parent directory does not exist: $parent"
[ -w "$parent" ] || die "parent directory is not writable: $parent"

sql="BACKUP SERVER TO '$TARGET_DIR'$(concurrent_clause)"
$MARIADB $MARIADB_OPTS -e "$sql" || die "BACKUP SERVER failed"

[ -f "$TARGET_DIR/backup.cnf" ] || exit 0

echo "$me: writing backup-prepare.cnf" >&2
write_prepare_cnf > "$TARGET_DIR/backup-prepare.cnf"
