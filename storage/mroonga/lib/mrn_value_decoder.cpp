/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "mrn_value_decoder.hpp"

#if MYSQL_VERSION_ID >= 50706 && !defined(MRN_MARIADB_P)
#  define MRN_DEST_IS_POINTER
#endif

namespace mrn {
  namespace value_decoder {
    void decode(uint16 *dest, const uchar *source) {
      MRN_DBUG_ENTER_FUNCTION();
#ifdef MRN_DEST_IS_POINTER
      ushortget(dest, source);
#else
      uint16 value;
      ushortget(value, source);
      *dest = value;
#endif
      DBUG_VOID_RETURN;
    };

    void decode(float *dest, const uchar *source) {
      MRN_DBUG_ENTER_FUNCTION();
#ifdef MRN_DEST_IS_POINTER
      float4get(dest, source);
#else
      float value;
      float4get(value, source);
      *dest = value;
#endif
      DBUG_VOID_RETURN;
    };

    void decode(double *dest, const uchar *source) {
      MRN_DBUG_ENTER_FUNCTION();
#ifdef MRN_DEST_IS_POINTER
      float8get(dest, source);
#else
      double value;
      float8get(value, source);
      *dest = value;
#endif
      DBUG_VOID_RETURN;
    }
    void decode(long long int *dest, const uchar *source) {
      MRN_DBUG_ENTER_FUNCTION();
#ifdef MRN_DEST_IS_POINTER
      longlongget(dest, source);
#else
      long long int value;
      longlongget(value, source);
      *dest = value;
#endif
      DBUG_VOID_RETURN;
    }
  }
}
