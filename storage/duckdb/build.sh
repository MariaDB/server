#!/bin/bash

set -e
set -o pipefail

SCRIPT_LOCATION=$(dirname "$0")
source "$SCRIPT_LOCATION/utils.sh"
MDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION"/../../)
DUCKDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION")
BUILD_PATH=$(realpath "$MDB_SOURCE_PATH"/../DuckdbBuildOf_$(basename "$MDB_SOURCE_PATH"))
CPUS=$(getconf _NPROCESSORS_ONLN)
BUILD_TYPE_OPTIONS=("Debug" "RelWithDebInfo")
BUILD_TYPE="${BUILD_TYPE:-}"
DISTRO_OPTIONS=("ubuntu:22.04" "ubuntu:24.04" "debian:12" "rockylinux:8" "rockylinux:9")
DEFAULT_MDB_DATADIR="/var/lib/mysql"
USER="mysql"
GROUP="mysql"
INSTALL_PREFIX="/usr/"
GCC_VERSION="12"

usage() {
    echo "Usage: $0 [options]"
    echo "  -t <type>   Build type: ${BUILD_TYPE_OPTIONS[*]} (interactive if omitted)"
    echo "  -d <distro> Distro: ${DISTRO_OPTIONS[*]} (auto-detected if omitted)"
    echo "  -j <N>      Number of parallel jobs (default: $CPUS)"
    echo "  -c          CI mode: only build, skip install"
    echo "  -p          Build packages (DEB or RPM)"
    echo "  -S          Start MariaDB after build"
    echo "  -n          No clean: keep existing data files"
    echo "  -D          Install build prerequisites (requires root/sudo)"
    echo "  -u          Build unit tests"
    echo "  -R          Use gcc-toolset-\${GCC_VERSION} on Rocky 8"
    echo "  -h          Show this help"
    exit 0
}

CI_MODE=false
START_MDB=false
NO_CLEAN=false
BUILD_PACKAGES=false
INSTALL_DEPS=false
GCC_TOOLSET=false
UNIT_TESTS=false
OS=""

while getopts "t:d:j:cpSnDRuh" opt; do
    case $opt in
        t) BUILD_TYPE="$OPTARG" ;;
        d) OS="$OPTARG" ;;
        j) CPUS="$OPTARG" ;;
        c) CI_MODE=true ;;
        p) BUILD_PACKAGES=true ;;
        S) START_MDB=true ;;
        n) NO_CLEAN=true ;;
        D) INSTALL_DEPS=true ;;
        R) GCC_TOOLSET=true ;;
        u) UNIT_TESTS=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [[ ! " ${BUILD_TYPE_OPTIONS[*]} " =~ " ${BUILD_TYPE} " ]]; then
    menu_choice "Select build type:" BUILD_TYPE_OPTIONS
    BUILD_TYPE="$MENU_RESULT"
fi

detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        local os_name=$(echo "$NAME" | cut -f1 -d" " | tr '[:upper:]' '[:lower:]')
        OS="${os_name}:${VERSION_ID}"
    elif [ -f /etc/lsb-release ]; then
        . /etc/lsb-release
        OS=$(echo "$DISTRIB_ID" | tr '[:upper:]' '[:lower:]'):"$DISTRIB_RELEASE"
    else
        fail "Cannot detect distro, specify with -d"
    fi
    info "Detected distro: ${_CLR_YELLOW}$OS"
}

select_pkg_format() {
    if [[ "$1" == *rocky* ]]; then
        PKG_FORMAT="rpm"
    else
        PKG_FORMAT="deb"
    fi
}

if [[ $BUILD_PACKAGES = true ]]; then
    if [[ ! " ${DISTRO_OPTIONS[*]} " =~ " ${OS} " ]]; then
        if [[ -z "$OS" ]]; then
            warn "Distro not specified, detecting..."
            detect_distro
        fi
        if [[ ! " ${DISTRO_OPTIONS[*]} " =~ " ${OS} " ]]; then
            menu_choice "Select distro:" DISTRO_OPTIONS
            OS="$MENU_RESULT"
        fi
    fi
    select_pkg_format "$OS"
fi

header "DuckDB Storage Engine Build"
info "Source:     ${_CLR_YELLOW}$MDB_SOURCE_PATH"
info "Build dir:  ${_CLR_YELLOW}$BUILD_PATH"
info "Build type: ${_CLR_YELLOW}$BUILD_TYPE"
info "Jobs:       ${_CLR_YELLOW}$CPUS"
if [[ $BUILD_PACKAGES = true ]]; then
    info "Packages:   ${_CLR_YELLOW}$PKG_FORMAT ($OS)"
fi
echo ""

check_user_and_group() {
    local user=$1
    if [ -z "$(grep "$user" /etc/passwd)" ]; then
        info "Adding user $user"
        useradd -r -U "$user" -d /var/lib/mysql
    fi
    if [ -z "$(grep "$user" /etc/group)" ]; then
        local gid=$(awk -F: '{uid[$3]=1}END{for(x=100; x<=999; x++) {if(uid[x] != ""){}else{print x; exit;}}}' /etc/group)
        info "Adding group $user with id $gid"
        groupadd -g "$gid" "$user"
    fi
}

