#!/bin/sh
#
# This tests is good to find bugs in the redo/undo handling and in
# finding bugs in blob handling
#

mkdir -p tmp
cd tmp
set -e
a=15
while test $a -le 5000
do
  echo $a
  rm -f aria_log*
  ../ma_test2 -s -L -K -W -P -M -T -c -b32768 -t4 -A1 -m$a > /dev/null
  ../aria_read_log -a -s >& /dev/null
  ../aria_chk -ess test2
  ../aria_read_log -a -s >& /dev/null
  ../aria_chk -ess test2
  rm test2.MA?
  ../aria_read_log -a -s >& /dev/null
  ../aria_chk -ess test2
  a=$((a+1))
done
cd ..
rm -r tmp
