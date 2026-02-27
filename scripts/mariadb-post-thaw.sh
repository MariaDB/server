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
# MariaDB Post-Thaw Script for MariaDB 11.5+ (InnoDB only)
#
# Use this script together with the mariadb-pre-freeze.sh script for external
# volume-based backup tools like Veeam.
#
# Purpose:
# - End the BACKUP STAGE and release locks after a Veeam snapshot.
# - Clean up the runtime directory created by the pre-freeze script.
#
# Usage:
# - Optionally override SCRIPT_TIMEOUT_SEC (the script can run slightly longer).
# - Optionally override DEBUG_LOGS_ENABLED=true for verbose logging.
#
# Requirements:
# - A running mariadb client session started by the pre-freeze script.
# - The runtime directory with FIFO files and state file must exist.
# - systemd-cat is optional; without it logs go to stdout/stderr only.
#
# Notes:
# - If FREEZE_AND_THAW_TIMEOUT_SEC (from pre-freeze) has expired, the mariadb
#   client may already be terminated. In this case, BACKUP STAGE END will fail,
#   but the script will still clean up the runtime directory.
#
# Flow and Process Safety:
# - This script is called AFTER the snapshot is taken
# - It communicates with the persistent mariadb session started by pre-freeze
# - It must release all BACKUP STAGE locks before returning
# - If the persistent session is dead (timeout), it still cleans up the runtime dir
# - Exit code 0 = success (BACKUP STAGE END completed)
# - Exit code 1 = warnings/errors (e.g., client already dead, but cleanup OK)
#
# Return Codes:
#   0 = Everything OK, BACKUP STAGE END sent successfully
#   1 = Errors detected (state file mismatch, client dead, FIFOs missing, etc.)
#       In this case, runtime dir is still cleaned up, but backup consistency is unclear.
#       Administrator should verify database state: SHOW VARIABLES LIKE 'innodb_backup_stage';
#
# Required State File Content:
# - pre-freeze must have written "BLOCK_COMMIT" to the state file
# - post-thaw verifies this state and warns if it differs
# - If state is unknown, return code will be 1
#
# Process Cleanup Order (Important):
# 1. Send BACKUP STAGE END (if client still alive)
# 2. Kill mariadb client (releases backup stage locks)
# 3. Kill FIFO holder (closes FIFOs, allows mariadb to fully exit)
# 4. Remove lock file (signals any remaining processes)
# 5. Remove entire runtime directory
#
# Troubleshooting:
#
# 1. "RUN_DIR does not exist":
#    - pre-freeze never ran or crashed before creating run dir
#    - Check if database is actually locked: SHOW PROCESSLIST;
#    - Manually check BACKUP STAGE state: SHOW VARIABLES LIKE 'innodb_backup_stage';
#    - Manually release if stuck: BACKUP STAGE END;
#    - You may need to investigate why pre-freeze failed
#
# 2. "Unexpected state: START/FLUSH/BLOCK_DDL":
#    - pre-freeze crashed after starting stages but before reaching BLOCK_COMMIT
#    - Return code will be 1, but cleanup still proceeds
#    - This is a sign of serious problems; investigate pre-freeze logs
#    - Database may be in inconsistent state; verify backup integrity after restore
#
# 3. "mariadb client already stopped (may have timed out)":
#    - Snapshot took > FREEZE_AND_THAW_TIMEOUT_SEC
#    - Mariadb was force-killed by the timeout wrapper
#    - BACKUP STAGE locks may still be held (check with SHOW PROCESSLIST)
#    - Cleanup still proceeds; runtime dir removed
#    - Administrator should verify database state manually
#    - Next backup attempt can proceed (pre-freeze will clean stale lock)
#
# 4. "BACKUP STAGE END failed":
#    - mariadb connection lost or broken
#    - SQL command did not return the expected marker
#    - Return code will be 1
#    - Cleanup still proceeds, but database may remain locked
#    - Check logs: journalctl -t mariadb-stage-backup
#
# 5. "Timeout waiting for SQL response":
#    - post-thaw itself timed out (SCRIPT_TIMEOUT_SEC exceeded)
#    - FIFOs may be broken or mariadb hung
#    - Increase timeout: env SCRIPT_TIMEOUT_SEC=300 ./mariadb-post-thaw.sh
#    - Or force kill: kill -9 <mariadb_pid>, rm -rf ~/.mariadb-stage-backup-run-dir
#
# 6. "mariadb pid validation failed (likely PID reuse)":
#    - The PID file contains a dead or wrong process
#    - Script will skip killing (safety measure)
#    - Runtime dir is still cleaned up
#    - Check for zombie mariadb processes: ps aux | grep -i mariadb
#
# Best Practices:
# - Always check logs after every backup: journalctl -t mariadb-stage-backup -n 50
# - Monitor FREEZE_AND_THAW_TIMEOUT_SEC; set it > expected snapshot time + 5 min
# - Test the full backup pipeline regularly (pre-freeze + snapshot + post-thaw)
# - Document your snapshot tool's expected duration and behavior
# - Verify restore procedure (point-in-time recovery) in a test environment
# - Keep detailed backups logs and correlate them with database logs
# - Alert on non-zero exit codes from either script
#

set -euo pipefail
readonly DEBUG_LOGS_ENABLED="${DEBUG_LOGS_ENABLED:-false}"
readonly SCRIPT_TIMEOUT_SEC="${SCRIPT_TIMEOUT_SEC:-180}"  # 3 minutes default