clean_old_installation() {
    if [[ $NO_CLEAN = true ]]; then
        return
    fi
    rm -rf "${DEFAULT_MDB_DATADIR}"
    rm -rf /var/run/mysqld
}

bootstrap_mdb() {
    info "Bootstrap MariaDB"
    "$INSTALL_PREFIX/bin/mariadb-install-db" \
        --datadir="$DEFAULT_MDB_DATADIR" \
        --user="$USER" --group="$GROUP" > /dev/null
}

stop_mdb() {
    if "$INSTALL_PREFIX/bin/mariadb-admin" ping --silent 2>/dev/null; then
        warn "Stopping MariaDB"
        "$INSTALL_PREFIX/bin/mariadb-admin" shutdown || true
    fi
}

start_mdb() {
    info "Starting MariaDB"
    mkdir -p /run/mysqld
    chown "$USER:$GROUP" /run/mysqld
    "$INSTALL_PREFIX/bin/mariadbd-safe" --datadir="$DEFAULT_MDB_DATADIR" &

    local max_attempts=30
    local attempt=0
    while ! "$INSTALL_PREFIX/bin/mariadb-admin" ping --silent 2>/dev/null; do
        attempt=$((attempt + 1))
        if [[ $attempt -ge $max_attempts ]]; then
            local err_log="${DEFAULT_MDB_DATADIR}/$(hostname).err"
            if [[ -f "$err_log" ]]; then
                error "Last 50 lines of $err_log:"
                tail -50 "$err_log"
            fi
            fail "MariaDB failed to start within ${max_attempts} seconds"
        fi
        sleep 1
    done
    success "MariaDB is ready"
}

setup_dev_user() {
    local current_user=$(logname 2>/dev/null || echo "$SUDO_USER")
    if [[ -n "$current_user" && "$current_user" != "root" ]]; then
        info "Creating dev user '${_CLR_YELLOW}$current_user${_CLR_CYAN}'"
        "$INSTALL_PREFIX/bin/mariadb" -e \
            "CREATE USER IF NOT EXISTS '$current_user'@'localhost' IDENTIFIED VIA unix_socket;
             GRANT ALL PRIVILEGES ON *.* TO '$current_user'@'localhost';"
    fi
}

create_config() {
    # Put config in /etc/my.cnf.d/ which is included by /etc/my.cnf
    mkdir -p /etc/my.cnf.d
    cp "$DUCKDB_SOURCE_PATH/duckdb.cnf" /etc/my.cnf.d/duckdb.cnf
}

construct_cmake_flags() {
    MDB_CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE
        -DCMAKE_INSTALL_PREFIX:PATH=$INSTALL_PREFIX
        -DCMAKE_EXPORT_COMPILE_COMMANDS=1
        -DPLUGIN_ARCHIVE=NO
        -DPLUGIN_BLACKHOLE=NO
        -DPLUGIN_FEDERATED=NO
        -DPLUGIN_FEDERATEDX=NO
        -DPLUGIN_CONNECT=NO
        -DPLUGIN_MROONGA=NO
        -DPLUGIN_OQGRAPH=NO
        -DPLUGIN_ROCKSDB=NO
        -DPLUGIN_SPHINX=NO
        -DPLUGIN_SPIDER=NO
        -DPLUGIN_TOKUDB=NO
        -DPLUGIN_COLUMNSTORE=NO
        -DWITH_EMBEDDED_SERVER=NO
        -DWITH_WSREP=NO
        -DWITH_SSL=system
        -DWITH_SAFEMALLOC=OFF
        -DMYSQL_MAINTAINER_MODE=OFF
        -DPLUGIN_DUCKDB=YES
        -DWITH_SBOM=0
        -DDBUG_ON=1
    )

    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        MDB_CMAKE_FLAGS+=(-DDUCKDB_WERROR=ON)
    fi

    if [[ $UNIT_TESTS = true ]]; then
        MDB_CMAKE_FLAGS+=(-DDUCKDB_UNIT_TESTS=ON)
    fi

    if [[ $BUILD_PACKAGES = true ]]; then
        if [[ "$PKG_FORMAT" == "rpm" ]]; then
            local os_version=${OS//[^0-9]/}
            if [[ "$OS" == *rocky* ]]; then
                MDB_CMAKE_FLAGS+=(-DRPM=rockylinux${os_version})
            fi
        else
            local codename=""
            case "$OS" in
                debian:12*)   codename="bookworm" ;;
                ubuntu:22.04) codename="jammy" ;;
                ubuntu:24.04) codename="noble" ;;
                *)            fail "Unknown DEB codename for $OS" ;;
            esac
            MDB_CMAKE_FLAGS+=(-DDEB=${codename} -DINSTALL_LAYOUT=DEB)
        fi
    else
        MDB_CMAKE_FLAGS+=(-DDEB=noble -DINSTALL_LAYOUT=DEB)
    fi
}

