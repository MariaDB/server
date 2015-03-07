#!/bin/sh

base_dir=$(dirname $0)

if [ 1 != $# -a 2 != $# ]; then
  echo "usage: $0 db_path [gene.txt_path]"
  exit 1
fi

if [ -z $2 ]; then
    dictionary_dir=gene95-dictionary
    gene_txt=${dictionary_dir}/gene.txt
    if [ ! -f $gene_txt ]; then
	gene95_tar_gz=gene95.tar.gz
	wget -O $gene95_tar_gz \
	    http://www.namazu.org/~tsuchiya/sdic/data/gene95.tar.gz
	mkdir -p ${dictionary_dir}
	tar xvzf ${gene95_tar_gz} -C ${dictionary_dir}
    fi
else
    gene_txt=$2
fi

if cat $gene_txt | ${base_dir}/gene2grn.rb | groonga $1 > /dev/null; then
  echo "gene95 data loaded."
fi
