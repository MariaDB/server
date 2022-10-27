/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2014 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/
#include "grn.h"
#include <string.h>
#include <stddef.h>
#include "grn_snip.h"
#include "grn_ctx.h"

#if !defined MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#if !defined MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static int
grn_bm_check_euc(const unsigned char *x, const size_t y)
{
  const unsigned char *p;
  for (p = x + y - 1; p >= x && *p >= 0x80U; p--);
  return (int) ((x + y - p) & 1);
}

static int
grn_bm_check_sjis(const unsigned char *x, const size_t y)
{
  const unsigned char *p;
  for (p = x + y - 1; p >= x; p--)
    if ((*p < 0x81U) || (*p > 0x9fU && *p < 0xe0U) || (*p > 0xfcU))
      break;
  return (int) ((x + y - p) & 1);
}

/*
static void
grn_bm_suffixes(const unsigned char *x, size_t m, size_t *suff)
{
  size_t f, g;
  intptr_t i;
  f = 0;
  suff[m - 1] = m;
  g = m - 1;
  for (i = m - 2; i >= 0; --i) {
    if (i > (intptr_t) g && suff[i + m - 1 - f] < i - g)
      suff[i] = suff[i + m - 1 - f];
    else {
      if (i < (intptr_t) g)
        g = i;
      f = i;
      while (g > 0 && x[g] == x[g + m - 1 - f])
        --g;
      suff[i] = f - g;
    }
  }
}
*/

static void
grn_bm_preBmBc(const unsigned char *x, size_t m, size_t *bmBc)
{
  size_t i;
  for (i = 0; i < ASIZE; ++i) {
    bmBc[i] = m;
  }
  for (i = 0; i < m - 1; ++i) {
    bmBc[(unsigned int) x[i]] = m - (i + 1);
  }
}

#define GRN_BM_COMPARE do { \
  if (string_checks[found]) { \
    size_t offset = cond->last_offset, found_alpha_head = cond->found_alpha_head; \
    /* calc real offset */\
    for (i = cond->last_found; i < found; i++) { \
      if (string_checks[i] > 0) { \
        found_alpha_head = i; \
        offset += string_checks[i]; \
      } \
    } \
    /* if real offset is in a character, move it the head of the character */ \
    if (string_checks[found] < 0) { \
      offset -= string_checks[found_alpha_head]; \
      cond->last_found = found_alpha_head; \
    } else { \
      cond->last_found = found; \
    } \
    cond->start_offset = cond->last_offset = offset; \
    if (flags & GRN_SNIP_SKIP_LEADING_SPACES) { \
      while (cond->start_offset < string_original_length_in_bytes && \
             (i = grn_isspace(string_original + cond->start_offset, \
                              string_encoding))) { cond->start_offset += i; } \
    } \
    for (i = cond->last_found; i < found + m; i++) { \
      if (string_checks[i] > 0) { \
        offset += string_checks[i]; \
      } \
    } \
    cond->end_offset = offset; \
    cond->found = found + shift; \
    cond->found_alpha_head = found_alpha_head; \
    /* printf("bm: cond:%p found:%zd last_found:%zd st_off:%zd ed_off:%zd\n", cond, cond->found,cond->last_found,cond->start_offset,cond->end_offset); */ \
    return; \
  } \
} while (0)

#define GRN_BM_BM_COMPARE do { \
  if (p[-2] == ck) { \
    for (i = 3; i <= m && p[-(intptr_t)i] == cp[-(intptr_t)i]; ++i) { \
    } \
    if (i > m) { \
      found = p - y - m; \
      GRN_BM_COMPARE; \
    } \
  } \
} while (0)

