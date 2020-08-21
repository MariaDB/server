/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013 Kouhei Sutou <kou@clear-code.com>
  Copyright(C) 2011-2013 Kentoku SHIBA

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

#include <mrn_err.h>
#include "mrn_encoding.hpp"

namespace mrn {
  namespace encoding {
    CHARSET_INFO *mrn_charset_utf8 = NULL;
    CHARSET_INFO *mrn_charset_utf8mb4 = NULL;
    CHARSET_INFO *mrn_charset_binary = NULL;
    CHARSET_INFO *mrn_charset_ascii = NULL;
    CHARSET_INFO *mrn_charset_latin1_1 = NULL;
    CHARSET_INFO *mrn_charset_latin1_2 = NULL;
    CHARSET_INFO *mrn_charset_cp932 = NULL;
    CHARSET_INFO *mrn_charset_sjis = NULL;
    CHARSET_INFO *mrn_charset_eucjpms = NULL;
    CHARSET_INFO *mrn_charset_ujis = NULL;
    CHARSET_INFO *mrn_charset_koi8r = NULL;

    void init(void) {
      CHARSET_INFO **cs;
      MRN_DBUG_ENTER_FUNCTION();
      for (cs = all_charsets; cs < all_charsets + MY_ALL_CHARSETS_SIZE; cs++)
      {
        if (!cs[0])
          continue;
        if (!strcmp(cs[0]->cs_name.str, "utf8"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_utf8)
            mrn_charset_utf8 = cs[0];
          else if (mrn_charset_utf8->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "utf8mb4"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_utf8mb4)
            mrn_charset_utf8mb4 = cs[0];
          else if (mrn_charset_utf8mb4->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "binary"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_binary)
            mrn_charset_binary = cs[0];
          else if (mrn_charset_binary->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "ascii"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_ascii)
            mrn_charset_ascii = cs[0];
          else if (mrn_charset_ascii->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "latin1"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_latin1_1)
            mrn_charset_latin1_1 = cs[0];
          else if (mrn_charset_latin1_1->cset != cs[0]->cset)
          {
            if (!mrn_charset_latin1_2)
              mrn_charset_latin1_2 = cs[0];
            else if (mrn_charset_latin1_2->cset != cs[0]->cset)
              DBUG_ASSERT(0);
          }
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "cp932"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_cp932)
            mrn_charset_cp932 = cs[0];
          else if (mrn_charset_cp932->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "sjis"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_sjis)
            mrn_charset_sjis = cs[0];
          else if (mrn_charset_sjis->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "eucjpms"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_eucjpms)
            mrn_charset_eucjpms = cs[0];
          else if (mrn_charset_eucjpms->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "ujis"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_ujis)
            mrn_charset_ujis = cs[0];
          else if (mrn_charset_ujis->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        if (!strcmp(cs[0]->cs_name.str, "koi8r"))
        {
          DBUG_PRINT("info", ("mroonga: %s is %s [%p]",
                              cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
          if (!mrn_charset_koi8r)
            mrn_charset_koi8r = cs[0];
          else if (mrn_charset_koi8r->cset != cs[0]->cset)
            DBUG_ASSERT(0);
          continue;
        }
        DBUG_PRINT("info", ("mroonga: %s[%s][%p] is not supported",
                            cs[0]->col_name.str, cs[0]->cs_name.str, cs[0]->cset));
      }
      DBUG_VOID_RETURN;
    }

    int set(grn_ctx *ctx, const CHARSET_INFO *charset) {
      MRN_DBUG_ENTER_FUNCTION();
      int error = 0;

      if (!set_raw(ctx, charset)) {
        const char *name = "<null>";
        const char *csname = "<null>";
        if (charset) {
          name = charset->col_name.str;
          csname = charset->cs_name.str;
        }
        error = ER_MRN_CHARSET_NOT_SUPPORT_NUM;
        my_printf_error(error,
                        ER_MRN_CHARSET_NOT_SUPPORT_STR,
                        MYF(0), name, csname);
      }

      DBUG_RETURN(error);
    }

    bool set_raw(grn_ctx *ctx, const CHARSET_INFO *charset) {
      MRN_DBUG_ENTER_FUNCTION();
      if (!charset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_NONE);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_utf8->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_UTF8);
        DBUG_RETURN(true);
      }
      if (mrn_charset_utf8mb4 && charset->cset == mrn_charset_utf8mb4->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_UTF8);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_cp932->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_SJIS);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_eucjpms->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_EUC_JP);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_latin1_1->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_LATIN1);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_latin1_2->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_LATIN1);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_koi8r->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_KOI8R);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_binary->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_NONE);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_ascii->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_UTF8);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_sjis->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_SJIS);
        DBUG_RETURN(true);
      }
      if (charset->cset == mrn_charset_ujis->cset)
      {
        GRN_CTX_SET_ENCODING(ctx, GRN_ENC_EUC_JP);
        DBUG_RETURN(true);
      }
      GRN_CTX_SET_ENCODING(ctx, GRN_ENC_NONE);
      DBUG_RETURN(false);
    }
  }
}
