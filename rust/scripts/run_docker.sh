#!/bin/bash
set -eau

this_path=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
script_dir=$(dirname "$this_path")/docker
maria_root=$(dirname "$(dirname "$(dirname "$script_dir")")")
rust_dir=${maria_root}/rust
dockerfile=${maria_root}/rust/Dockerfile
obj_dir=${maria_root}/docker_obj

echo $script_dir
echo $maria_root
echo $dockerfile
echo $obj_dir

mkdir -p "$obj_dir"

args=""
# args="$args --volume $maria_root:/checkout"
args="$args --volume $maria_root:/checkout:ro"
args="$args --volume $obj_dir:/obj"
args="$args --rm"

build_cmd="/checkout/rust/scripts/docker/build_maria.sh"
test_cmd="/checkout/rust/scripts/docker/run_mtr.sh"

echo 1
echo $1 "build"

if [ -z "${1:-""}" ]; then
    echo "no command provided"
    exit 1
elif [ "$1" = "shell" ]; then
    echo building for terminal
    command="/bin/bash"
    args="$args -it"
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

echo cmd
echo $command
echo $args

    
docker build --file "$dockerfile" --tag mdb-rust .

docker run \
    --workdir /obj \
    $args \
    mdb-rust \
    /bin/bash -c "$command"
