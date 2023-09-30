#!/bin/bash
# Create a docker image with volumes set up. This script is an entrypoint
# and lets you choose an operation

set -eau

this_path=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
maria_root="$(dirname "$(dirname "$this_path")")"
rust_dir=${maria_root}/rust
script_dir=${rust_dir}/scripts
dockerfile=${rust_dir}/scripts/Dockerfile
obj_dir=${maria_root}/docker_obj

echo "using root $maria_root"
echo "using script_dir $script_dir"
echo "using dockerfile $dockerfile"
echo "using obj_dir $obj_dir"

mkdir -p "$obj_dir"

docker_args=()
# docker_args="$docker_args --volume $maria_root:/checkout"
docker_args=("${docker_args[@]}" "--volume" "$maria_root:/checkout:ro")
docker_args=("${docker_args[@]}" "--volume" "$obj_dir:/obj")
docker_args=("${docker_args[@]}" "--rm")
docker_args=("${docker_args[@]}" "--name" "mdb-plugin-test")

build_cmd="/checkout/rust/scripts/launch/build_maria.sh"
test_cmd="/checkout/rust/scripts/launch/run_mtr.sh"
start_cmd="/checkout/rust/scripts/launch/install_run_maria.sh"

make_exports="export BUILD_CMD=$build_cmd && export TEST_CMD=test_cmd && export START_CMD=start_cmd"

help="USAGE: ./run.sh build|test|shell"

if [ -z "${1:-""}" ]; then
    echo "$help"
    exit 1
elif [ "$1" = "shell" ]; then
    echo building for terminal
    command="$make_exports && /bin/bash"
    docker_args=("${docker_args[@]}" "-it")
elif [ "$1" = "build" ]; then
    echo building mariadb
    command="$make_exports && $build_cmd"
elif [ "$1" = "test" ]; then
    echo testing mariadb
    command="$make_exports && $build_cmd && $test_cmd"
elif [ "$1" = "start" ]; then
    echo starting mariadb
    command="$make_exports && $build_cmd && $start_cmd"
else
    echo invalid command
    exit 1
fi

# allow using podman
if [ -z "${2:-""}" ]; then
    launch="docker"
else
    launch="$2"
fi
    
echo cmd
echo "command: $command"
echo "run args:" "${docker_args[@]}"
    
"$launch" build --file "$dockerfile" --tag mdb-rust .

"$launch" run \
    --workdir /obj \
    "${docker_args[@]}" \
    mdb-rust \
    /bin/bash -c "$command"
