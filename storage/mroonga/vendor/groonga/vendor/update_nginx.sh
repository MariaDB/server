#!/bin/sh

set -u
set -e
set -x

if [ $# != 1 ]; then
    echo "Usage: $0 VERSION"
    echo " e.g.: $0 1.2.6"
    exit 1
fi

new_nginx_version="$1"

base_dir="$(dirname "$0")"
top_src_dir="${base_dir}/.."

nginx_version_file="${top_src_dir}/nginx_version"
current_nginx_version=$(cat "${nginx_version_file}")

current_nginx_dir="${base_dir}/nginx-${current_nginx_version}"

new_nginx_base_name="nginx-${new_nginx_version}"
new_nginx_tar_gz="${new_nginx_base_name}.tar.gz"
wget "http://nginx.org/download/${new_nginx_tar_gz}"

tar xzf "${new_nginx_tar_gz}"
rm "${new_nginx_tar_gz}"

echo "${new_nginx_version}" > "${nginx_version_file}"

git add "${new_nginx_base_name}"
git rm -rf "${current_nginx_dir}" || :
