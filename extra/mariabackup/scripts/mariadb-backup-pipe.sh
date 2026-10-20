#!/bin/sh
# Stream helper for BACKUP SERVER WITH 'pipe'.
# The server runs this as "mariadb-backup-pipe <stream>" with
# that stream's tar on our standard input, and looks it up
# in the PATH of the mariadbd process. We hand the tar to a
# named pipe that the caller created and is already draining,
# so the backup never lands on disk here.
#
# The caller (mariadb-backup-server --stream) must
# mkfifo .mariadb-backup-pipe-<stream>.tar in the server @@datadir,
# start a reader on it, and remove it afterwards.
# Relative paths resolve there because @@datadir is the working
# directory of the mariadbd process.
#
# One fifo per stream, so this also works for
# BACKUP SERVER WITH N CONCURRENT
# where a driver gives every stream its own reader.
#
# Note that opening a fifo for writing blocks
# until a reader opens it, so a caller that creates the fifo
# but never reads it will hang the backup while
# it holds backup locks.

set -e

fifo=.mariadb-backup-pipe-$1.tar

# Refuse to run unless the caller really did create the fifo:
# without this the redirection below would create a regular
# file and quietly write the
# whole backup into the server data directory.
test -p "$fifo"

exec cat > "$fifo"
