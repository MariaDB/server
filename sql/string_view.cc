/*****************************************************************************
Copyright (c) 2021 MariaDB Corporation.
This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
*****************************************************************************/

#include "string_view.h"

#include <ostream>

std::basic_ostream<char> &operator<<(std::basic_ostream<char> &os,
                                     string_view v)
{
  // TODO standard requires a much more complicated code here.
  auto size= static_cast<std::streamsize>(v.size());
  os.write(v.data(), size);
  return os;
}