void
grn_bm_tunedbm(grn_ctx *ctx, snip_cond *cond, grn_obj *string, int flags)
{
  register unsigned char *limit, ck;
  register const unsigned char *p, *cp;
  register size_t *bmBc, delta1, i;

  const unsigned char *x;
  unsigned char *y;
  size_t shift, found;

  const char *string_original;
  unsigned int string_original_length_in_bytes;
  const short *string_checks;
  grn_encoding string_encoding;
  const char *string_norm, *keyword_norm;
  unsigned int n, m;

  grn_string_get_original(ctx, string,
                          &string_original, &string_original_length_in_bytes);
  string_checks = grn_string_get_checks(ctx, string);
  string_encoding = grn_string_get_encoding(ctx, string);
  grn_string_get_normalized(ctx, string, &string_norm, &n, NULL);
  grn_string_get_normalized(ctx, cond->keyword, &keyword_norm, &m, NULL);

  y = (unsigned char *)string_norm;
  if (m == 1) {
    if (n > cond->found) {
      shift = 1;
      p = memchr(y + cond->found, keyword_norm[0], n - cond->found);
      if (p != NULL) {
        found = p - y;
        GRN_BM_COMPARE;
      }
    }
    cond->stopflag = SNIPCOND_STOP;
    return;
  }

  x = (unsigned char *)keyword_norm;
  bmBc = cond->bmBc;
  shift = cond->shift;

  /* Restart */
  p = y + m + cond->found;
  cp = x + m;
  ck = cp[-2];

  /* 12 means 1(initial offset) + 10 (in loop) + 1 (shift) */
  if (n - cond->found > 12 * m) {
    limit = y + n - 11 * m;
    while (p <= limit) {
      p += bmBc[p[-1]];
      if(!(delta1 = bmBc[p[-1]])) {
        goto check;
      }
      p += delta1;
      p += bmBc[p[-1]];
      p += bmBc[p[-1]];
      if(!(delta1 = bmBc[p[-1]])) {
        goto check;
      }
      p += delta1;
      p += bmBc[p[-1]];
      p += bmBc[p[-1]];
      if(!(delta1 = bmBc[p[-1]])) {
        goto check;
      }
      p += delta1;
      p += bmBc[p[-1]];
      p += bmBc[p[-1]];
      continue;
    check:
      GRN_BM_BM_COMPARE;
      p += shift;
    }
  }
  /* limit check + search */
  limit = y + n;
  while(p <= limit) {
    if (!(delta1 = bmBc[p[-1]])) {
      GRN_BM_BM_COMPARE;
      p += shift;
    }
    p += delta1;
  }
  cond->stopflag = SNIPCOND_STOP;
}

static size_t
count_mapped_chars(const char *str, const char *end)
{
  const char *p;
  size_t dl;

  dl = 0;
  for (p = str; p != end; p++) {
    switch (*p) {
    case '<':
    case '>':
      dl += 4;                  /* &lt; or &gt; */
      break;
    case '&':
      dl += 5;                  /* &amp; */
      break;
    case '"':
      dl += 6;                  /* &quot; */
      break;
    default:
      dl++;
      break;
    }
  }
  return dl;
}

