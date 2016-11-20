#!/bin/bash
warn_path=$1
warn_mode=$2
shift 2

warn_file="$warn_path/compile.warnings"
suppress_file="$warn_path/suppress.warnings"

exec 3>&1
cmderr=$("$@" 2>&1 1>&3)
error=$?

if [[ -n "$cmderr" ]]; then
    [[ "$warn_mode" == "both" ]] &&
        echo "$cmderr" >&2
    [[ "$cmderr" =~ warning:(.+)$ ]] &&
        echo -n "$cmderr" >> "$warn_file"
fi

exit ${error}
