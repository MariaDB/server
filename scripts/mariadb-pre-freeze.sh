#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 ARZ Haan AG
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
#    contributors may be used to endorse or promote products derived from this
#    software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS “AS IS”
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
# LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
# CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
# SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
# INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
# CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
# ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.

#
# MariaDB Pre-Freeze Script for MariaDB 11.5+ (InnoDB only)
#
# Use this script together with the mariadb-post-thaw.sh script for external
# volume-based backup tools like Veeam.
#
# Purpose:
# - Initiate MariaDB BACKUP STAGE steps to get a consistent filesystem snapshot
#   without shutting down the server or taking read locks.
# - Writes are blocked only for the shortest possible time, but applications
#   can still see SQL timeouts during the lock window.
# - A timeout of 10 minutes (can be overridden) is enforced; if the snapshot
#   is not taken in time, the database files may be inconsistent, and this
#   script will fail.
#
# References:
# - https://mariadb.com/docs/server/reference/sql-statements/administrative-sql-statements/backup-commands/backup-stage
# - https://helpcenter.veeam.com/docs/vbr/userguide/pre_post_scripts.html
#
# Usage:
# - Optionally override FREEZE_AND_THAW_TIMEOUT_SEC (the mariadb client + FIFO holder can run longer across freeze/thaw).
# - Optionally override SCRIPT_TIMEOUT_SEC (the script can run slightly longer).
# - Optionally set DEBUG_LOGS_ENABLED=true for verbose logging.
#
# Timeout implementation:
# - SCRIPT_TIMEOUT_SEC limits this script's own runtime (reads, SQL responses, overall flow).
# - FREEZE_AND_THAW_TIMEOUT_SEC limits how long the mariadb client + FIFO holder stay alive
#   across pre-freeze, snapshot, and post-thaw.
# - The mariadb client is run under timeout; the FIFO holder also enforces the same limit.
# - When the limit is hit, the client is terminated and the run dir may be left behind.
#
# Requirements:
# - mariadb, mkfifo, stdbuf, and timeout must be installed and in PATH.
# - systemd-cat is optional; without it logs go to stdout/stderr only.
# - See setup section for required files and config.
#
# Setup:
# - Create a Linux user "veeam" and add its public SSH key to authorized_keys.
# - Copy this script and mariadb-post-thaw.sh into the user's home and make them executable:
#   chmod 700 /home/veeam/mariadb-*.sh
#
# Create a DB user:
#   CREATE USER 'veeam'@'localhost' IDENTIFIED BY 'secure-password';
#   GRANT RELOAD, PROCESS ON *.* TO 'veeam'@'localhost';
#   FLUSH PRIVILEGES;
#
# Create ~/.my.cnf in the veeam user home directory:
#   [client]
#   user=veeam
#   password=secure-password
#
# Test the script and check logs with:
#   journalctl -t mariadb-stage-backup
#
# Timeout Behavior and Parameters:
# - SCRIPT_TIMEOUT_SEC (default 600s / 10 min): limits this script's runtime.
#   Includes all preflight checks, SQL commands, and syncs. Mainly for safety.
# - FREEZE_AND_THAW_TIMEOUT_SEC (default 1200s / 20 min): limits the lifetime of the
#   persistent mariadb session and FIFO holder across pre-freeze, snapshot, and post-thaw.
#   This is the critical timeout for snapshot tools. If snapshot takes > 20 min, the
#   database will be forcibly unlocked (BACKUP STAGE END will fail).
#   Recommendation: snapshot should complete in 10-15 minutes; set this to 2x expected time.
#
# How It Works (Advanced):
# 1. pre-freeze:
#    a. Creates runtime directory with FIFOs and lock file
#    b. Starts a persistent mariadb session under timeout wrapper (as background process)
#    c. Starts FIFO holder process (keeps FIFOs open, survives pre-freeze)
#    d. Executes BACKUP STAGE START/FLUSH/BLOCK_DDL/BLOCK_COMMIT via FIFO
#    e. Closes its own FDs (but FIFOs stay open via FIFO holder)
#    f. Returns with database in consistent state, ready for snapshot
# 2. Snapshot tool takes filesystem snapshot (should complete in minutes)
# 3. post-thaw:
#    a. Reads state file to verify database state
#    b. Executes BACKUP STAGE END via the still-running mariadb session
#    c. Kills mariadb session and FIFO holder
#    d. Removes runtime directory
#
# FIFO Communication Protocol:
# - FIFOs are created in pre-freeze and survive it (held open by FIFO holder)
# - Both scripts write to FIFO_IN and read from FIFO_OUT
# - Each SQL command is followed by a marker "---MARKER---"
# - Scripts wait for marker in response before considering command complete
# - This allows one persistent connection to handle pre-freeze + post-thaw
#
# Troubleshooting:
#
# 1. Script hangs or times out:
#    - Check DATABASE connectivity: mariadb
#    - Check DB logs: tail -f /var/log/mariadb/mariadb.log
#    - Look for BACKUP STAGE errors in journalctl
#    - Example: journalctl -t mariadb-stage-backup -n 100 | grep -i error
#
# 2. "Cannot connect to mariadb":
#    - Verify ~/.my.cnf exists and is readable (chmod 600)
#    - Test credentials: mariadb -e "SELECT 1;"
#    - Ensure veeam user has RELOAD + PROCESS grants
#    - Check MariaDB socket/port in my.cnf [client] section
#
# 3. "Another backup process is running":
#    - Check if old pre-freeze is still running: ps aux | grep mariadb-pre-freeze
#    - If yes, wait or kill it: kill -9 <pid>
#    - If snapshot tool crashed: manually run post-thaw to clean up (or wait until timeout)
#    - Check lock file: ls -la ~/.mariadb-stage-backup-run-dir/
#
# 4. "Timeout waiting for SQL response" or "Timeout after 20 min":
#    - Snapshot took too long or was never taken
#    - Check if snapshot tool has errors
#    - Increase FREEZE_AND_THAW_TIMEOUT_SEC: env FREEZE_AND_THAW_TIMEOUT_SEC=2400 ./mariadb-pre-freeze.sh
#    - Run post-thaw to release locks: ./mariadb-post-thaw.sh
#    - Database may have stale BACKUP STAGE state; check with: SHOW PROCESSLIST;
#
# 5. "Stale lock found, cleaning runtime dir":
#    - Indicates a crashed pre-freeze from a previous run
#    - Script auto-cleans and proceeds (safe)
#    - Check logs for why it crashed: journalctl -t mariadb-stage-backup --since "2 hours ago"
#
# 6. Database locked, snapshot failed, unclear state:
#    - Check current BACKUP STAGE state: SHOW VARIABLES LIKE 'innodb_backup_stage';
#    - Manually release if needed: BACKUP STAGE END;
#    - Check post-thaw cleanup worked: ls -la ~/.mariadb-stage-backup-run-dir/ (should be empty)
#
# Safety and Consistency:
# - The scripts use PID validation (cmdline + start_time) to prevent killing wrong processes
# - nohup + timeout wrapper ensures mariadb survives script termination
# - FIFO holder survives pre-freeze; post-thaw is responsible for cleanup
# - If post-thaw fails, manual cleanup may be needed: kill mariadb session + rm -rf run dir
# - Always check logs and database state before retrying:
#   journalctl -t mariadb-stage-backup
#   SHOW PROCESSLIST;
#

