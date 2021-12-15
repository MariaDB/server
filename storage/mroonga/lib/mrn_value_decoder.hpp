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

#ifndef MRN_VALUE_DECODER_HPP_
#define MRN_VALUE_DECODER_HPP_

#include <mrn_mysql.h>

namespace mrn {
  namespace value_decoder {
    void decode(uint16 *dest, const uchar *source);
    void decode(float *dest, const uchar *source);
    void decode(double *dest, const uchar *source);
    void decode(long long int *dest, const uchar *source);
  }
}

#endif // MRN_VALUE_DECODER_HPP_
