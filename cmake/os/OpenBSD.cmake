# Copyright (C) 2012 Monty Program Ab, 2021 Brad Smith
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA 

# This file includes OpenBSD specific options and quirks, related to system checks

# Find libexecinfo (library that contains backtrace_symbols etc)
FIND_LIBRARY(EXECINFO NAMES execinfo)
IF(EXECINFO)
 SET(LIBEXECINFO ${EXECINFO})
ENDIF()