set -euo pipefail
readonly DEBUG_LOGS_ENABLED="${DEBUG_LOGS_ENABLED:-false}"
readonly SCRIPT_TIMEOUT_SEC="${SCRIPT_TIMEOUT_SEC:-600}"  # 10 minutes default
readonly FREEZE_AND_THAW_TIMEOUT_SEC="${FREEZE_AND_THAW_TIMEOUT_SEC:-1200}"  # 20 minutes default

# Safety check: HOME must be set and absolute
if [[ -z "${HOME:-}" ]] || [[ ! "${HOME}" =~ ^/ ]]; then
    echo "[ERROR] HOME is not set or not absolute. Cannot proceed." >&2
    exit 1
fi

readonly LOG_PREFIX="[pre-freeze]"
readonly SQL_MARKER="---MARKER---"
readonly DEFAULTS_FILE="${HOME}/.my.cnf"

# same as in mariadb-post-thaw.sh!
readonly LOG_TAG="mariadb-stage-backup"
readonly RUN_DIR="${HOME}/.mariadb-stage-backup-run-dir"
readonly MARIADB_PID_FILE="${RUN_DIR}/mariadb.pid"
readonly FIFO_IN="${RUN_DIR}/mariadb.in.fifo"
readonly FIFO_OUT="${RUN_DIR}/mariadb.out.fifo"
readonly STATE_FILE="${RUN_DIR}/mariadb-stage-backup.state"

