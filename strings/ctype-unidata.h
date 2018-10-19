#ifndef CTYPE_UNIDATA_H_INCLUDED
#define CTYPE_UNIDATA_H_INCLUDED
/*
  Copyright (c) 2018 MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#define MY_UNICASE_INFO_DEFAULT_MAXCHAR 0xFFFF
extern MY_UNICASE_CHARACTER my_unicase_default_page00[256];
extern MY_UNICASE_CHARACTER *my_unicase_default_pages[256];

size_t my_strxfrm_pad_nweights_unicode(uchar *str, uchar *strend, size_t nweights);
size_t my_strxfrm_pad_unicode(uchar *str, uchar *strend);


#define PUT_WC_BE2_HAVE_1BYTE(dst, de, wc) \
  do { *dst++= (uchar) (wc >> 8); if (dst < de) *dst++= (uchar) (wc & 0xFF); } while(0)

#endif /* CTYPE_UNIDATA_H_INCLUDED */
