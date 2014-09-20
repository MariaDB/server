#!/bin/sh

case "$0" in
    */*)
	cd `dirname $0`
	;;
esac

GRN_VERSION_SH=version.sh

if test -f version
then
    GRN_VN=`cat version`
elif test -d .git -o -f .git
then
    GRN_VN=`git describe --abbrev=7 HEAD 2>/dev/null`
    if [ $? -ne 0 ]
    then
	GRN_VN=`cat base_version`
    fi
fi

GRN_VN=`expr "$GRN_VN" : v*'\(.*\)'`

if test -r $GRN_VERSION_SH
then
    GRN_VN_OLD=`sed -e 's/^GRN_VERSION=//' <$GRN_VERSION_SH`
else
    GRN_VN_OLD=unset
fi

if test "$GRN_VN_OLD" != "$GRN_VN"
then
    echo "GRN_VERSION=$GRN_VN" >$GRN_VERSION_SH
fi
