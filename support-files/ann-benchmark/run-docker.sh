#!/bin/bash
set -e

# Description:
#   This script automates the execution of the ANN benchmarking test with the ann-benchmarks tool within a Docker container.
#   It builds the required Docker image if it doesn't exist or if forced, and then runs the benchmark in the specified workspace.
#   Additionally, it offers support for overriding default behavior through environment variables.
#
# Dependencies:
#   - Docker
#
# Usage:
#   [MARIADB_SOURCE_DIR=.] [ANN_WORKSPACE=./ann-workspace] [DOCKER_REBUILD=yes] [MARIADB_REBUILD=yes] [DATASET=random-xs-20-euclidean] ./support-files/ann-benchmark/run-docker.sh
#
# Environment Variables:
#   - ANN_WORKSPACE: Path to the workspace directory. Default: '<MariaDB source dir>/ann-workspace'
#   - DOCKER_REBUILD: Flag to force the rebuild of the Docker image. Default: no
#   - MARIADB_REBUILD: Flag to determine if MariaDB needs to be rebuilt. Default: yes
#   - DATASET: Dataset to be used for benchmarking. Default: 'random-xs-20-euclidean'

# Default paths
SCRIPT_DIR=$(dirname "$(readlink -m "$0")")
MARIADB_SOURCE_DIR="$SCRIPT_DIR/../.."
ANN_WORKSPACE=$(realpath "${ANN_WORKSPACE:-$MARIADB_SOURCE_DIR/ann-workspace}")

# Check if ANN_WORKSPACE exists, and create it if it doesn't
if [ ! -d "$ANN_WORKSPACE" ]; then
  mkdir -p "$ANN_WORKSPACE" || { echo -e "Error: Failed to create directory '$ANN_WORKSPACE'.\n"; exit 1; }
fi

IMAGE_NAME="mariadb-ann-benchmark"
# Check if the Docker image exists and whether to force rebuild
if [[ "$(docker images -q $IMAGE_NAME:latest 2> /dev/null)" == "" || "$DOCKER_REBUILD" == "yes" ]]; then
  echo "Rebuilding docker image $IMAGE_NAME ..."
  docker build -t $IMAGE_NAME -f $SCRIPT_DIR/Dockerfile $MARIADB_SOURCE_DIR
else
  echo "Docker image found."
fi

# Default value for MARIADB_REBUILD
MARIADB_REBUILD=${MARIADB_REBUILD:-yes}

cmd="./support-files/ann-benchmark/run-local.sh"
# Run Ann-benchmark in docker container
if [ "$MARIADB_REBUILD" == yes ]; then
  cmd="./support-files/ann-benchmark/server-vector-build.sh && $cmd"
fi

vol_options="-v $MARIADB_SOURCE_DIR:/build/server -v $ANN_WORKSPACE:/build/ann-workspace"
env_options="-e ANN_WORKSPACE=/build/ann-workspace -e MARIADB_BUILD_DIR=/build/ann-workspace/builddir -e CCACHE_DIR=/build/server/.ccache -e DATASET=$DATASET"

docker run -it --rm --user $(id -u):$(id -g) $env_options $vol_options $IMAGE_NAME /bin/bash -c "$cmd"