grn_rc
grn_snip_cond_close(grn_ctx *ctx, snip_cond *cond)
{
  if (!cond) {
    return GRN_INVALID_ARGUMENT;
  }
  if (cond->keyword) {
    grn_obj_close(ctx, cond->keyword);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_snip_cond_init(grn_ctx *ctx, snip_cond *sc, const char *keyword, unsigned int keyword_len,
                   grn_encoding enc, grn_obj *normalizer, int flags)
{
  const char *norm;
  unsigned int norm_blen;
  int f = GRN_STR_REMOVEBLANK;
  memset(sc, 0, sizeof(snip_cond));
  if (!(sc->keyword = grn_string_open(ctx, keyword, keyword_len,
                                      normalizer, f))) {
    GRN_LOG(ctx, GRN_LOG_ALERT,
            "grn_string_open on snip_cond_init failed!");
    return GRN_NO_MEMORY_AVAILABLE;
  }
  grn_string_get_normalized(ctx, sc->keyword, &norm, &norm_blen, NULL);
  if (!norm_blen) {
    grn_snip_cond_close(ctx, sc);
    return GRN_INVALID_ARGUMENT;
  }
  if (norm_blen != 1) {
    grn_bm_preBmBc((unsigned char *)norm, norm_blen, sc->bmBc);
    sc->shift = sc->bmBc[(unsigned char)norm[norm_blen - 1]];
    sc->bmBc[(unsigned char)norm[norm_blen - 1]] = 0;
  }
  return GRN_SUCCESS;
}

void
grn_snip_cond_reinit(snip_cond *cond)
{
  cond->found = 0;
  cond->last_found = 0;
  cond->last_offset = 0;
  cond->start_offset = 0;
  cond->end_offset = 0;

  cond->count = 0;
  cond->stopflag = SNIPCOND_NONSTOP;
}

inline static char *
grn_snip_strndup(grn_ctx *ctx, const char *string, unsigned int string_len)
{
   char *copied_string;

   copied_string = GRN_MALLOC(string_len + 1);
   if (!copied_string) {
     return NULL;
   }
   grn_memcpy(copied_string, string, string_len);
   copied_string[string_len]= '\0'; /* not required, but for ql use */
   return copied_string;
}

inline static grn_rc
grn_snip_cond_set_tag(grn_ctx *ctx,
                      const char **dest_tag, size_t *dest_tag_len,
                      const char *tag, unsigned int tag_len,
                      const char *default_tag, unsigned int default_tag_len,
                      int copy_tag)
{
  if (tag) {
    if (copy_tag) {
      char *copied_tag;
      copied_tag = grn_snip_strndup(ctx, tag, tag_len);
      if (!copied_tag) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      *dest_tag = copied_tag;
    } else {
      *dest_tag = tag;
    }
    *dest_tag_len = tag_len;
  } else {
    *dest_tag = default_tag;
    *dest_tag_len = default_tag_len;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_snip_set_normalizer(grn_ctx *ctx, grn_obj *snip,
                        grn_obj *normalizer)
{
  grn_snip *snip_;
  if (!snip) {
    return GRN_INVALID_ARGUMENT;
  }

  snip_ = (grn_snip *)snip;
  snip_->normalizer = normalizer;
  return GRN_SUCCESS;
}

grn_obj *
grn_snip_get_normalizer(grn_ctx *ctx, grn_obj *snip)
{
  grn_snip *snip_;

  if (!snip) {
    return NULL;
  }

  snip_ = (grn_snip *)snip;
  return snip_->normalizer;
}

grn_rc
grn_snip_add_cond(grn_ctx *ctx, grn_obj *snip,
                  const char *keyword, unsigned int keyword_len,
                  const char *opentag, unsigned int opentag_len,
                  const char *closetag, unsigned int closetag_len)
{
  grn_rc rc;
  int copy_tag;
  snip_cond *cond;
  unsigned int norm_blen;
  grn_snip *snip_;

  snip_ = (grn_snip *)snip;
  if (!snip_ || !keyword || !keyword_len || snip_->cond_len >= MAX_SNIP_COND_COUNT) {
    return GRN_INVALID_ARGUMENT;
  }

  cond = snip_->cond + snip_->cond_len;
  if ((rc = grn_snip_cond_init(ctx, cond, keyword, keyword_len,
                               snip_->encoding, snip_->normalizer, snip_->flags))) {
    return rc;
  }
  grn_string_get_normalized(ctx, cond->keyword, NULL, &norm_blen, NULL);
  if (norm_blen > snip_->width) {
    grn_snip_cond_close(ctx, cond);
    return GRN_INVALID_ARGUMENT;
  }

  copy_tag = snip_->flags & GRN_SNIP_COPY_TAG;
  rc = grn_snip_cond_set_tag(ctx,
                             &(cond->opentag), &(cond->opentag_len),
                             opentag, opentag_len,
                             snip_->defaultopentag, snip_->defaultopentag_len,
                             copy_tag);
  if (rc) {
    grn_snip_cond_close(ctx, cond);
    return rc;
  }

  rc = grn_snip_cond_set_tag(ctx,
                             &(cond->closetag), &(cond->closetag_len),
                             closetag, closetag_len,
                             snip_->defaultclosetag, snip_->defaultclosetag_len,
                             copy_tag);
  if (rc) {
    if (opentag && copy_tag) {
      GRN_FREE((void *)cond->opentag);
    }
    grn_snip_cond_close(ctx, cond);
    return rc;
  }

  snip_->cond_len++;
  return GRN_SUCCESS;
}

static size_t
grn_snip_find_firstbyte(const char *string, grn_encoding encoding, size_t offset,
                        size_t doffset)
{
  switch (encoding) {
  case GRN_ENC_EUC_JP:
    while (!(grn_bm_check_euc((unsigned char *) string, offset)))
      offset += doffset;
    break;
  case GRN_ENC_SJIS:
    if (!(grn_bm_check_sjis((unsigned char *) string, offset)))
      offset += doffset;
    break;
  case GRN_ENC_UTF8:
    while ((signed char)string[offset] <= (signed char)0xc0)
      offset += doffset;
    break;
  default:
    break;
  }
  return offset;
}

inline static grn_rc
grn_snip_set_default_tag(grn_ctx *ctx,
                         const char **dest_tag, size_t *dest_tag_len,
                         const char *tag, unsigned int tag_len,
                         int copy_tag)
{
  if (copy_tag && tag) {
    char *copied_tag;
    copied_tag = grn_snip_strndup(ctx, tag, tag_len);
    if (!copied_tag) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    *dest_tag = copied_tag;
  } else {
    *dest_tag = tag;
  }
  *dest_tag_len = tag_len;
  return GRN_SUCCESS;
}

grn_obj *
grn_snip_open(grn_ctx *ctx, int flags, unsigned int width,
              unsigned int max_results,
              const char *defaultopentag, unsigned int defaultopentag_len,
              const char *defaultclosetag, unsigned int defaultclosetag_len,
              grn_snip_mapping *mapping)
{
  int copy_tag;
  grn_snip *ret = NULL;
  if (!(ret = GRN_MALLOC(sizeof(grn_snip)))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_snip allocation failed on grn_snip_open");
    return NULL;
  }
  if (max_results > MAX_SNIP_RESULT_COUNT || max_results == 0) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "max_results is invalid on grn_snip_open");
    GRN_FREE(ret);
    return NULL;
  }
  GRN_API_ENTER;
  ret->encoding = ctx->encoding;
  ret->flags = flags;
  ret->width = width;
  ret->max_results = max_results;
  ret->defaultopentag = NULL;
  ret->defaultclosetag = NULL;

  copy_tag = flags & GRN_SNIP_COPY_TAG;
  if (grn_snip_set_default_tag(ctx,
                               &(ret->defaultopentag),
                               &(ret->defaultopentag_len),
                               defaultopentag, defaultopentag_len,
                               copy_tag)) {
    GRN_FREE(ret);
    GRN_API_RETURN(NULL);
  }

  if (grn_snip_set_default_tag(ctx,
                               &(ret->defaultclosetag),
                               &(ret->defaultclosetag_len),
                               defaultclosetag, defaultclosetag_len,
                               copy_tag)) {
    if (copy_tag && ret->defaultopentag) {
      GRN_FREE((void *)ret->defaultopentag);
    }
    GRN_FREE(ret);
    GRN_API_RETURN(NULL);
  }

  ret->cond_len = 0;
  ret->mapping = mapping;
  ret->nstr = NULL;
  ret->tag_count = 0;
  ret->snip_count = 0;
  if (ret->flags & GRN_SNIP_NORMALIZE) {
    ret->normalizer = GRN_NORMALIZER_AUTO;
  } else {
    ret->normalizer = NULL;
  }

  GRN_DB_OBJ_SET_TYPE(ret, GRN_SNIP);
  {
    grn_obj *db;
    grn_id id;
    db = grn_ctx_db(ctx);
    id = grn_obj_register(ctx, db, NULL, 0);
    DB_OBJ(ret)->header.domain = GRN_ID_NIL;
    DB_OBJ(ret)->range = GRN_ID_NIL;
    grn_db_obj_init(ctx, db, id, DB_OBJ(ret));
  }

  GRN_API_RETURN((grn_obj *)ret);
}

static grn_rc
exec_clean(grn_ctx *ctx, grn_snip *snip)
{
  snip_cond *cond, *cond_end;
  if (snip->nstr) {
    grn_obj_close(ctx, snip->nstr);
    snip->nstr = NULL;
  }
  snip->tag_count = 0;
  snip->snip_count = 0;
  for (cond = snip->cond, cond_end = cond + snip->cond_len;
       cond < cond_end; cond++) {
    grn_snip_cond_reinit(cond);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_snip_close(grn_ctx *ctx, grn_snip *snip)
{
  snip_cond *cond, *cond_end;
  if (!snip) { return GRN_INVALID_ARGUMENT; }
  GRN_API_ENTER;
  if (snip->flags & GRN_SNIP_COPY_TAG) {
    int i;
    snip_cond *sc;
    const char *dot = snip->defaultopentag, *dct = snip->defaultclosetag;
    for (i = snip->cond_len, sc = snip->cond; i; i--, sc++) {
      if (sc->opentag != dot) { GRN_FREE((void *)sc->opentag); }
      if (sc->closetag != dct) { GRN_FREE((void *)sc->closetag); }
    }
    if (dot) { GRN_FREE((void *)dot); }
    if (dct) { GRN_FREE((void *)dct); }
  }
  if (snip->nstr) {
    grn_obj_close(ctx, snip->nstr);
  }
  for (cond = snip->cond, cond_end = cond + snip->cond_len;
       cond < cond_end; cond++) {
    grn_snip_cond_close(ctx, cond);
  }
  GRN_FREE(snip);
  GRN_API_RETURN(GRN_SUCCESS);
}

grn_rc
grn_snip_exec(grn_ctx *ctx, grn_obj *snip, const char *string, unsigned int string_len,
              unsigned int *nresults, unsigned int *max_tagged_len)
{
  size_t i;
  grn_snip *snip_;
  int f = GRN_STR_WITH_CHECKS|GRN_STR_REMOVEBLANK;
  if (!snip || !string || !nresults || !max_tagged_len) {
    return GRN_INVALID_ARGUMENT;
  }
  GRN_API_ENTER;
  snip_ = (grn_snip *)snip;
  exec_clean(ctx, snip_);
  *nresults = 0;
  snip_->nstr = grn_string_open(ctx, string, string_len, snip_->normalizer, f);
  if (!snip_->nstr) {
    exec_clean(ctx, snip_);
    GRN_LOG(ctx, GRN_LOG_ALERT, "grn_string_open on grn_snip_exec failed !");
    GRN_API_RETURN(ctx->rc);
  }
  for (i = 0; i < snip_->cond_len; i++) {
    grn_bm_tunedbm(ctx, snip_->cond + i, snip_->nstr, snip_->flags);
  }

  {
    _snip_tag_result *tag_result = snip_->tag_result;
    _snip_result *snip_result = snip_->snip_result;
    size_t last_end_offset = 0, last_last_end_offset = 0;
    unsigned int unfound_cond_count = snip_->cond_len;

    *max_tagged_len = 0;
    while (1) {
      size_t tagged_len = 0, last_tag_end = 0;
      int_least8_t all_stop = 1, found_cond = 0;
      snip_result->tag_count = 0;

      while (1) {
        size_t min_start_offset = (size_t) -1;
        size_t max_end_offset = 0;
        snip_cond *cond = NULL;

        /* get condition which have minimum offset and is not stopped */
        for (i = 0; i < snip_->cond_len; i++) {
          if (snip_->cond[i].stopflag == SNIPCOND_NONSTOP &&
              (min_start_offset > snip_->cond[i].start_offset ||
               (min_start_offset == snip_->cond[i].start_offset &&
                max_end_offset < snip_->cond[i].end_offset))) {
            min_start_offset = snip_->cond[i].start_offset;
            max_end_offset = snip_->cond[i].end_offset;
            cond = &snip_->cond[i];
          }
        }
        if (!cond) {
          break;
        }
        /* check whether condtion is the first condition in snippet */
        if (snip_result->tag_count == 0) {
          /* skip condition if the number of rest snippet field is smaller than */
          /* the number of unfound keywords. */
          if (snip_->max_results - *nresults <= unfound_cond_count && cond->count > 0) {
            int_least8_t exclude_other_cond = 1;
            for (i = 0; i < snip_->cond_len; i++) {
              if ((snip_->cond + i) != cond
                  && snip_->cond[i].end_offset <= cond->start_offset + snip_->width
                  && snip_->cond[i].count == 0) {
                exclude_other_cond = 0;
              }
            }
            if (exclude_other_cond) {
              grn_bm_tunedbm(ctx, cond, snip_->nstr, snip_->flags);
              continue;
            }
          }
          snip_result->start_offset = cond->start_offset;
          snip_result->first_tag_result_idx = snip_->tag_count;
        } else {
          if (cond->start_offset >= snip_result->start_offset + snip_->width) {
            break;
          }
          /* check nesting to make valid HTML */
          /* ToDo: allow <test><te>te</te><st>st</st></test> */
          if (cond->start_offset < last_tag_end) {
            grn_bm_tunedbm(ctx, cond, snip_->nstr, snip_->flags);
            continue;
          }
        }
        if (cond->end_offset > snip_result->start_offset + snip_->width) {
          /* If a keyword gets across a snippet, */
          /* it was skipped and never to be tagged. */
          cond->stopflag = SNIPCOND_ACROSS;
          grn_bm_tunedbm(ctx, cond, snip_->nstr, snip_->flags);
        } else {
          found_cond = 1;
          if (cond->count == 0) {
            unfound_cond_count--;
          }
          cond->count++;
          last_end_offset = cond->end_offset;

          tag_result->cond = cond;
          tag_result->start_offset = cond->start_offset;
          tag_result->end_offset = last_tag_end = cond->end_offset;

          snip_result->tag_count++;
          tag_result++;
          tagged_len += cond->opentag_len + cond->closetag_len;
          if (++snip_->tag_count >= MAX_SNIP_TAG_COUNT) {
            break;
          }
          grn_bm_tunedbm(ctx, cond, snip_->nstr, snip_->flags);
        }
      }
      if (!found_cond) {
        break;
      }
      if (snip_result->start_offset + last_end_offset < snip_->width) {
        snip_result->start_offset = 0;
      } else {
        snip_result->start_offset =
          MAX(MIN
              ((snip_result->start_offset + last_end_offset - snip_->width) / 2,
               string_len - snip_->width), last_last_end_offset);
      }
      snip_result->start_offset =
        grn_snip_find_firstbyte(string, snip_->encoding, snip_result->start_offset, 1);

      snip_result->end_offset = snip_result->start_offset + snip_->width;
      if (snip_result->end_offset < string_len) {
        snip_result->end_offset =
          grn_snip_find_firstbyte(string, snip_->encoding, snip_result->end_offset, -1);
      } else {
        snip_result->end_offset = string_len;
      }
      last_last_end_offset = snip_result->end_offset;

      if (snip_->mapping == (grn_snip_mapping *) -1) {
        tagged_len +=
          count_mapped_chars(&string[snip_result->start_offset],
                             &string[snip_result->end_offset]) + 1;
      } else {
        tagged_len += snip_result->end_offset - snip_result->start_offset + 1;
      }

      *max_tagged_len = MAX(*max_tagged_len, tagged_len);

      snip_result->last_tag_result_idx = snip_->tag_count - 1;
      (*nresults)++;
      snip_result++;

      if (*nresults == snip_->max_results || snip_->tag_count == MAX_SNIP_TAG_COUNT) {
        break;
      }
      for (i = 0; i < snip_->cond_len; i++) {
        if (snip_->cond[i].stopflag != SNIPCOND_STOP) {
          all_stop = 0;
          snip_->cond[i].stopflag = SNIPCOND_NONSTOP;
        }
      }
      if (all_stop) {
        break;
      }
    }
  }
  snip_->snip_count = *nresults;
  snip_->string = string;

  snip_->max_tagged_len = *max_tagged_len;

  GRN_API_RETURN(ctx->rc);
}

grn_rc
grn_snip_get_result(grn_ctx *ctx, grn_obj *snip, const unsigned int index, char *result, unsigned int *result_len)
{
  char *p;
  size_t i, j, k;
  _snip_result *sres;
  grn_snip *snip_;

  snip_ = (grn_snip *)snip;
  if (snip_->snip_count <= index || !snip_->nstr) {
    return GRN_INVALID_ARGUMENT;
  }

  GRN_ASSERT(snip_->snip_count != 0 && snip_->tag_count != 0);

  GRN_API_ENTER;
  sres = &snip_->snip_result[index];
  j = sres->first_tag_result_idx;
  for (p = result, i = sres->start_offset; i < sres->end_offset; i++) {
    for (; j <= sres->last_tag_result_idx && snip_->tag_result[j].start_offset == i; j++) {
      if (snip_->tag_result[j].end_offset > sres->end_offset) {
        continue;
      }
      grn_memcpy(p,
                 snip_->tag_result[j].cond->opentag,
                 snip_->tag_result[j].cond->opentag_len);
      p += snip_->tag_result[j].cond->opentag_len;
    }

    if (snip_->mapping == GRN_SNIP_MAPPING_HTML_ESCAPE) {
      switch (snip_->string[i]) {
      case '<':
        *p++ = '&';
        *p++ = 'l';
        *p++ = 't';
        *p++ = ';';
        break;
      case '>':
        *p++ = '&';
        *p++ = 'g';
        *p++ = 't';
        *p++ = ';';
        break;
      case '&':
        *p++ = '&';
        *p++ = 'a';
        *p++ = 'm';
        *p++ = 'p';
        *p++ = ';';
        break;
      case '"':
        *p++ = '&';
        *p++ = 'q';
        *p++ = 'u';
        *p++ = 'o';
        *p++ = 't';
        *p++ = ';';
        break;
      default:
        *p++ = snip_->string[i];
        break;
      }
    } else {
      *p++ = snip_->string[i];
    }

    for (k = sres->last_tag_result_idx;
         snip_->tag_result[k].end_offset <= sres->end_offset; k--) {
      /* TODO: avoid all loop */
      if (snip_->tag_result[k].end_offset == i + 1) {
        grn_memcpy(p,
                   snip_->tag_result[k].cond->closetag,
                   snip_->tag_result[k].cond->closetag_len);
        p += snip_->tag_result[k].cond->closetag_len;
      }
      if (k <= sres->first_tag_result_idx) {
        break;
      }
    };
  }
  *p = '\0';

  if(result_len) { *result_len = (unsigned int)(p - result); }
  GRN_ASSERT((unsigned int)(p - result) <= snip_->max_tagged_len);

  GRN_API_RETURN(ctx->rc);
}
