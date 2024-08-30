# Copyright (c) 2020 IBM
# Use is subject to license terms.
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


# MRI scripts have a problem with +. It's a line contination character
# unfortunately there is no escape character. We know we don't have
# "+" in libraries or the MariaDB paths, but Ubuntu CI builds will have
# in their CI path due to the package names that Ubuntu generates.
# So here we replace the fully expanded paths in the TARGET_SCRIPT,
# strip off the TOP_DIR to make it a relative path to the top level directory
# and then execute AR on the top level directory.

FILE(READ ${TARGET_SCRIPT} SCRIPT_CONTENTS)
STRING(REPLACE "${TOP_DIR}/" "" SCRIPT_CONTENTS_TRIMMED "${SCRIPT_CONTENTS}")
FILE(WRITE "${TARGET_SCRIPT}.mri" ${SCRIPT_CONTENTS_TRIMMED})

EXECUTE_PROCESS(
  WORKING_DIRECTORY ${TOP_DIR}
  COMMAND ${CMAKE_AR} -M
  INPUT_FILE ${TARGET_SCRIPT}.mri
)
