# Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 


# This will never work with AIX, so switching off per default
option(WITH_UNIT_TESTS "WITH_UNIT_TESTS" OFF)
option(WITH_JEMALLOC "WITH_JEMALLOC" NO)


#Enable 64 bit file offsets
SET(_LARGE_FILES 1)


# Compiler/Linker Flags 
# "-qarch=pwr7 -qtune=pwr7" is essential,so customize this value to your target cpu (power 6/7/8/whatever)
#  or you will have a very bad time with innodb.

SET(CMAKE_CC_FLAGS "-q64 -qlanglvl=extended0x -qmaxmem=-1 -qstaticinline -qcpluscmt -qarch=pwr7 -qtune=pwr7 -Dalloca=__alloca -D_H_ALLOCA -DNDEBUG -DSYSV -D_AIX -D_AIX64 -D_AIX61 -D_AIX71 -D_ALL_SOURCE \
-DUNIX -DFUNCPROTO=15 -O2")
SET(CMAKE_CXX_FLAGS "-q64 -qlanglvl=extended0x -qmaxmem=-1 -qstaticinline -qcpluscmt -qarch=pwr7 -qtune=pwr7 -Dalloca=__alloca -D_H_ALLOCA -DNDEBUG -DSYSV -D_AIX -D_AIX64 -D_AIX61 -D_AIX71 -D_ALL_SOURCE \
-DUNIX -DFUNCPROTO=15 -O2")

SET(CMAKE_EXE_LINKER_FLAGS "-Wl,-b64 -Wl,-bexpall -Wl,-bexpfull -Wl,-bnoipath -Wl,-bbigtoc")
SET(CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "-Wl,-b64 -Wl,-bexpall -Wl,-bexpfull -Wl,-bnoipath -Wl,-bbigtoc")
SET(CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "-Wl,-b64 -Wl,-bexpall -Wl,-bexpfull -Wl,-bnoipath -Wl,-bbigtoc")
