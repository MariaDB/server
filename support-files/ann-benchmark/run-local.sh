#!/bin/bash
set -e

# Description:
#   This script facilitates the execution of the ANN (Approximate Nearest Neighbors) benchmarking test using the ann-benchmarks tool.
#   It runs the benchmark either against local builds or a specified folder where the MariaDB server is installed.
#   Additionally, it offers support for overriding default behavior through environment variables.
#
# Dependencies:
#   - Python 3.10+
#   - Python library dependencies required by ann-benchmarks tool.
#
# Usage:
#   [MARIADB_ROOT_DIR=./builddir] [ANN_WORKSPACE=./ann-workspace] [DATASET=random-xs-20-euclidean] [FLAMEGRAPH=yes] ./support-files/ann-benchmark/run-local.sh
#
# Environment Variables:
#   - MARIADB_BUILD_DIR: Path to the directory where MariaDB is built. Default: '<MariaDB source dir>/builddir'
#   - ANN_WORKSPACE:     Path to the workspace directory. Default: '<MariaDB source dir>/ann-workspace'
#   - DATASET:           Dataset to be used for benchmarking. Default: 'random-xs-20-euclidean'
#   - FLAMEGRAPH:        Generate flame graph using perf. <yes/no>. Default: no

# Ensure that subprocesses are terminated upon an unexpected exit.
trap 'pkill -P $$' EXIT INT

# Default paths
SCRIPT_DIR=$(dirname "$(readlink -m "$0")")
MARIADB_SOURCE_DIR="$SCRIPT_DIR/../.."
MARIADB_BUILD_DIR=$(realpath "${MARIADB_BUILD_DIR:-$MARIADB_SOURCE_DIR/builddir}")
ANN_WORKSPACE=$(realpath "${ANN_WORKSPACE:-$MARIADB_SOURCE_DIR/ann-workspace}")

# Default dataset
DATASET="${DATASET:-random-xs-20-euclidean}"

# Sub-workdirs
ANN_SOURCE_DIR=$ANN_WORKSPACE/ann-benchmarks
MARIADB_DB_WORKSPACE=$ANN_WORKSPACE/mariadb-workspace

# Ensure the build artifacts exist
if [ ! -d $MARIADB_BUILD_DIR ] || [ ! -f $MARIADB_BUILD_DIR/*/mariadbd ]; then
  echo "Error: '$MARIADB_BUILD_DIR' does not exist or 'mariadbd' file does not found."
  exit 1
fi

mkdir -p $ANN_WORKSPACE
cd $ANN_WORKSPACE

# Download ann-benchmarks in the target folder and install dependencies
function install_ann_benchmarks() {
  target_dir=$1
  
  # Check if Python 3.10 or newer is available, required by ann-benchmarks
  if ! python3 -c 'import sys; exit(sys.version_info < (3,10))'; then
      echo -e "Python is required but not found. Please install Python 3.10 or higher to proceed.\n"
      exit 1
  fi
  
  ann_git_repo="https://github.com/HugoWenTD/ann-benchmarks.git"
  ann_git_branch="mariadb"
  
  echo -e "Downloading ann-benchmark...\n"
  if [ ! -d "$target_dir" ]; then
      # Only clone ann-benchmarks repository if it doesn't exist
      git clone --branch "$ann_git_branch" "$ann_git_repo" --depth 1 "$target_dir" || {
          echo -e "Failed to clone ann-benchmarks repository. Please check your internet connection and try again.\n"
          exit 1
      }
      # Existing benchmark results for various algorithms in the repository may cause confusion:
      # https://github.com/erikbern/ann-benchmarks/tree/main/results
      # Deleting these irrelevant images to prevent confusion.
      rm -rf $target_dir/results/*
  else
      # Do not overwrite the script to allow user customization
      echo -e "[WARN] ann-benchmarks repository already exists. Skipping cloning. Remove $target_dir if you want it to be re-initialized.\n"
  fi
  
  echo -e "Installing ann-benchmark dependencies...\n"
  if ! pip3 install -q -r $target_dir/requirements.txt mariadb; then
      echo -e "Failed to install dependencies. Please make sure pip is installed and try again.\n"
      exit 1
  fi
}

# Prepare ann-benchmarks
install_ann_benchmarks $ANN_SOURCE_DIR
cd $ANN_SOURCE_DIR

# Env variables as arguments for ann-benchmarks run
export DO_INIT_MARIADB=yes
export MARIADB_ROOT_DIR="$MARIADB_BUILD_DIR"
export MARIADB_DB_WORKSPACE
export MARIADB_SOURCE_DIR
export FLAMEGRAPH

# Remove previous results if there's any
rm -rf results/$DATASET

echo -e "Starting ann-benchmark...\n"
# One known issue of ann-benchmars tool is that it returns 0 even test failed:
python3 -u run.py  --algorithm mariadb --dataset $DATASET --local

echo -e "\nAnn-benchmark exporting data...\n"
python3 -u data_export.py --out results/res.csv

echo -e "\nAnn-benchmark plotting...\n"
python3 -u plot.py --dataset $DATASET
# For this version we use the recall rate and QPS provided by the tool for eveluation of the performance.
# Note that QPS number could be different on different instance type.
echo -e "\nAnn-benchmark plot done, the last two colunms in above output for 'recall rate' and 'QPS'. ^^^ \n"
echo -e "\n[COMPLETED]\n"