readonly LOCK_FILE="${RUN_DIR}/mariadb-stage-backup.lock"
readonly FIFO_HOLDER_PID_FILE="${RUN_DIR}/fifo_holder.pid"

# remember start time of this script.
SCRIPT_START_EPOCH="$(date +%s)"
readonly SCRIPT_START_EPOCH

HAS_SYSTEMD_CAT="false"
if command -v systemd-cat &>/dev/null; then
    HAS_SYSTEMD_CAT="true"
fi
readonly HAS_SYSTEMD_CAT


# --- Logs to journald via systemd-cat and conditionally to stdout/stderr based on log level. ---
# Usage: log <prio 0..7> <message...>
# Priority levels: 0=emergency, 1=alert, 2=critical, 3=error, 4=warning, 5=notice, 6=info, 7=debug
function log() {
    local prio="${1:-6}"   # default: info (6)
    shift || true

    # best effort failsafe for wrong prio level
    if [[ ! "$prio" =~ ^[0-7]$ ]]; then
        prio=6
    fi

    local line="${LOG_PREFIX} $*"

    # journald (if available)
    if [[ "${HAS_SYSTEMD_CAT}" == "true" ]]; then
        printf '%s\n' "$line" | systemd-cat -t "${LOG_TAG}" -p "${prio}"
    fi

    # stdout/stderr according to severity
    if (( prio <= 3 )); then
        printf '%s\n' "$line" >&2
    else
        printf '%s\n' "$line"
    fi
}
function log_debug() {
    if [[ "${DEBUG_LOGS_ENABLED}" == "true" ]]; then
        log 7 "[DEBUG] $*"
    fi
}
function log_info() { log 6 "[INFO] $*"; }
function log_warn() { log 4 "[WARN] $*"; }
function log_error() { log 3 "[ERROR] $*"; }

# --- Special logger for background jobs. They should not log to stderr/stdout. ----
# Asynchronous logging to stdout/stderr is annoying and can break automations.
function log_system_cat_only() {
    local prio="${1:-6}"   # default: info (6)
    shift || true

    # best effort failsafe for wrong prio level
    if [[ ! "$prio" =~ ^[0-7]$ ]]; then
        prio=6
    fi

    local line="${LOG_PREFIX} $*"

    # journald (if available)
    if [[ "${HAS_SYSTEMD_CAT}" == "true" ]]; then
        printf '%s\n' "$line" | systemd-cat -t "${LOG_TAG}" -p "${prio}"
    fi
}

# helper function: remaining time in seconds until script timeout
# A minimum of 1 sec will be returned, so that timed reads are still possible.
function remaining_seconds() {
    local now elapsed remaining
    now="$(date +%s)"
    elapsed=$(( now - SCRIPT_START_EPOCH ))
    remaining=$(( SCRIPT_TIMEOUT_SEC - elapsed ))

    # Minimum 1 second so reads do not time out immediately.
    if (( remaining < 1 )); then
        remaining=1
    fi
    echo "${remaining}"
}

