#!/usr/bin/env bash
#
# This file is part of PerconaFT.
# Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.
#

if [[ $# -ne 1 ]]; then exit 1; fi

bin=$1; shift

set -e

$bin -a -thread_stack 16384
$bin -a -thread_stack 16384 -resume
