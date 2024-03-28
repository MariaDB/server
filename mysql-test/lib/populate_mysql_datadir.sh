#!/bin/bash

function usage() {
    cat <<EOM
    This is a helper script that populates a complete MySQL data directory
    via MySQL official docker image.

    Usage:
        $0 version dst_dir [mysql_files]...

        version     - specify which version of MySQL to use (e.g. 5.7)
        dst_dir     - destination directory that the generated datadir will be copied into
        mysql_files - .sql file to execute and generate specific data in datadir
EOM
}

echo "Cleaning up the old container if there is any..."
docker stop mysql1
docker rm mysql1

if [ $# -lt 2 ]; then
    usage && exit
fi

mysql_version="${1}"
dst_dir="${2}"
mysql_files="${@:3}"

if [ ! -d ${dst_dir} ]; then
    echo "Please provide a valid destination directory"
    exit 1
fi

echo "Acquiring MySQL docker image..."
sudo docker pull mysql/mysql-server:${mysql_version}
retval=$?
if [ ${retval} -ne 0 ]; then
    echo "Failed to get MySQL docker image. Please make sure a valid MySQL version is passed in. errno=${retval}"
    exit 1
fi
echo "'Acquire MySQL docker image' Done"

echo "Starting MySQL server and wait for 10s for it to be ready..."
sudo docker run --name=mysql1 -e MYSQL_ALLOW_EMPTY_PASSWORD=yes -d mysql/mysql-server:${mysql_version} --early-plugin-load=keyring_file.so
sleep 10
echo "'Start MySQL server' Done"

echo "Verifying connection to server..."
docker exec -t mysql1 mysql -uroot --execute='SELECT 1;'
retval=$?
if [ ${retval} -ne 0 ]; then
    echo "Failed to connect to MySQL server. errno=${retval}"
    docker stop mysql1
    docker rm mysql1
    exit 1
fi
echo "'Connect to MySQL server' Done"

echo "Start executing all MySQL commands..."
for sql in ${mysql_files}
do
    echo "Executing ${sql}..."
    docker exec -i mysql1 mysql -uroot < $sql
done
echo "'Execute all commands' Done"

# Stop the server before copying the files so the files are shut down properly
docker stop mysql1

echo "Copying the datadir to ${dst_dir}"
docker cp mysql1:/var/lib/mysql "${dst_dir}"
echo "'Copy datadir' Done"

docker rm mysql1
echo "Complete populating datadir. Exit."