function timeout_reached() {
    local now
    now="$(date +%s)"
    (( now - SCRIPT_START_EPOCH >= SCRIPT_TIMEOUT_SEC ))
}

# --- Preflight checks ---
if [[ ! -f "${DEFAULTS_FILE}" ]]; then
    log_error "mariadb defaults file not found: ${DEFAULTS_FILE}"
    exit 1
fi
if ! command -v mariadb &>/dev/null; then
    log_error "mariadb client not found in PATH."
    exit 1
fi
if ! command -v mkfifo &>/dev/null; then
    log_error "mkfifo not found in PATH."
    exit 1
fi
if ! command -v stdbuf &>/dev/null; then
    log_error "stdbuf not found in PATH."
    exit 1
fi
if ! command -v timeout &>/dev/null; then
    log_error "timeout not found in PATH."
    exit 1
fi

# --- Test mariadb connectivity ---
if ! mariadb --defaults-file="${DEFAULTS_FILE}" -e "SELECT 1;" &>/dev/null; then
    log_error "Cannot connect to mariadb – check credentials in ${DEFAULTS_FILE}"
    exit 1
fi

# --- No usage of unsupported database engines? ---
# BACKUP STAGE only works with InnoDB. Warn if other engines are found.
# This includes MyISAM, Memory, CSV, Archive, etc.
log_info "Checking for unsupported database engines..."

