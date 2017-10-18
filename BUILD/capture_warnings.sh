#!/bin/bash
warn_path=$1
warn_mode=$2 # 'late', 'early' or 'both'
shift 2

warn_file="$warn_path/compile.warnings"
suppress_file="$warn_path/suppress.warnings"

# suppress_warnings:
#
# 1. treat STDIN as sequence of warnings (w) delimited by ^~~~... lines
#
# 2. sanitize each w to matchable m:
#    a. ignore 'In file included from' lines;
#    b. protect BRE chars;
#    c. ignore text coords (NNN:NNN);
#    d. convert multiline to X-delimited single line.
#
# 3. match sanitized m against X-delimited suppress.warnings:
#    if match not found print w to STDOUT.

suppress_warnings()
{
    [ -f "$suppress_file" ] || {
        cat
        return
    }
    [ -z "$suppress_file" ] && {
        cat > /dev/null
        return
    }
    local m w from
    IFS=""
    while read -r l
    do
        w="$w$l"$'\n'

        [[ $l =~ ^"In file included from " ]] && {
            from=1
            continue
        }

        [[ $from && $l =~ ^[[:space:]]+"from " ]] &&
            continue

        unset from

        if [[ $l =~ ^[[:space:]]*~*\^~*$ ]]
        then
            cat "$suppress_file" | tr '\n' 'X' | /bin/grep -Gq "$m" ||
                echo "$w"
            unset m w
        else
            # Protect BRE metacharacters \.[*^$
            l=${l//\\/\\\\}
            l=${l//./\\.}
            l=${l//[/\\[}
            l=${l//-/\\-}
            l=${l//\*/\\*}
            l=${l//^/\\^}
            l=${l//\$/\\\$}
            # replace text coords line:char with BRE wildcard
            [[ $l =~ ^(.*:)[[:digit:]]+:[[:digit:]]+(.*)$ ]] &&
                l=${BASH_REMATCH[1]}[[:digit:]]\\+:[[:digit:]]\\+${BASH_REMATCH[2]}
            m="$m$l"$'X'
        fi
    done
}

exec 3>&1
"$@" 2>&1 1>&3 | suppress_warnings | (
    cmderr=`cat`

    if [[ -n "$cmderr" ]]; then
        if [[ "$cmderr" =~ error: ]]; then
            echo "$cmderr" >&2
            exit
        fi
        [[ "$warn_mode" != "late" ]] &&
            echo "$cmderr" >&2
        [[ "$warn_mode" != "early" && "$cmderr" =~ (warning|note): ]] &&
            echo "$cmderr" >> "$warn_file"
    fi
)

exit ${PIPESTATUS}
