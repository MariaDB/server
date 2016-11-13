#!/bin/bash

warn_path=$1
shift
warn_file="$warn_path/compile.warnings"

set -o pipefail
exec 3>&1
x=$(("$@" >&2) 2>&1 1>&3)
error=${PIPESTATUS}

if ! [[ -z "$x" ]]; then
    echo -n "$x" >> $warn_file
    echo -n "$x" >&2
fi

exit ${error}
