#!/bin/sh
# mbstream-compatible for the BACKUP SERVER wrapper.
#
# This maps the mbstream CLI to `tar`:
#
#   mbstream -x -C <dir>      < archive      ->  tar -x -C <dir>
#   mbstream -c <paths...>    > archive      ->  tar -c <paths...>
#
# mbstream-only options are accepted and ignored so
# existing invocations keep working unchanged.
me=${0##*/}
die() {
    echo "$me: $*" >&2
    exit 1
}

# The tar implementation can be overridden for testing.
# e.g. TAR=bsdtar.
: "${TAR:=tar}"

mode=
dir=.
files=

while [ $# -gt 0 ]; do
    case $1 in
    -x|--extract)            mode=x ;;
    -c|--create)             mode=c ;;
    -C|--directory)          dir=$2; shift ;;
    -C*)                     dir=${1#-C} ;;
    --directory=*)           dir=${1#*=} ;;

    # mbstream-only flags that have no tar equivalent: drop them.
    -p|--parallel)           shift ;;
    -p*|--parallel=*)        ;;
    --decompress|--compress) ;;
    -v|--verbose)            ;;

    --)                      shift; break ;;
    # Reject anything we do not recognise rather than
    # silently ignoring it (e.g. GNU tar's -b takes an argument
    # that would otherwise be mistaken for a file operand).
    # This is an mbstream-on-tar shim, not tar.
    -*)                      die "unsupported option: $1" ;;
    *)                       files="$files $1" ;;
    esac
    shift
done

while [ $# -gt 0 ]; do
    files="$files $1"
    shift
done

case $mode in
    # The wrapper concatenates the per-stream tar entries
    # with no end-of-archive marker between them; only the
    # trailing backup-prepare.cnf adds the single end marker.
    x) exec $TAR -x -f - -C "$dir" ;;
    c)
        [ -n "$files" ] || files=.
        exec $TAR -c -f - -C "$dir" $files ;;
    *) die "expected -x (extract) or -c (create)" ;;
esac
