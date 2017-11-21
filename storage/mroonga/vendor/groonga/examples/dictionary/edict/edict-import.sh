#!/bin/sh

base_dir=$(dirname $0)

if [ 1 != $# -a 2 != $# ]; then
  echo "usage: $0 db_path [edict.gz_path]"
  exit 1
fi

if [ -z $2 ]; then
    edict_gz=edict.gz
    if [ ! -f $edict_gz ]; then
	wget -O $edict_gz http://ftp.monash.edu.au/pub/nihongo/edict.gz
    fi
else
    edict_gz=$2
fi

if type gzcat > /dev/null 2>&1; then
    zcat="gzcat"
else
    zcat="zcat"
fi

if $zcat $edict_gz | ${base_dir}/edict2grn.rb | groonga $1 > /dev/null; then
  echo "edict data loaded."
fi