install_deps() {
    if [[ $INSTALL_DEPS = false ]]; then
        return
    fi

    if [[ -z "$OS" ]]; then
        detect_distro
    fi

    local SUDO=""
    if [[ $EUID -ne 0 ]]; then
        SUDO="sudo"
    fi

    # MariaDB server + DuckDB build prerequisites
    local RPM_DEPS="git make cmake ninja-build bison flex \
        ncurses-devel readline-devel openssl-devel zlib-devel bzip2-devel \
        libzstd-devel libcurl-devel libaio-devel libxml2-devel pcre2-devel \
        libxcrypt-devel xz-devel pam-devel perl-DBI python3 python3-devel \
        ccache rpm-build"

    local DEB_DEPS="build-essential git cmake ninja-build bison flex \
        libncurses-dev libreadline-dev libssl-dev zlib1g-dev libbz2-dev \
        libzstd-dev libcurl4-openssl-dev libaio-dev libxml2-dev libpcre2-dev \
        libxcrypt-dev liblzma-dev libpam0g-dev libperl-dev python3 python3-dev \
        ccache devscripts equivs debhelper libdistro-info-perl"

    local command=""
    case "$OS" in
        rockylinux:8|rocky:8)
            command="$SUDO dnf install -y 'dnf-command(config-manager)' epel-release && \
                     $SUDO dnf config-manager --set-enabled powertools && \
                     $SUDO dnf install -y ${RPM_DEPS}"
            if [[ $GCC_TOOLSET = true ]]; then
                command="$command && $SUDO dnf install -y gcc-toolset-${GCC_VERSION} gcc-toolset-${GCC_VERSION}-gcc-c++"
                warn "Activate toolchain before rebuilding: . /opt/rh/gcc-toolset-${GCC_VERSION}/enable"
            else
                command="$command && $SUDO dnf groupinstall -y \"Development Tools\""
                warn "Rocky 8 default gcc 8 lacks C++20 -- consider re-running with -R"
            fi
            ;;
        rockylinux:9|rocky:9|rocky:10)
            command="$SUDO dnf install -y 'dnf-command(config-manager)' epel-release && \
                     $SUDO dnf config-manager --set-enabled crb && \
                     $SUDO dnf install -y gcc gcc-c++ ${RPM_DEPS}"
            ;;
        ubuntu:*|debian:*)
            command="$SUDO apt-get update && \
                     DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y ${DEB_DEPS}"
            ;;
        *)
            fail "Unsupported distro for -D: $OS"
            ;;
    esac

    separator
    info "Installing prerequisites for ${_CLR_YELLOW}$OS"
    set +e
    eval "$command" | one_liner
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "DEPENDENCY INSTALL FAILED (exit code $rc)"
    success "Prerequisites installed"
}

construct_cmake_flags
install_deps

build_binary() {
    separator
    info "Configuring"
    set +e
    cmake "${MDB_CMAKE_FLAGS[@]}" -S"$MDB_SOURCE_PATH" -B"$BUILD_PATH" | one_liner
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "CONFIGURE FAILED (exit code $rc)"

    separator
    info "Building with ${_CLR_YELLOW}$CPUS${_CLR_CYAN} jobs"
    set +e
    cmake --build "$BUILD_PATH" -j "$CPUS" | one_liner
    rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "BUILD FAILED (exit code $rc)"

    success "Build complete"
    info "Adding compile_commands.json symlink"
    ln -sf "$BUILD_PATH/compile_commands.json" "$MDB_SOURCE_PATH"
}

build_package() {
    separator
    info "Building ${_CLR_YELLOW}$PKG_FORMAT${_CLR_CYAN} package for ${_CLR_YELLOW}$OS"

    set +e
    if [[ "$PKG_FORMAT" == "rpm" ]]; then
        cd "$BUILD_PATH"
        make -j "$CPUS" package | one_liner
    else
        cd "$MDB_SOURCE_PATH"
        export DEBIAN_FRONTEND="noninteractive"
        export DEB_BUILD_OPTIONS="parallel=$CPUS"
        export BUILDPACKAGE_FLAGS="-b"
        CMAKEFLAGS="${MDB_CMAKE_FLAGS[*]}" debian/autobake-deb.sh | one_liner
    fi
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "PACKAGE BUILD FAILED (exit code $rc)"
    success "Packages ready"
}

build_binary

if [[ $BUILD_PACKAGES = true ]]; then
    build_package
    header "BUILD FINISHED"
    exit 0
fi

if [[ $CI_MODE = false ]]; then
    check_user_and_group "$USER"
    stop_mdb
    clean_old_installation

    separator
    info "Installing"
    set +e
    cmake --install "$BUILD_PATH" | one_liner
    rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "INSTALL FAILED (exit code $rc)"

    create_config
    if [[ $NO_CLEAN = false ]]; then
        bootstrap_mdb
    else
        warn "Skipping bootstrap (--no-clean mode, keeping existing data)"
    fi
fi

if [[ $START_MDB = true ]]; then
    stop_mdb
    start_mdb
    setup_dev_user

fi

header "BUILD FINISHED"