# Get all non-InnoDB tables (excluding temporary tables and system tables)
unsupported_tables=$(mariadb --defaults-file="${DEFAULTS_FILE}" --skip-column-names -e "
    SELECT CONCAT(table_schema, '.', table_name, ' (', engine, ')')
    FROM information_schema.tables
    WHERE table_schema NOT IN ('information_schema', 'mysql', 'performance_schema', 'sys')
    AND engine IS NOT NULL
    AND upper(engine) != 'INNODB'
    AND table_type = 'BASE TABLE'
    ORDER BY table_schema, table_name;
" 2>/dev/null)

if [[ -n "${unsupported_tables}" ]]; then
    log_error "BACKUP STAGE requires InnoDB. Found tables with unsupported engines:"
    while IFS= read -r table; do
        log_error "  - ${table}"
    done <<< "${unsupported_tables}"
    log_error "Please migrate these tables to InnoDB before proceeding."
    log_error "Example: ALTER TABLE schema.table ENGINE=InnoDB;"
    exit 1
fi

log_info "Database engine check passed - all tables use InnoDB"

# helper function: validate PID against reuse
# PID file format: PID:CMDLINE:START_TIME
# Returns 0 if PID is valid and matches, 1 otherwise
function validate_pid() {
    local pid_file="$1"
    local expected_cmdline_pattern="$2"  # regex pattern to match in cmdline

    if [[ ! -f "${pid_file}" ]]; then
        return 1
    fi

    local pid_info
    pid_info="$(cat "${pid_file}" 2>/dev/null || true)"
    if [[ -z "${pid_info}" ]]; then
        return 1
    fi

    local pid cmdline start_time
    local original_ifs="${IFS}"
    IFS=':' read -r pid cmdline start_time <<< "${pid_info}"
    IFS="${original_ifs}"

    # Check if PID is still running
    if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
        return 1
    fi

    # Check cmdline matches (protect against PID reuse)
    if [[ -n "${expected_cmdline_pattern}" ]]; then
        local current_cmdline
        current_cmdline="$(cat /proc/"${pid}"/cmdline 2>/dev/null | tr '\0' ' ' || true)"
        if [[ ! "${current_cmdline}" =~ ${expected_cmdline_pattern} ]]; then
            log_warn "PID ${pid} cmdline mismatch. Expected pattern: ${expected_cmdline_pattern}, got: ${current_cmdline}"
            return 1
        fi
    fi

    # Check start time matches (additional protection against PID reuse)
    if [[ -n "${start_time}" ]] && [[ -f "/proc/${pid}/stat" ]]; then
        local current_start_time
        current_start_time="$(awk '{print $22}' /proc/"${pid}"/stat 2>/dev/null || true)"
        if [[ -n "${current_start_time}" ]] && [[ "${current_start_time}" != "${start_time}" ]]; then
            log_warn "PID ${pid} start time mismatch. Expected: ${start_time}, got: ${current_start_time}"
            return 1
        fi
    fi

    return 0
}

# helper function: save PID with metadata to protect against PID reuse
function save_pid_with_metadata() {
    local pid="$1"
    local pid_file="$2"

    if [[ -z "${pid}" ]] || ! kill -0 "${pid}" 2>/dev/null; then
        log_error "Cannot save invalid PID: ${pid}"
        return 1
    fi

    local cmdline start_time
    cmdline="$(cat /proc/"${pid}"/cmdline 2>/dev/null | tr '\0' ' ' || echo "unknown")"
    start_time="$(awk '{print $22}' /proc/"${pid}"/stat 2>/dev/null || echo "0")"

    printf '%s:%s:%s\n' "${pid}" "${cmdline}" "${start_time}" > "${pid_file}"
}

# helper function: extract PID from PID file (first field)
function get_pid_from_file() {
    local pid_file="$1"
    if [[ ! -f "${pid_file}" ]]; then
        return 1
    fi
    local pid_info
    pid_info="$(cat "${pid_file}" 2>/dev/null || true)"
    echo "${pid_info}" | cut -d: -f1
}

# helper function: stop and kill a process by pid file with PID reuse protection
function stop_and_kill_by_pid_file() {
    local pid_file="$1"
    local label="${2:-process}"
    local expected_cmdline_pattern="${3:-}"  # optional cmdline pattern for validation

    if [[ ! -f "${pid_file}" ]]; then
        log_debug "Pid file not found (${pid_file}). Cannot stop or kill ${label}."
        return 0
    fi

    local pid
    pid="$(get_pid_from_file "${pid_file}" || true)"

    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        # Validate against PID reuse if pattern provided
        if [[ -n "${expected_cmdline_pattern}" ]] && ! validate_pid "${pid_file}" "${expected_cmdline_pattern}"; then
            log_warn "PID validation failed for ${label} (${pid}) - likely PID reuse, skipping kill"
            rm -f "${pid_file}"
            return 0
        fi

        log_debug "Stopping ${label} (${pid})..."
        kill "${pid}" 2>/dev/null || true
        local i
        # shellcheck disable=SC2034
        for i in $(seq 1 3); do
            kill -0 "${pid}" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "${pid}" 2>/dev/null; then
            log_warn "Force-killing ${label} (${pid})"
            kill -9 "${pid}" 2>/dev/null || true
        fi
        log_info "Stopped ${label} (${pid})."
    fi
    rm -f "${pid_file}"
}

# --- Create cleanup trap ---
# shellcheck disable=SC2317
function cleanup_on_abort_or_error() {
    # All actions are best effort.
    log_error "Pre-freeze failed – attempting cleanup"

    # Close file descriptors
    exec 3>&- 2>/dev/null || true
    exec 4<&- 2>/dev/null || true

    # Close FIFO_HOLDER background process; that should stop mariadb also.
    stop_and_kill_by_pid_file "${FIFO_HOLDER_PID_FILE}" "fifo_holder" "bash"

    # Failsafe stop of mariadb; that should release the locks.
    stop_and_kill_by_pid_file "${MARIADB_PID_FILE}" "mariadb_client" "timeout.*mariadb"

    rm -rf "${RUN_DIR}" || true
    exit 1
}
trap cleanup_on_abort_or_error ERR INT TERM


# --- Prepare runtime dir and lock file ---
if [[ -f "${LOCK_FILE}" ]]; then
    # Check if another pre-freeze instance is running using PID validation
    if validate_pid "${LOCK_FILE}" "mariadb-pre-freeze"; then
        old_pid="$(get_pid_from_file "${LOCK_FILE}" || true)"
        log_error "Aborting because another backup process is running (${old_pid})"
        exit 1
    fi
    log_warn "Stale lock found, cleaning runtime dir"
    stop_and_kill_by_pid_file "${FIFO_HOLDER_PID_FILE}" "fifo_holder" "bash"
    stop_and_kill_by_pid_file "${MARIADB_PID_FILE}" "mariadb_client" "timeout.*mariadb"
    rm -rf "${RUN_DIR:?}/"*
fi
mkdir -p "${RUN_DIR}"
chmod 700 "${RUN_DIR}"
save_pid_with_metadata "$$" "${LOCK_FILE}"
log_debug "Created lock file with PID metadata: ${LOCK_FILE}"

# --- Create FIFOs for remote control of the mariadb background job ---
rm -f "${FIFO_IN}" "${FIFO_OUT}"
(
    umask 077
    mkfifo "${FIFO_IN}"
    mkfifo "${FIFO_OUT}"
)
log_debug "Created FIFO_IN: ${FIFO_IN}"
log_debug "Created FIFO_OUT: ${FIFO_OUT}"


# --- Start mariadb as background process ---
# nohup ::= keep running if this script exits; output goes to FIFO/redirects below.
# stdbuf -oL ::= line-buffered output; required for FIFO line-by-line processing.
# timeout ::= hard limit to avoid an orphaned client after pre-freeze.
# --skip-reconnect ::= reconnects will lose session and locks.
# Note: We store the PID of the timeout wrapper. Signals will be forwarded to mariadb.
# When killing this PID, timeout will propagate the signal to the mariadb client.
log_info "Starting persistent mariadb session with FIFO_IN ${FIFO_IN} and FIFO_OUT ${FIFO_OUT}..."
nohup stdbuf -oL \
    timeout --preserve-status --signal=KILL "${FREEZE_AND_THAW_TIMEOUT_SEC}" \
    mariadb --defaults-file="${DEFAULTS_FILE}" --skip-reconnect --skip-column-names --silent \
    < "${FIFO_IN}" > "${FIFO_OUT}" 2>&1 &

readonly MARIADB_PID=$!
save_pid_with_metadata "${MARIADB_PID}" "${MARIADB_PID_FILE}"
sleep 1    # Failsafe: some time for the mariadb process to start and connect to the FIFOs.
if [[ -z "${MARIADB_PID}" ]] || ! kill -0 "${MARIADB_PID}" 2>/dev/null; then
    log_error "Failed to start mariadb session (no valid pid)."
    exit 1
fi
log_info "Started persistent mariadb session with pid ${MARIADB_PID} (timeout wrapper)."


# --- Create background process to keep the FIFO open ---
# This process survives after pre-freeze; post-thaw relies on the FIFOs staying open.
# The lock file removal by post-thaw is the normal stop signal.
# No logging to stdout or stderr in background jobs. This can break automations.
(
    exec 3> "${FIFO_IN}"
    exec 4< "${FIFO_OUT}"

    # Apply timeout
    declare -i start_ts
    start_ts=$(date +%s)

    # Wait for a timeout or the post-thaw script to end us to avoid an orphaned process after pre-freeze.
    while [[ -f "${LOCK_FILE}" ]]; do
        declare -i current_ts
        current_ts=$(date +%s)
        if (( current_ts - start_ts >= FREEZE_AND_THAW_TIMEOUT_SEC )); then
            log_system_cat_only 3 "[ERROR] Timeout after ${FREEZE_AND_THAW_TIMEOUT_SEC}s waiting for lock file ${LOCK_FILE}."
            exit 124
        fi
        sleep 1
    done

    log_system_cat_only 6 "[INFO] FIFO_HOLDER stopped by deleted lock file ${LOCK_FILE}."
) &
readonly FIFO_HOLDER_PID=$!
save_pid_with_metadata "${FIFO_HOLDER_PID}" "${FIFO_HOLDER_PID_FILE}"
log_debug "Created background process fifo_holder with pid ${FIFO_HOLDER_PID}"

# --- Create persistent file descriptors for this script ---
# FD 3 = Writing to FIFO_IN (additionally to the holder)
# FD 4 = Reading from FIFO_OUT (additionally to the holder)
exec 3> "${FIFO_IN}"
exec 4< "${FIFO_OUT}"

log_debug "Created persistent FD for FIFO_IN and FIFO_OUT"

function send_sql() {
    local sql="$1"

    if ! kill -0 "${MARIADB_PID}" 2>/dev/null; then
        log_error "mariadb client is not running anymore (${MARIADB_PID})."
        return 1
    fi

    # Validate PID hasn't been reused before sending SQL
    if ! validate_pid "${MARIADB_PID_FILE}" "timeout.*mariadb"; then
        log_error "mariadb client PID validation failed (likely PID reuse)."
        return 1
    fi

    # send SQL + Marker via FD 3
    log_debug "mariadb> ${sql}\\nSELECT \"${SQL_MARKER}\" AS marker;\\n"
    printf '%s\nSELECT "%s" AS marker;\n' "${sql}" "${SQL_MARKER}" >&3

    # read response until marker appears (or timeout)
    local line
    local response=""
    local found_marker=0
    local read_timeout

    # Read response from FD 4 until marker is found or timeout
    while true; do
        read_timeout="$(remaining_seconds)"

        if ! read -r -t "${read_timeout}" line <&4; then
            if timeout_reached; then
                log_error "Timeout waiting for SQL response: ${sql}"
                return 1
            fi
            # EOF or timeout
            break
        fi

        log_debug "mariadb> ${line}"

        # Check for SQL errors (ERROR keyword from MariaDB)
        if [[ "$line" =~ ^ERROR ]]; then
            log_error "SQL Error in response: ${line}"
            return 1
        fi

        # Check for marker to indicate command completion
        if [[ "$line" == "${SQL_MARKER}" ]]; then
            found_marker=1
            break
        fi

        response+="${line}"$'\n'
    done

    # Validate successful completion: marker found AND no SQL error
    if (( found_marker == 1 )); then
        log_info "SQL command succeeded: ${sql}"
        return 0
    elif timeout_reached; then
        log_error "Timeout waiting for SQL response marker: ${sql}"
        return 1
    else
        log_error "SQL command did not complete properly: ${sql}, response ${response}"
        return 1
    fi
}

log_info "Starting mariadb backup stages..."

# Validate STATE_FILE is writable before proceeding
if ! touch "${STATE_FILE}" 2>/dev/null; then
    log_error "STATE_FILE (${STATE_FILE}) is not writable. Check permissions."
    exit 1
fi

send_sql "BACKUP STAGE START;"
echo "START" > "${STATE_FILE}"
log_debug "STATE_FILE set to START"

send_sql "BACKUP STAGE FLUSH;"
echo "FLUSH" > "${STATE_FILE}"
log_debug "STATE_FILE set to FLUSH"

send_sql "BACKUP STAGE BLOCK_DDL;"
echo "BLOCK_DDL" > "${STATE_FILE}"

send_sql "BACKUP STAGE BLOCK_COMMIT;"
echo "BLOCK_COMMIT" > "${STATE_FILE}"

# --- Close FDs, but FIFO_HOLDER background process still keeps the FIFOs open. ---
exec 3>&-
exec 4<&-

log_info "BACKUP STAGE BLOCK_COMMIT sent - database should now be in consistent state."

sync
log_info "Filesystem write caches synchronized. Filesystem should now be in consistent state also."

exit 0
