#!/bin/bash
#  Copyright (c) 2014, 2015, Oracle and/or its affiliates. All rights reserved.
#
#  This program is free software; you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation; version 2 of the License.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, write to the Free Software
#  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

OS=`uname`
SYSDIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
GENDIR=$SYSDIR/gen
PWD=$( pwd )

# Grab the current sys version
SYSVERSIONTMP=`cat views/version.sql | grep 'AS sys_version' | awk '{print $2}'`
SYSVERSION=`echo "${SYSVERSIONTMP//\'}"`

MYSQLUSER="'root'@'localhost'"

if [ $OS == "Darwin" ] ;
then
  SED_R="sed -E"
else
  SED_R="sed -r"
fi

USAGE="
Options:
================

    v: The version of MySQL to build the sys schema for, either '56' or '57'

    b: Whether to omit any lines that deal with sql_log_bin (useful for RDS)

    m: Whether to generate a mysql_install_db / mysqld --initialize formatted file

    u: The user to set as the owner of the objects (useful for RDS)

Examples:
================

Generate a MySQL 5.7 SQL file that uses the 'mark'@'localhost' user:

    $0 -v 57 -u \"'mark'@'localhost'\"

Generate a MySQL 5.6 SQL file for RDS:

    $0 -v 56 -b -u CURRENT_USER

Generate a MySQL 5.7 initialize / bootstrap file:

    $0 -v 57 -m
"

# Grab options
while getopts ":v:bhmu:" opt; do
  case $opt in
    b)
      SKIPBINLOG=true
      ;;
    h)
      echo $"$USAGE"
      exit 0
      ;;
    m)
      MYSQLCOMPAT=true
      # Bundled mysql expects the mysql.sys user to be used
      MYSQLUSER="'mysql.sys'@'localhost'"
      ;;
    u)
      if [ -z "$MYSQLCOMPAT" ] ;
      then
        MYSQLUSER="${OPTARG}"
      fi
      ;;
    v)
      if [ $OPTARG == "56" ] || [ $OPTARG == "57" ] ;
      then
        MYSQLVERSION=$OPTARG
      else
      	echo "Invalid -v option, please run again with either '-v 56' or '-v 57'"
      	exit 1
      fi
      ;;
    \?)
      echo "Invalid option: -$OPTARG" >&2
      echo $"$USAGE"
      exit 1
      ;;
    :)
      echo "Option -$OPTARG requires an argument." >&2
      echo $"$USAGE"
      exit 1
      ;;
  esac
done

# Check required options
if [[ -z "$MYSQLVERSION" ]] ;
then
  echo "  -v (MySQL Version) parameter required, please run again with either '-v 56' or '-v 57'"
  echo $"$USAGE"
  exit 1
fi

# Create the gen directory
if [[ ! -d $GENDIR ]] ;
then
  mkdir $GENDIR
fi

# Create output file name
if [[ ! -z "$MYSQLCOMPAT" ]] ;
then
  OUTPUTFILE="mysql_sys_schema.sql"
else
  OUTPUTFILE="sys_${SYSVERSION}_${MYSQLVERSION}_inline.sql"
fi