# Safety check: HOME must be set and absolute
if [[ -z "${HOME:-}" ]] || [[ ! "${HOME}" =~ ^/ ]]; then
    echo "[ERROR] HOME is not set or not absolute. Cannot proceed." >&2
    exit 1
fi

readonly LOG_PREFIX="[post-thaw]"
readonly SQL_MARKER="---MARKER---"

# same as in mariadb-pre-freeze.sh!
readonly LOG_TAG="mariadb-stage-backup"
readonly RUN_DIR="${HOME}/.mariadb-stage-backup-run-dir"
readonly MARIADB_PID_FILE="${RUN_DIR}/mariadb.pid"
readonly FIFO_IN="${RUN_DIR}/mariadb.in.fifo"
readonly FIFO_OUT="${RUN_DIR}/mariadb.out.fifo"
readonly STATE_FILE="${RUN_DIR}/mariadb-stage-backup.state"

# remember start time of this script.
SCRIPT_START_EPOCH="$(date +%s)"
readonly SCRIPT_START_EPOCH

HAS_SYSTEMD_CAT="false"
if command -v systemd-cat &>/dev/null; then
    HAS_SYSTEMD_CAT="true"
fi
readonly HAS_SYSTEMD_CAT

# --- Logs to journald via systemd-cat and to stdout and stderr according to log level. ---
# Usage: log <prio 0..7> <message...>
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

# helper function: remaining time in seconds until script timeout
# A minimum of 1 sec will be returned so that timed reads do not exit immediately.
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

# --- check directory ---
# Without data from the run directory this script cannot work.
if [[ ! -d "${RUN_DIR}" ]]; then
    log_error "RUN_DIR (${RUN_DIR}) does not exist. The database may be locked! The mariadb process may still be running. The backup may be inconsistent."
    exit 1
fi

# Trying to free the locks in a clean way, with a best effort failover.
# If the clean way does not work, the return code will not be 0.
return_code=0

# --- check state file ---
current_state="$(cat "${STATE_FILE}" 2>/dev/null || echo "UNKNOWN")"
log_info "Current backup state: ${current_state}"
if [[ "${current_state}" != "BLOCK_COMMIT" ]]; then
    log_warn "Unexpected state: ${current_state}. Backup may be inconsistent."
    return_code=1
else
    log_debug "state file check passed with: ${current_state}"
fi

# --- Load mariadb pid (actually the timeout wrapper PID from pre-freeze) ---
MARIADB_PID=""
if [[ -f "${MARIADB_PID_FILE}" ]]; then
    MARIADB_PID="$(get_pid_from_file "${MARIADB_PID_FILE}" || true)"
    if [[ -z "${MARIADB_PID}" ]]; then
        log_error "No mariadb pid found in ${MARIADB_PID_FILE}."
        return_code=1
    elif ! validate_pid "${MARIADB_PID_FILE}" "timeout.*mariadb"; then
        log_error "mariadb pid validation failed (likely PID reuse) in ${MARIADB_PID_FILE}."
        return_code=1
        MARIADB_PID=""  # Reset to avoid killing wrong process
    else
        log_debug "Loaded and validated mariadb pid (timeout wrapper): ${MARIADB_PID}"
    fi
else
    log_error "Missing mariadb pid file: ${MARIADB_PID_FILE}."
    return_code=1
fi
readonly MARIADB_PID

if [[ -n "${MARIADB_PID}" ]] && kill -0 "${MARIADB_PID}" 2>/dev/null; then
    # --- send SQL "BACKUP STAGE END" if FIFOs exist ---
    if [[ -p "${FIFO_IN}" && -p "${FIFO_OUT}" ]]; then
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
            local has_error=0

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
                    log_debug "SQL Error in response: ${line}"
                    has_error=1
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
                if (( has_error == 1 )); then
                    log_error "SQL command failed (error detected in response): ${sql}, response ${response}"
                    return 1
                fi
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

        # --- send BACKUP STAGE END ---
        log_info "Sending BACKUP STAGE END to persistent session..."
        if send_sql "BACKUP STAGE END;"; then
            echo "END" > "${STATE_FILE}"
            log_info "BACKUP STAGE END completed successfully"
        else
            log_error "BACKUP STAGE END failed"
            return_code=1
        fi

        # --- Close FDs, but FIFO_HOLDER background process still keeps the FIFOs open. ---
        exec 3>&-
        exec 4<&-
    else
        log_warn "Missing FIFO files: ${FIFO_IN} and/or ${FIFO_OUT}."
        return_code=1
    fi

    # --- Stop mariadb process (via timeout wrapper), that will also free the "backup stage" locks. ---
    # Killing the timeout wrapper will propagate the signal to the mariadb client.
    if kill -0 "${MARIADB_PID}" 2>/dev/null; then
        stop_and_kill_by_pid_file "${MARIADB_PID_FILE}" "mariadb_client_via_timeout_wrapper" "timeout.*mariadb"
    else
        log_info "mariadb client already stopped (may have timed out after ${FREEZE_AND_THAW_TIMEOUT_SEC}s)."
        rm -f "${MARIADB_PID_FILE}"
    fi
fi

# --- Final cleanup: remove entire run dir ---
# Also deletes the lock file which signals the fifo_holder background process from pre-freeze script to stop.
rm -rf "${RUN_DIR}" 2>/dev/null || true
log_info "Runtime directory (${RUN_DIR}) cleaned up."

log_info "Post-thaw cleanup completed"
exit ${return_code}
