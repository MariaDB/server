#ifndef CTYPE_UNIDATA_H_INCLUDED
#define CTYPE_UNIDATA_H_INCLUDED
/*
  Copyright (c) 2018, 2023 MariaDB Corporation

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


extern const uint16 weight_general_ci_page00[256];
extern const uint16 *weight_general_ci_index[256];
extern const uint16 weight_general_mysql500_ci_page00[256];
extern const uint16 *weight_general_mysql500_ci_index[256];

extern const MY_CASEFOLD_CHARACTER u300_casefold_page00[256];

static inline my_wc_t my_u300_tolower_7bit(uchar ch)
{
  return u300_casefold_page00[ch].tolower;
}

static inline my_wc_t my_u300_toupper_7bit(uchar ch)
{
  return u300_casefold_page00[ch].toupper;
}


static inline my_wc_t my_general_ci_bmp_char_to_weight(my_wc_t wc)
{
  const uint16 *page;
  DBUG_ASSERT((wc >> 8) < array_elements(weight_general_ci_index));
  page= weight_general_ci_index[wc >> 8];
  return page ? page[wc & 0xFF] : wc;
}


static inline my_wc_t my_general_ci_char_to_weight(my_wc_t wc)
{
  if ((wc >> 8) < array_elements(weight_general_ci_index))
    return my_general_ci_bmp_char_to_weight(wc);
  return MY_CS_REPLACEMENT_CHARACTER;
}


static inline my_wc_t my_general_mysql500_ci_bmp_char_to_weight(my_wc_t wc)
{
  const uint16 *page;
  DBUG_ASSERT((wc >> 8) < array_elements(weight_general_mysql500_ci_index));
  page= weight_general_mysql500_ci_index[wc >> 8];
  return page ? page[wc & 0xFF] : wc;
}


static inline void my_tosort_unicode_bmp(MY_CASEFOLD_INFO *uni_plane,
                                         my_wc_t *wc)
{
  const uint16 *page;
  DBUG_ASSERT(*wc <= uni_plane->maxchar);
  if ((page= uni_plane->simple_weight[*wc >> 8]))
    *wc= page[*wc & 0xFF];
}


static inline void my_tosort_unicode(MY_CASEFOLD_INFO *uni_plane,
                                     my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    const uint16 *page;
    if ((page= uni_plane->simple_weight[*wc >> 8]))
      *wc= page[*wc & 0xFF];
  }
  else
  {
    *wc= MY_CS_REPLACEMENT_CHARACTER;
  }
}


static inline void
my_tolower_unicode_bmp(MY_CASEFOLD_INFO *uni_plane, my_wc_t *wc)
{
  const MY_CASEFOLD_CHARACTER *page;
  DBUG_ASSERT(*wc <= uni_plane->maxchar);
  if ((page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].tolower;
}


static inline void
my_toupper_unicode_bmp(MY_CASEFOLD_INFO *uni_plane, my_wc_t *wc)
{
  const MY_CASEFOLD_CHARACTER *page;
  DBUG_ASSERT(*wc <= uni_plane->maxchar);
  if ((page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].toupper;
}


static inline void
my_tolower_unicode(MY_CASEFOLD_INFO *uni_plane, my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    const MY_CASEFOLD_CHARACTER *page;
    if ((page= uni_plane->page[(*wc >> 8)]))
      *wc= page[*wc & 0xFF].tolower;
  }
}


static inline void
my_toupper_unicode(MY_CASEFOLD_INFO *uni_plane, my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    const MY_CASEFOLD_CHARACTER *page;
    if ((page= uni_plane->page[(*wc >> 8)]))
      *wc= page[*wc & 0xFF].toupper;
  }
}


extern MY_CASEFOLD_INFO my_casefold_default;
extern MY_CASEFOLD_INFO my_casefold_turkish;
extern MY_CASEFOLD_INFO my_casefold_mysql500;
extern MY_CASEFOLD_INFO my_casefold_unicode520;
extern MY_CASEFOLD_INFO my_casefold_unicode1400;
extern MY_CASEFOLD_INFO my_casefold_unicode1400tr;


size_t my_strxfrm_pad_nweights_unicode(uchar *str, uchar *strend, size_t nweights);
size_t my_strxfrm_pad_unicode(uchar *str, uchar *strend);


#define PUT_WC_BE2_HAVE_1BYTE(dst, de, wc) \
  do { *dst++= (uchar) (wc >> 8); if (dst < de) *dst++= (uchar) (wc & 0xFF); } while(0)

#endif /* CTYPE_UNIDATA_H_INCLUDED */
