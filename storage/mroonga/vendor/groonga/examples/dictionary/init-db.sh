#!/bin/sh

if [ 1 != $# ]; then
  echo "usage: $0 db_path"
  exit 1
fi

if groonga-suggest-create-dataset $1 dictionary > /dev/null; then
  echo "db initialized."
fi