# Create the initial output file
if [[ ! -z "$MYSQLCOMPAT" ]] ;
then
  # Pre-process all the files in a copied temp directory
  if [[ ! -d $GENDIR/tmpgen ]] ;
  then
    mkdir $GENDIR/tmpgen
  fi
  cd $GENDIR/tmpgen
  rm -rf *
  cp -r $SYSDIR/after_setup.sql $SYSDIR/tables $SYSDIR/triggers $SYSDIR/functions $SYSDIR/views $SYSDIR/procedures .

  # Switch user if requested
  # Remove individual copyrights
  # Replace newlines in COMMENTs with literal \n
  # Drop added trailing \n <sad panda>
  # Remove DELIMITER commands
  # Replace $$ delimiter with ;
  # Remove leading spaces
  # Remove -- line comments *after removing leading spaces*
  for file in `find . -name '*.sql'`; do
    # The 5.6 MEM/WB integration should still use the root@localhost user
    if [ ! $MYSQLVERSION == "56" ] ;
    then
      sed -i -e "s/'root'@'localhost'/$MYSQLUSER/g" $file
    fi
    sed -i -e "/Copyright/,/51 Franklin St/d" $file
    sed -i -e "/^ *COMMENT/,/^ *'/{G;s/\n/\\\n/g;}" $file
    sed -i -e "s/            '\\\n/            '/g" $file
    sed -i -e "/^DELIMITER/d" $file
    sed -i -e "s/\\$\\$/;/g" $file
    sed -i -e "s/^ *//g" $file
    sed -i -e "/^--/d" $file
  done

  # Start the output file from a non-removed copyright file
  sed -e "s/^/-- /" $SYSDIR/LICENSE > $OUTPUTFILE
  echo "" >> $OUTPUTFILE
  echo "--" >> $OUTPUTFILE
  echo "-- WARNING: THIS IS A GENERATED FILE, CHANGES NEED TO BE MADE ON THE UPSTREAM MYSQL-SYS REPOSITORY AS WELL" >> $OUTPUTFILE
  echo "-- PLEASE SUBMIT A PULL REQUEST TO https://github.com/mysql/mysql-sys" >> $OUTPUTFILE
  echo "--" >> $OUTPUTFILE
  echo "" >> $OUTPUTFILE

  # Add the expected user
  # Note this currently only works with 5.7 mysql.user structure
  if [ ! $MYSQLVERSION == "56" ] ;
  then
    echo "REPLACE INTO mysql.user VALUES ('localhost','mysql.sys','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','','','','',0,0,0,0,'mysql_native_password','*THISISNOTAVALIDPASSWORDTHATCANBEUSEDHERE','N',CURRENT_TIMESTAMP,NULL,'Y');" >> $OUTPUTFILE
    echo "" >> $OUTPUTFILE
    echo "REPLACE INTO mysql.db VALUES ('localhost','sys','mysql.sys','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','N','Y');" >> $OUTPUTFILE
    echo "" >> $OUTPUTFILE
    echo "REPLACE INTO mysql.tables_priv VALUES ('localhost','sys','mysql.sys','sys_config','root@localhost', CURRENT_TIMESTAMP, 'Select', '');" >> $OUTPUTFILE
    echo "" >> $OUTPUTFILE
    echo "FLUSH PRIVILEGES;" >> $OUTPUTFILE
    echo "" >> $OUTPUTFILE
  fi

  # Put in the contents of before_setup.sql, though don't collapse lines
  sed -e "/sql_log_bin/d;s/'root'@'localhost'/$MYSQLUSER/g;/Copyright/,/51 Franklin St/d" $SYSDIR/before_setup.sql >> $OUTPUTFILE

  # Add the rest of the files in install file order, removing new lines along the way
  cat "$SYSDIR/sys_$MYSQLVERSION.sql" | tr -d '\r' | grep 'SOURCE' | grep -v before_setup | grep -v after_setup | $SED_R 's .{8}  ' | sed 's/^/./' >  "./sys_$MYSQLVERSION.sql"
  while read file; do
      # First try and get a DROP command
      grep -E '(^DROP PROCEDURE|^DROP FUNCTION|^DROP TRIGGER)' $file >> $OUTPUTFILE
      # And remove any that may exist (but keep DROP TEMPORARY TABLE)
      sed -i -e "/^DROP PROCEDURE/d;/^DROP FUNCTION/d;/^DROP TRIGGER/d" $file
      echo "" >> $OUTPUTFILE
      # Then collapse the rest of the file
      cat $file | tr '\n' ' ' >> $OUTPUTFILE
      echo "" >> $OUTPUTFILE
      echo "" >> $OUTPUTFILE
  done < "./sys_$MYSQLVERSION.sql"

  # Does essentially nothing right now, but may in future
  sed -e "/Copyright/,/51 Franklin St/d;/sql_log_bin/d" $SYSDIR/after_setup.sql >> $OUTPUTFILE

  # Remove final leading and trailing spaces
  sed -i '' -e "s/^ *//g" $OUTPUTFILE
  sed -i '' -e "s/[ \t]*$//g" $OUTPUTFILE
  # Remove more than one empty line
  sed -i '' -e "/^$/N;/^\n$/D" $OUTPUTFILE

  mv $OUTPUTFILE $GENDIR/
  cd $GENDIR/
  rm -rf $GENDIR/tmpgen
else
  sed -e "s/^/-- /" $SYSDIR/LICENSE > $GENDIR/$OUTPUTFILE
  cat "$SYSDIR/sys_$MYSQLVERSION.sql" | tr -d '\r' | grep 'SOURCE' | $SED_R 's .{8}  ' | sed "s/^/$(echo $SYSDIR | sed -e 's/[]\/$*.^|[]/\\&/g')/g" \
    | xargs sed -e "/Copyright/,/51 Franklin St/d;s/'root'@'localhost'/$MYSQLUSER/g" >> $GENDIR/$OUTPUTFILE
fi

# Check if sql_log_bin lines should be removed
if [[ ! -z "$SKIPBINLOG" ]] ;
then
  LOGWARNING="WARNING: Using a routine that could cause binary log events, but disabling of binary logging has been removed"
  sed -i '' -e "s/^[ \s]*SET sql_log_bin = 0/SELECT \"$LOGWARNING\"/g" $GENDIR/$OUTPUTFILE
  sed -i '' -e "s/\sSET sql_log_bin = @log_bin;/SET @log_bin = NULL;/g" $GENDIR/$OUTPUTFILE
  SKIPBINLOG="disabled"
else
  SKIPBINLOG="enabled"
fi

cd $PWD

# Print summary
echo $"
    Wrote file: $GENDIR/$OUTPUTFILE
Object Definer: $MYSQLUSER
   sql_log_bin: $SKIPBINLOG
 "
