#!/bin/bash

# This script installs an existing version of MariaDB from the archives,
# adds the appropriate users, starts the server, and stores the database
# system files before and after mariadb-upgrade is executed.
# mariadb-upgrade should do nothing since the minor version is the same.

# Adapted from: https://mariadb.com/kb/en/installing-mariadb-binary-tarballs/#installing-mariadb-as-root-in-usrlocalmysql

# Exit on error
set -e -o pipefail

GREEN='\033[0;32m'
NC='\033[0m' # No Color

# Initialize variables
source_version=""
rpm_dir=""
target_version=""

function usage {
    echo Usage:
    echo "    $0 source_version [target-version] [--rpm-dir rpm/]"
    echo "    (target_version and --rpm-dir are mutually exclusive)"

}

rpm_dir=""
# Parse long arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
    --rpm-dir)
        rpm_dir="$2"
        shift 2
        ;;
    *)
        break
        ;;
    esac
done

# Parse positional arguments
if [[ $# -lt 1 ]]; then
    echo "Missing required positional argument: source_version"
    usage
    exit 1
fi

source_version="$1"
shift

if [[ $# -gt 0 ]]; then
    target_version="$1"
    shift
fi

if [[ -n "$rpm_dir" && -n "$target_version" ]]; then
    usage
    exit 1
fi

# Check for extra arguments
if [[ $# -gt 0 ]]; then
    echo "Unknown arguments: $*"
    usage
    exit 1
fi

function log {
    local text="$*"
    echo -e "${GREEN}${text}${NC}"
}

if [[ -z "$target_version" ]]; then
    echo "No target version specified"
    if [[ -z "$rpm_dir" ]]; then
        echo "Using default RPM directory"
        rpm_dir="rpm/"
    fi
    if [ ! -d $rpm_dir ]; then
        echo "Error: directory $rpm_dir not found"
        usage
        exit 1
    fi
fi

if [[ -n "$target_version" ]]; then
    log "Source version: $source_version"
    log "Target version: $target_version"
else
    log "Source version: $source_version"
    log "RPM Directory: $rpm_dir"
fi

function wait_for_socket {
    SOCK="$1"
    LIMIT="${2:-10}"
    log "Waiting $LIMIT seconds for Unix socket to be created at $SOCK ..."
    for ii in $(seq $LIMIT); do
        [ -S $SOCK ] && break || sleep 1
    done
    if [ $ii == $LIMIT ]; then
        log "Unix socket was not created within $LIMIT seconds"
        false
    fi
}

# Install dependencies
log Installing dependencies
dnf install -y wget libxcrypt-compat libaio ncurses-compat-libs numactl-libs

install_mariadb_from_archive() {
    version=$1
    # Find latest RPM repository from https://archive.mariadb.org/
    # The RPMs for each MariaDB version are build for the latest distribution version at the time of release.
    # Certain required libraries are missing in the latest version of the distribuiton this test is being run on (currently Fedora).
    # The missing libraries depned on the latest distribution version, so that is found by scanning the MariaDB archive directory.

    log Finding MariaDB RPM repository for version $version
    if ! $(wget --spider -r --no-parent --level 1 --quiet https://archive.mariadb.org/mariadb-$version/yum/fedora/ -P /tmp/); then
        log Could not find RPMs
        exit 1
    fi
    latest_distro=$(ls -1v /tmp/archive.mariadb.org/mariadb-$version/yum/fedora/ | tail -n 1)
    rpm_repository=https://archive.mariadb.org/mariadb-$version/yum/fedora/$latest_distro/$(uname -m)/
    log RPM repository: $rpm_repository
    # log Fedora distribution: $latest_distro # Currently only supports tests on Fedora

    # Use custom repository
    log Using custom MariaDB repository
    cat <<EOF >/etc/yum.repos.d/MariaDB.repo
[mariadb]
name=MariaDB
baseurl=$rpm_repository
gpgkey=https://ftp.osuosl.org/pub/mariadb/yum/RPM-GPG-KEY-MariaDB
gpgcheck=1
EOF

    cat /etc/yum.repos.d/MariaDB.repo

    # Get version of MariaDB server. This is necessary to check which additional dependencies must be installed.
    # Full version has been defined
    major_version=$(echo $version | cut -d '.' -f1-2)
    minor_version=$(echo $version | cut -d '.' -f3)
    # Only major version has been defined, get minor version from RPM
    if [ -z $minor_version ]; then
        rpm_version=$(dnf list MariaDB-server | grep -oP 'MariaDB-server.x86_64\s+\K[0-9]+\.[0-9]+\.[0-9]+(?=-)')
        minor_version=$(echo $rpm_version | cut -d '.' -f3)
    fi
    # Install missing dependencies that are version/distro specific
    log "Installing missing libraries"
    [[ $latest_distro -le 32 ]] && dnf install -y https://download-ib01.fedoraproject.org/pub/epel/8/Everything/x86_64/Packages/b/boost169-program-options-1.69.0-5.el8.x86_64.rpm
    [[ $latest_distro == 33 ]] && dnf install -y http://springdale.princeton.edu/data/springdale/7/x86_64/os/Computational/boost173-program-options-1.73.0-7.sdl7.x86_64.rpm
    [[ $latest_distro == 34 ]] && dnf install -y https://repo.almalinux.org/almalinux/9/AppStream/x86_64/os/Packages/boost-program-options-1.75.0-8.el9.x86_64.rpm \
        https://vault.centos.org/centos/8/AppStream/x86_64/os/Packages/liburing-1.0.7-3.el8.x86_64.rpm
    [[ $latest_distro == 35 ]] && dnf install -y https://download-ib01.fedoraproject.org/pub/fedora/linux/updates/36/Everything/x86_64/Packages/b/boost-program-options-1.76.0-12.fc36.x86_64.rpm
    if [[ $major_version == "10.4" ]] && [[ $minor_version -ge 24 ]]; then
        log RPMs not available for version 10.4.24+ from this repository. You may try testing by installing from the .tar.gz binaries.
        exit 1
    fi

    log Begin installation of MariaDB version $version
    dnf install -y MariaDB-server
}

install_mariadb_from_archive $source_version

# Set variables and binaries
MYSQLD=$([ -f /usr/sbin/mariadbd ] && echo "/usr/sbin/mariadbd" || echo "/usr/sbin/mysqld")
MYSQL_BIN=/usr/bin
TMP_DATA_DIR=/tmp/var/lib/mysql
rm -rf $TMP_DATA_DIR test_upgrade.log # clean up from old tests
SOCK=/var/lib/mysql/mysql.sock

start_mariadb_server() {
    $MYSQLD --version # check version

    log Creating system tables with mysql_install_db
    $MYSQL_BIN/mysql_install_db --user=mysql --datadir=$TMP_DATA_DIR
    chown -R mysql $TMP_DATA_DIR

    # Start server
    log Starting mariadb daemon
    sudo -u mysql $MYSQLD --datadir=$TMP_DATA_DIR &
    wait_for_socket $SOCK 10
    $MYSQL_BIN/mysql --skip-column-names -e "SELECT @@version, @@version_comment" # Show version
}

shutdown_and_wait() {
    while $MYSQL_BIN/mysql -e 'SELECT 1' 2>&1 >/dev/null; do
        $MYSQL_BIN/mysql -e 'SHUTDOWN'
        sleep 1
        [ ! -S $SOCK ] && break
    done
}

log Start server with source version
start_mariadb_server

log Dump database contents in installed state
$MYSQL_BIN/mysqldump --all-databases --all-tablespaces --triggers --routines --events --skip-extended-insert >old-installed-database.sql
log Check if tables need upgrade
$MYSQL_BIN/mysqlcheck --all-databases --check-upgrade | tee -a test_upgrade.log

# Generate user tables filled with random data
$MYSQL_BIN/mysql -e "CREATE DATABASE random_data;" || true
$MYSQL_BIN/mysql -e "USE random_data;"
# Varchars
$MYSQL_BIN/mysql -D random_data -e "CREATE TABLE random_strings (string VARCHAR(255) NOT NULL);" || true
for i in {1..10}; do
    $MYSQL_BIN/mysql -D random_data -e "INSERT INTO random_strings ( string ) VALUES ( MD5(RAND()) );"
done
$MYSQL_BIN/mysql -D random_data -e "SELECT count(*) FROM random_strings;"
# Integers
$MYSQL_BIN/mysql -D random_data -e "CREATE TABLE random_numbers (number INT NOT NULL);" || true
for i in {1..10}; do
    $MYSQL_BIN/mysql -D random_data -e "INSERT INTO random_numbers ( number ) VALUES ( RAND() );"
done
$MYSQL_BIN/mysql -D random_data -e "SELECT count(*) FROM random_numbers;"

# Run mysql_upgrade which should have no effect
log Do upgrade
$MYSQL_BIN/mysql_upgrade -u root -v | tee -a test_upgrade.log
log Dump database contents in upgraded state
$MYSQL_BIN/mysqldump --all-databases --all-tablespaces --triggers --routines --events --skip-extended-insert >old-upgraded-database.sql
$MYSQL_BIN/mysql --skip-column-names -e "SELECT @@version, @@version_comment" # Show version
log Shutdown
shutdown_and_wait

log Uninstall source version
dnf remove -y MariaDB-server

log ------------------------------------------------
if [[ -n "$target_version" ]]; then
    install_mariadb_from_archive $target_version
elif [[ -n "$rpm_dir" ]]; then
    log Begin installation of MariaDB from local RPMs
    log "rpm_dir: $rpm_dir"
    dnf install -y $rpm_dir/*.rpm # from previous job
fi

start_mariadb_server

log Dump database contents in installed state
$MYSQL_BIN/mysqldump --all-databases --all-tablespaces --triggers --routines --events --skip-extended-insert >new-installed-database.sql || true
# The step above fails on: mariadb-dump: Couldn't execute 'show events': Cannot proceed, because event scheduler is disabled (1577)

log Run check for upgrade
$MYSQL_BIN/mysqlcheck --all-databases --check-upgrade --check --extended | tee -a test_upgrade.log

log Run upgrade
$MYSQL_BIN/mysql_upgrade -u root | tee -a test_upgrade.log # minor version is the same, so no upgrade done

log Force upgrade
$MYSQL_BIN/mysql_upgrade -u root --force | tee -a test_upgrade.log

log Dump database contents in upgraded state
$MYSQL_BIN/mysqldump --all-databases --all-tablespaces --triggers --routines --events --skip-extended-insert >new-upgraded-database.sql
$MYSQL_BIN/mysql --skip-column-names -e "SELECT @@version, @@version_comment" # Show version

log Shutdown
shutdown_and_wait
echo

# Test whether a table rebuild was triggered
if grep -E "Needs upgrade|Table rebuild required" test_upgrade.log; then
    log "================================="
    log "= A table rebuild was triggered ="
    log "================================="
    exit 1
else
    log "=================================="
    log "= No table rebuild was triggered ="
    log "=================================="
    exit 0
fi
