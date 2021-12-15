# Copyright (c) 2017 Percona LLC and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

PROJECT(crc C)

IF(NOT CMAKE_CROSSCOMPILING AND NOT MSVC)
  STRING(TOLOWER ${CMAKE_SYSTEM_PROCESSOR}  processor)
  IF(processor MATCHES "86" OR processor MATCHES "amd64" OR processor MATCHES "x64")
  # Check for PCLMUL instruction
  CHECK_C_SOURCE_RUNS("
  int main()
  {
    asm volatile (\"pclmulqdq \\$0x00, %%xmm1, %%xmm0\":::\"cc\");
    return 0;
  }"  HAVE_CLMUL_INSTRUCTION)
  ENDIF()
ENDIF()
IF(HAVE_CLMUL_INSTRUCTION)
  ADD_DEFINITIONS(-DHAVE_CLMUL_INSTRUCTION)
ENDIF()
ADD_LIBRARY(crc STATIC crc_glue.c crc-intel-pclmul.c)
