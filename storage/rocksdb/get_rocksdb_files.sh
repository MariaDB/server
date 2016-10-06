#!/bin/bash
MKFILE=`mktemp`
# create and run a simple makefile
# include rocksdb make file relative to the path of this script
echo "include rocksdb/src.mk
all:
	@echo \$(LIB_SOURCES)" > $MKFILE
for f in `make --makefile $MKFILE`
do
  echo ../../rocksdb/$f
done
rm $MKFILE

# create build_version.cc file. Only create one if it doesn't exists or if it is different
# this is so that we don't rebuild mysqld every time
bv=rocksdb/util/build_version.cc
date=$(date +%F)
git_sha=$(pushd rocksdb >/dev/null && git rev-parse  HEAD 2>/dev/null && popd >/dev/null)
if [ ! -f $bv ] || [ -z $git_sha ] || [ ! `grep $git_sha $bv` ]
then
echo "#include \"build_version.h\"
const char* rocksdb_build_git_sha =
\"rocksdb_build_git_sha:$git_sha\";
const char* rocksdb_build_git_date =
\"rocksdb_build_git_date:$date\";
const char* rocksdb_build_compile_date = __DATE__;" > $bv
fi
