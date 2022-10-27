#!/usr/bin/env bash
#
# This file is part of PerconaFT.
# Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.
#

set -e

test $# -ge 2

bin=$1; shift
abortcode=$1; shift

num_writes=$($bin -q)
set +e
for (( i = 0; i < $num_writes; i++ ))
do
    $bin -C $i
    test $? -eq $abortcode || exit 1
done
