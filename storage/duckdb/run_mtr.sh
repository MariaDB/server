#!/bin/bash

set -e
set -o pipefail

SCRIPT_LOCATION=$(dirname "$0")
source "$SCRIPT_LOCATION/utils.sh"

MDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION"/../../../)
DUCKDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION")
BUILD_PATH=$(realpath "$MDB_SOURCE_PATH"/../DuckdbBuildOf_$(basename "$MDB_SOURCE_PATH"))
MTR_PATH="$BUILD_PATH/mysql-test"
MTR="$MTR_PATH/mtr"
SUITE="duckdb"
SUITE_DIR="$DUCKDB_SOURCE_PATH/mysql-test"
BUILD_PLUGIN_DIR="$BUILD_PATH/storage/duckdb-engine/duck"

# ─── Defaults ───────────────────────────────────────────────────────────────────

TEST_NAME=""
RECORD=false
EXTERN=false
EXTERN_SOCKET=""
RUN_ALL=false
SKIP_DISABLED=false
PARALLEL=4
EXTRA_MTR_ARGS=()

# ─── Usage ──────────────────────────────────────────────────────────────────────

usage() {
    echo "Usage: $0 [options] [test_name]"
    echo ""
    echo "Modes:"
    echo "  <test_name>       Run a single test (without .test extension)"
    echo "  -a, --all         Run all tests in the suite"
    echo ""
    echo "Options:"
    echo "  -r, --record      Record test result (update .result file)"
    echo "  -e, --extern      Use externally running MariaDB (auto-detects socket)"
    echo "  --socket <path>   Socket path for extern (default: auto-detect)"
    echo "  -s, --skip-dis    Skip disabled tests (run-all-with-disabled.def)"
    echo "  -j <N>            Parallel threads (default: $PARALLEL)"
    echo "  -v, --verbose     Verbose MTR output"
    echo "  -d, --debug       Run test under debugger (--debug)"
    echo "  --force            Continue on failure (--force)"
    echo "  --retry=N          Retry failed tests N times"
    echo "  -h, --help        Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 create_table_column          # run one test"
    echo "  $0 -r create_table_column       # run and record"
    echo "  $0 -a                            # run all tests"
    echo "  $0 -a -e                         # run all against extern server"
    echo "  $0 -e create_table_column       # single test against extern server"
    exit 0
}

# ─── Parse args ─────────────────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        -a|--all)       RUN_ALL=true; shift ;;
        -r|--record)    RECORD=true; shift ;;
        -e|--extern)    EXTERN=true; shift ;;
        --socket)       EXTERN_SOCKET="$2"; shift 2 ;;
        -s|--skip-dis)  SKIP_DISABLED=true; shift ;;
        -j)             PARALLEL="$2"; shift 2 ;;
        -v|--verbose)   EXTRA_MTR_ARGS+=("--verbose"); shift ;;
        -d|--debug)     EXTRA_MTR_ARGS+=("--debug"); shift ;;
        --force)        EXTRA_MTR_ARGS+=("--force"); shift ;;
        --retry=*)      EXTRA_MTR_ARGS+=("$1"); shift ;;
        -h|--help)      usage ;;
        -*)             warn "Unknown option: $1"; usage ;;
        *)              TEST_NAME="$1"; shift ;;
    esac
done

# ─── Validate ───────────────────────────────────────────────────────────────────

if [[ ! -x "$MTR" ]]; then
    fail "MTR not found at $MTR. Build first with build.sh"
fi

# Symlink source mysql-test into build tree so MTR discovers the suite
# via its storage/*/*/mysql-test search pattern
if [[ ! -d "$BUILD_PLUGIN_DIR/mysql-test" ]]; then
    ln -sf "$SUITE_DIR" "$BUILD_PLUGIN_DIR/mysql-test"
    info "Symlinked test suite into build tree"
fi

