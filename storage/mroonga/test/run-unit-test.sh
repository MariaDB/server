#!/bin/sh

export BASE_DIR="`dirname $0`"
top_dir="$BASE_DIR/.."

if test -z "$NO_MAKE"; then
    MAKE_ARGS=
    case `uname` in
	Linux)
	    MAKE_ARGS="-j$(grep '^processor' /proc/cpuinfo | wc -l)"
	    ;;
	Darwin)
	    MAKE_ARGS="-j$(/usr/sbin/sysctl -n hw.ncpu)"
	    ;;
	*)
	    :
	    ;;
    esac
    make $MAKE_ARGS -C $top_dir > /dev/null || exit 1
fi

if test -z "$CUTTER"; then
    CUTTER="`make -s -C $top_dir echo-cutter`"
fi
export CUTTER

CUTTER_ARGS=
CUTTER_WRAPPER=
if test x"$STOP" = x"yes"; then
    CUTTER_ARGS="-v v --fatal-failures"
else
    CUTTER_ARGS="-v v"
fi

if test x"$CUTTER_DEBUG" = x"yes"; then
    if test x"$TUI_DEBUG" = x"yes"; then
        CUTTER_WRAPPER="$top_dir/libtool --mode=execute gdb --tui --args"
    else
        CUTTER_WRAPPER="$top_dir/libtool --mode=execute gdb --args"
    fi
    CUTTER_ARGS="--keep-opening-modules"
elif test x"$CUTTER_CHECK_LEAK" = x"yes"; then
    CUTTER_WRAPPER="$top_dir/libtool --mode=execute valgrind "
    CUTTER_WRAPPER="$CUTTER_WRAPPER --leak-check=full --show-reachable=yes -v"
    CUTTER_ARGS="--keep-opening-modules"
fi

CUTTER_ARGS="$CUTTER_ARGS -s $BASE_DIR"
$CUTTER_WRAPPER $CUTTER $CUTTER_ARGS "$@" $BASE_DIR
