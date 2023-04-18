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

extern MY_UNICASE_CHARACTER my_unicase_mysql500_page00[256];
extern MY_UNICASE_CHARACTER *my_unicase_mysql500_pages[256];


static inline my_wc_t my_u300_tolower_7bit(uchar ch)
{
  return my_unicase_default_page00[ch].tolower;
}

static inline my_wc_t my_u300_toupper_7bit(uchar ch)
{
  return my_unicase_default_page00[ch].toupper;
}


static inline void my_tosort_unicode_bmp(MY_UNICASE_INFO *uni_plane,
                                         my_wc_t *wc)
{
  const MY_UNICASE_CHARACTER *page;
  DBUG_ASSERT(*wc <= uni_plane->maxchar);
  if ((page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].sort;
}


static inline void my_tosort_unicode(MY_UNICASE_INFO *uni_plane,
                                     my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    const MY_UNICASE_CHARACTER *page;
    if ((page= uni_plane->page[*wc >> 8]))
      *wc= page[*wc & 0xFF].sort;
  }
  else
  {
    *wc= MY_CS_REPLACEMENT_CHARACTER;
  }
}


static inline void
my_tolower_unicode_bmp(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  const MY_UNICASE_CHARACTER *page;
  DBUG_ASSERT(*wc <= uni_plane->maxchar);
  if ((page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].tolower;
}


static inline void
my_toupper_unicode_bmp(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  const MY_UNICASE_CHARACTER *page;
  DBUG_ASSERT(*wc <= uni_plane->maxchar);
  if ((page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].toupper;
}


static inline void
my_tolower_unicode(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    const MY_UNICASE_CHARACTER *page;
    if ((page= uni_plane->page[(*wc >> 8)]))
      *wc= page[*wc & 0xFF].tolower;
  }
}


static inline void
my_toupper_unicode(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    const MY_UNICASE_CHARACTER *page;
    if ((page= uni_plane->page[(*wc >> 8)]))
      *wc= page[*wc & 0xFF].toupper;
  }
}


size_t my_strxfrm_pad_nweights_unicode(uchar *str, uchar *strend, size_t nweights);
size_t my_strxfrm_pad_unicode(uchar *str, uchar *strend);


#define PUT_WC_BE2_HAVE_1BYTE(dst, de, wc) \
  do { *dst++= (uchar) (wc >> 8); if (dst < de) *dst++= (uchar) (wc & 0xFF); } while(0)

#endif /* CTYPE_UNIDATA_H_INCLUDED */