# MTR discovers plugins via glob storage/*/*.so (one level).
# ha_duckdb.so lives in storage/duckdb-engine/duck/ (two levels).
# Symlink it one level up so MTR finds it for managed-server mode.
PARENT_PLUGIN_DIR="$BUILD_PATH/storage/duckdb-engine"
if [[ -f "$BUILD_PLUGIN_DIR/ha_duckdb.so" && ! -e "$PARENT_PLUGIN_DIR/ha_duckdb.so" ]]; then
    ln -sf "$BUILD_PLUGIN_DIR/ha_duckdb.so" "$PARENT_PLUGIN_DIR/ha_duckdb.so"
    info "Symlinked ha_duckdb.so for MTR plugin discovery"
fi

if [[ "$RUN_ALL" == false && -z "$TEST_NAME" ]]; then
    # Interactive: show menu of available tests
    mapfile -t available_tests < <(
        find "$SUITE_DIR/duckdb/t" -name "*.test" -printf "%f\n" | sed 's/\.test$//' | sort
    )
    if [[ ${#available_tests[@]} -eq 0 ]]; then
        fail "No tests found in $SUITE_DIR/duckdb/t/"
    fi
    menu_choice "Select test to run:" available_tests
    TEST_NAME="$MENU_RESULT"
fi

# ─── Resolve extern early (affects parallel) ────────────────────────────────────

if [[ "$EXTERN" == true ]]; then
    if [[ -z "$EXTERN_SOCKET" ]]; then
        EXTERN_SOCKET=$(mariadb -BNe "SELECT @@socket" 2>/dev/null || echo "/run/mysqld/mysqld.sock")
    fi
    if [[ ! -S "$EXTERN_SOCKET" ]]; then
        fail "Socket not found: $EXTERN_SOCKET. Is MariaDB running?"
    fi
    # Force serial execution with extern — all tests share one server,
    # parallel workers would collide on table names and global variables
    PARALLEL=1
fi

# ─── Build MTR command ──────────────────────────────────────────────────────────

MTR_CMD=("perl" "$MTR"
    --suite="$SUITE"
    --parallel="$PARALLEL"
    --force
    --max-test-fail=0
)

if [[ "$RECORD" == true ]]; then
    MTR_CMD+=("--record")
fi

if [[ "$EXTERN" == true ]]; then
    MTR_CMD+=("--extern" "socket=$EXTERN_SOCKET")
fi

if [[ "$SKIP_DISABLED" == true ]]; then
    MTR_CMD+=("--skip-disabled")
fi

MTR_CMD+=("${EXTRA_MTR_ARGS[@]}")

if [[ "$RUN_ALL" == false && -n "$TEST_NAME" ]]; then
    # Strip .test extension if provided
    TEST_NAME="${TEST_NAME%.test}"
    MTR_CMD+=("$SUITE.$TEST_NAME")
fi

# ─── Print summary ──────────────────────────────────────────────────────────────

header "DuckDB MTR Test Runner"
if [[ "$RUN_ALL" == true ]]; then
    info "Mode:       ${_CLR_YELLOW}all tests"
else
    info "Test:       ${_CLR_YELLOW}$TEST_NAME"
fi
if [[ "$RECORD" == true ]]; then
    warn "Recording:  ${_CLR_YELLOW}ON (will update .result files)"
fi
if [[ "$EXTERN" == true ]]; then
    info "Server:     ${_CLR_YELLOW}extern ($EXTERN_SOCKET)"
else
    info "Server:     ${_CLR_YELLOW}MTR-managed"
fi
info "Parallel:   ${_CLR_YELLOW}$PARALLEL"
info "Suite dir:  ${_CLR_YELLOW}$SUITE_DIR/duckdb"
info "Build dir:  ${_CLR_YELLOW}$BUILD_PATH"
separator
echo ""

# ─── Run ────────────────────────────────────────────────────────────────────────

info "Running: ${_CLR_DARKGRAY}${MTR_CMD[*]}"
echo ""

set +e
cd "$MTR_PATH"
"${MTR_CMD[@]}"
rc=$?
set -e

echo ""
if [[ $rc -eq 0 ]]; then
    success "All tests passed"
else
    error "Tests failed (exit code $rc)"
    # Show log location for failed tests
    if [[ -d "$MTR_PATH/var/log" ]]; then
        info "Logs: ${_CLR_YELLOW}$MTR_PATH/var/log/"
    fi
    exit $rc
fi
