/* Copyright (c) 2004, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB   

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1335  USA */


#ifdef SCANNER_NEXT_NCHARS

#define SCANNER_NEXT_RETURN(_w,_n) \
  do { weight_and_nchars_t rc= {_w, _n}; return rc; } while(0)

#define SCANNER_NEXT_RETURN_CONTRACTION(_cnt,_ignorable_nchars) \
  do { \
    weight_and_nchars_t rc= { _cnt->weight[0], \
                              _ignorable_nchars + \
                              my_contraction_char_length(_cnt) }; \
     return rc; \
  } while(0)

#else

#define SCANNER_NEXT_RETURN(_w,_n) do { return _w; } while (0)

#define SCANNER_NEXT_RETURN_CONTRACTION(_cnt,_ignorable_nchars) \
  do { return _cnt->weight[0]; } while(0)

#endif

static inline
#ifdef SCANNER_NEXT_NCHARS
weight_and_nchars_t
MY_FUNCTION_NAME(scanner_next_with_nchars)(my_uca_scanner *scanner,
                                           size_t nchars)
#else
int
MY_FUNCTION_NAME(scanner_next)(my_uca_scanner *scanner)
#endif
{
#ifdef SCANNER_NEXT_NCHARS
  uint ignorable_nchars;
#define LOCAL_MAX_CONTRACTION_LENGTH nchars
#else
#define LOCAL_MAX_CONTRACTION_LENGTH MY_UCA_MAX_CONTRACTION
#endif
  /*
    Check if the weights for the previous character have been
    already fully scanned. If yes, then get the next character and
    initialize wbeg and wlength to its weight string.
  */

  if (scanner->wbeg[0])
  {
    /*
      More weights left from the previous step.
      Return the next weight from the current expansion.
      Return "0" as "nchars". The real nchars was set on a previous
      iteration.
    */
    SCANNER_NEXT_RETURN(*scanner->wbeg++, 0);
  }

#ifdef SCANNER_NEXT_NCHARS
  for (ignorable_nchars= 0 ; ; ignorable_nchars++)
#else
  for ( ; ; )
#endif
  {
    const uint16 *wpage;
    my_wc_t wc[MY_UCA_MAX_CONTRACTION];
    int mblen;

    /* Get next character */
#if MY_UCA_ASCII_OPTIMIZE
    /* Get next ASCII character */
    if (scanner->sbeg < scanner->send && scanner->sbeg[0] < 0x80)
    {
      wc[0]= scanner->sbeg[0];
      scanner->sbeg+= 1;

#if MY_UCA_COMPILE_CONTRACTIONS
      if (my_uca_needs_context_handling(scanner->level, wc[0]))
      {
        const MY_CONTRACTION *cnt= my_uca_context_weight_find(scanner, wc,
                                                  LOCAL_MAX_CONTRACTION_LENGTH);
        if (cnt)
          SCANNER_NEXT_RETURN_CONTRACTION(cnt, ignorable_nchars);
      }
#endif

      scanner->page= 0;
      scanner->code= (int) wc[0];
      scanner->wbeg= scanner->level->weights[0] + scanner->code * scanner->level->lengths[0];
      if (scanner->wbeg[0])
        SCANNER_NEXT_RETURN(*scanner->wbeg++, ignorable_nchars + 1);
      continue;
    }
    else
#endif
    /* Get next MB character */
    if (((mblen= MY_MB_WC(scanner, wc, scanner->sbeg,
                                       scanner->send)) <= 0))
    {
      if (scanner->sbeg >= scanner->send)
      {
        /* No more bytes, end of line reached */
        SCANNER_NEXT_RETURN(-1, ignorable_nchars);
      }
      /*
        There are some more bytes left. Non-positive mb_len means that
        we got an incomplete or a bad byte sequence. Consume mbminlen bytes.
      */
      if ((scanner->sbeg+= scanner->cs->mbminlen) > scanner->send)
      {
        /* For safety purposes don't go beyond the string range. */
        scanner->sbeg= scanner->send;
      }
      /*
        Treat every complete or incomplete mbminlen unit as a weight which is
        greater than weight for any possible normal character.
        0xFFFF is greater than any possible weight in the UCA weight table.
      */
      SCANNER_NEXT_RETURN(0xFFFF, ignorable_nchars + 1);
    }

    scanner->sbeg+= mblen;
    if (wc[0] > scanner->level->maxchar)
    {
      /* Return 0xFFFD as weight for all characters outside BMP */
      scanner->wbeg= nochar;
      SCANNER_NEXT_RETURN(0xFFFD, ignorable_nchars + 1);
    }

#if MY_UCA_COMPILE_CONTRACTIONS
    if (my_uca_needs_context_handling(scanner->level, wc[0]))
    {
      const MY_CONTRACTION *cnt= my_uca_context_weight_find(scanner, wc,
                                                LOCAL_MAX_CONTRACTION_LENGTH);
      if (cnt)
        SCANNER_NEXT_RETURN_CONTRACTION(cnt, ignorable_nchars);
    }
#endif

    /* Process single character */
    scanner->page= wc[0] >> 8;
    scanner->code= wc[0] & 0xFF;

    /* If weight page for w[0] does not exist, then calculate algoritmically */
    if (!(wpage= scanner->level->weights[scanner->page]))
      SCANNER_NEXT_RETURN(my_uca_scanner_next_implicit(scanner),
                          ignorable_nchars + 1);

    /* Calculate pointer to w[0]'s weight, using page and offset */
    scanner->wbeg= wpage +
                   scanner->code * scanner->level->lengths[scanner->page];
    if (scanner->wbeg[0])
      break;
    /* Skip ignorable character and continue the loop */
  }

  SCANNER_NEXT_RETURN(*scanner->wbeg++, ignorable_nchars + 1);
}

#undef SCANNER_NEXT_NCHARS
#undef SCANNER_NEXT_RETURN
#undef SCANNER_NEXT_RETURN_CONTRACTION
#undef LOCAL_MAX_CONTRACTION_LENGTH
