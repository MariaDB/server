#!/bin/sh

base_dir=$(dirname $0)

if [ 2 != $# ]; then
  echo "usage: $0 db_path eijiro.csv_path"
  exit 1
fi

if iconv -f UCS2 -t UTF8 $2 | ${base_dir}/eijiro2grn.rb | groonga $1 > /dev/null; then
  echo "eijiro data loaded."
fi
