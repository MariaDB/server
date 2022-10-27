#!/usr/bin/env bash
#
# This file is part of PerconaFT.
# Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.
#


if [[ $# -ne 1 ]]; then exit 1; fi

bin=$1; shift

if $bin --test
then
    echo $bin --test did not crash
    exit 1
else
    set -e
    $bin --recover
fi
