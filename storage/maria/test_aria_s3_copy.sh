#!/bin/bash

#
# Note that this test expact that there are tables test1 and test2 in
# the current directory where test2 has also a .frm file
#

TMPDIR=tmpdir
LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib64/

my_cmp()
{
    if ! cmp $1 $TMPDIR/$1
    then
        echo "aborting"
        exit 1;
    fi
}

run_test()
{
    OPT=$1;
    echo "******* Running test with options '$OPT' **********"
    rm -rf $TMPDIR
    mkdir $TMPDIR
    cp test?.* $TMPDIR
    if ! ./aria_s3_copy --op=to --force $OPT test1 test2
    then
        echo Got error $?
        exit 1;
    fi
    rm test?.*
    if ! ./aria_s3_copy --op=from $OPT test1 test2
    then
        echo Got error $?
        exit 1;
    fi
    if ! ./aria_s3_copy --op=delete $OPT test1 test2
    then
        echo Got error $?
        exit 1;
    fi
    my_cmp test1.MAI
    my_cmp test1.MAD
    my_cmp test2.MAI
    my_cmp test2.MAD
    my_cmp test2.frm
    rm test?.*
    cp $TMPDIR/* .
    rm -r $TMPDIR
}

run_test ""
run_test "--s3_block_size=64K --compress"
run_test "--s3_block_size=4M"
echo "ok"
