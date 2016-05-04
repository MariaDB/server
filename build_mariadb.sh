#!/usr/bin/ksh

if [ "$1" = "-c" ] ; then
	DO_CMAKE=1
fi



export MALLOCALIGN=64

export LANG=EN_US
export APPATH="/usr/local/itsvbuild/64"
export INSTPATH="/usr/local/mariadb"
export CC=xlc_r
export CXX=xlC_r
export OBJECT_MODE=64
export CFLAGS="-q64 -qmaxmem=-1 -qcpluscmt -DNDEBUG -DSYSV -D_AIX -D_AIX64 -D_AIX51 -D_AIX61 -D_AIX71 -DDBUG_OFF -D_ALL_SOURCE -DUNIX -O2"
export CFLAGS="$CFLAGS -qarch=pwr7 -qtune=pwr7 -qcache=auto"
# export CFLAGS="$CFLAGS -qalloca"
export CFLAGS="$CFLAGS -Dalloca=__alloca -D_H_ALLOCA"
export CFLAGS="$CFLAGS -qdirectstorage"
export CFLAGS="$CFLAGS -qstaticinline"
export CFLAGS="$CFLAGS -qnoproto"
export CFLAGS="$CFLAGS -qobjmodel=ibm"
export CFLAGS="$CFLAGS -qnokeepinlines"

# export CFLAGS="$CFLAGS -DFUNCPROTO=15"
# export CFLAGS="$CFLAGS -DNO_CPLUSPLUS_ALLOCA"
# export CFLAGS="$CFLAGS -qnamemangling=v5"
# export CFLAGS="$CFLAGS -qnoguards"
# export CFLAGS="$CFLAGS -qnoroconst"
# export CFLAGS="$CFLAGS -qlanglvl=extended0x"

export CXXFLAGS="$CFLAGS"
export CXXFLAGS="$CXXFLAGS -qlanglvl=extended0x"
# export CXXFLAGS="$CXXFLAGS -qlanglvl=compatrvaluebinding"
export CXXFLAGS="$CXXFLAGS -qnoweaksymbol"

export LDFLAGS="-L$APPATH/lib -L/opt/freeware/lib64 -L/opt/freeware/lib -Wl,-blibpath:$INSTPATH/lib:$APPATH/lib:/usr/lib:/lib -Wl,-b64 -Wl,-bexpall -Wl,-bexpfull -Wl,-bnoipath -Wl,-bbigtoc -Wl,-lxlsmp"
# export LDFLAGS="$LDFLAGS -bmaxdata:0x80000000000"

export PATH="$APPATH/bin:$PATH"
export LIBPATH="$APPATH/lib:/usr/lib:/lib:/opt/freeware/lib64:/opt/freeware/lib"
export LD_LIBRARY_PATH="$APPATH/lib:/opt/freeware/lib64:/opt/freeware/lib:/usr/lib:/lib"


export CMAKE_ARGS="-DCMAKE_INSTALL_PREFIX=$INSTPATH -DWITH_UNIT_TESTS=OFF -DWITH_JEMALLOC=NO -DWITH_EXTRA_CHARSETS=complex -DDEFAULT_CHARSET=utf8 -DDEFAULT_COLLATION=utf8_general_ci -DWITH_SSL=bundled -DWITH_WSREP=OFF " 


if [ "$DO_CMAKE" = "1" ] ; then
	make clean
	sh BUILD/cleanup
	/bin/rm -f CMakeCache.txt
	find $PWD -name CMakeFiles -exec rm -rf {} \; 2>/dev/null
	find $PWD -name CMakeCache.txt -exec rm -rf {} \; 2>/dev/null
#	cmake . $CMAKE_ARGS || exit 1
#	/usr/local/src/walzjo/cmake-3.4.3/bin/cmake . $CMAKE_ARGS || exit 1
	/home/walzjo/source/cmake-3.5.2/bin/cmake . $CMAKE_ARGS || exit 1
fi


# gmake
