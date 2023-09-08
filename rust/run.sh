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

args=()
# args="$args --volume $maria_root:/checkout"
args=("${args[@]}" "--volume" "$maria_root:/checkout:ro")
args=("${args[@]}" "--volume" "$obj_dir:/obj")
args=("${args[@]}" "--rm")

build_cmd="/checkout/rust/scripts/launch/build_maria.sh"
test_cmd="/checkout/rust/scripts/launch/run_mtr.sh"

help="usage: ./run_docker.sh build|test|shell"

if [ -z "${1:-""}" ]; then
    echo "$help"
    exit 1
elif [ "$1" = "shell" ]; then
    echo building for terminal
    command="/bin/bash"
    args=("${args[@]}" "-it")
elif [ "$1" = "build" ]; then
    echo building mariadb
    command="$build_cmd"
elif [ "$1" = "test" ]; then
    echo testing mariadb
    command="$build_cmd && $test_cmd"
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
echo "run args:" "${args[@]}"
    
"$launch" build --file "$dockerfile" --tag mdb-rust .

"$launch" run \
    --workdir /obj \
    "${args[@]}" \
    mdb-rust \
    /bin/bash -c "$command"
