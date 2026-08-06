/* Copyright (c) 2016, 2022, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "item.h"
#include "sql_parse.h" // For check_stack_overrun

#ifndef DBUG_OFF
int dbug_json_check_min_stack_requirement()
{
  my_error(ER_STACK_OVERRUN_NEED_MORE, MYF(ME_FATAL),
           my_thread_stack_size, my_thread_stack_size, STACK_MIN_SIZE);
  return 1;
}
#endif

extern void pause_execution(THD *thd, double timeout);


#ifndef DBUG_OFF
/*
  Counts a reading of a JSON value.

  What these functions are being taught is not to read a value they have
  already read, and a value read one time fewer returns exactly what
  it gave back before - so there is nothing in any answer that says the
  saving happened.  The count is the only place it shows, which is why
  it exists at all.

  A scanner started while there is no session to charge is left
  uncounted rather than refused.

  Written the way json_lib calls it, that being what its address is
  handed to.
*/
extern "C" void json_count_scan()
{
  THD *thd= current_thd;

  if (thd)
    status_var_increment(thd->status_var.json_scans);
}


/*
  Asks json_lib to say when a value is read.

  The count is taken where the reading begins rather than where it is
  asked for.  Every way of asking - reading a path, asking whether a
  value is a document, normalizing one - ends up at json_scan_start(),
  so a count taken there is a count of readings.  A count taken at the
  asking would have to name every way of asking, and one such list has
  already been wrong: normalizing was reading twice and being counted
  none, because it does its reading from inside json_lib where a list
  kept here cannot see it.

  What is counted is therefore every reading this session does, and not
  only the ones these functions ask for by name.
*/
static struct Json_scan_count_hook
{
  Json_scan_count_hook() { json_scan_start_hook= json_count_scan; }
} json_scan_count_hook;


Json_scans_unbilled::Json_scans_unbilled(THD *thd)
 :m_thd(thd), m_scans(thd->status_var.json_scans)
{ }


Json_scans_unbilled::~Json_scans_unbilled()
{
  m_thd->status_var.json_scans= m_scans;
}
#endif


#ifdef DBUG_ASSERT_EXISTS
/*
  Takes a reading back off the count.

  A value is read back here to check what was claimed about it, and that
  reading exists only because the assertion checking the claim does.
  Counting it would put work no released server does into a number kept
  to watch the work it does, and the number would then move for reasons
  that have nothing to do with anybody's query.

  Where the count is not kept there is nothing to take anything off:
  Json_scans belongs to a debug build, while the assertions that make
  these readings are compiled by one build more than that.
*/
static inline void json_uncount_scan()
{
#ifndef DBUG_OFF
  THD *thd= current_thd;

  if (thd)
    status_var_decrement(thd->status_var.json_scans);
#endif
}


/*
  The way to the scanner for a reading that is not the server's work:
  counted like any other and then taken back off again, so that the one
  place the counting happens stays the only place it happens.
*/
static inline int json_scan_start_unbilled(json_engine_t *je,
                                           CHARSET_INFO *i_cs,
                                           const uchar *str, const uchar *end)
{
  int rc= json_scan_start(je, i_cs, str, end);

  json_uncount_scan();
  return rc;
}
#endif

/*
  Allocating memory and *also* using it (reading and
  writing from it) because some build instructions cause
  compiler to optimize out stack_used_up. Since alloca()
  here depends on stack_used_up, it doesnt get executed
  correctly and causes json_debug_nonembedded to fail
  ( --error ER_STACK_OVERRUN_NEED_MORE does not occur).
*/

#define JSON_DO_PAUSE_EXECUTION(A, B) do \
                                 { \
                                  DBUG_EXECUTE_IF("json_pause_execution", \
                                  { pause_execution(A, B); }); \
                                 } while(0)

/*
  Compare ASCII string against the string with the specified
  character set.
  Only compares the equality, case insensitive.
*/
static bool eq_ascii_string(const CHARSET_INFO *cs,
                            const char *ascii,
                            const char *s,  uint32 s_len)
{
  const char *s_end= s + s_len;

  while (*ascii && s < s_end)
  {
    my_wc_t wc;
    int wc_len;

    wc_len= cs->mb_wc(&wc, (uchar *) s, (uchar *) s_end);
    if (wc_len <= 0 || (wc | 0x20) != (my_wc_t) *ascii)
      return 0;

    ascii++;
    s+= wc_len;
  }

  return *ascii == 0 && s >= s_end;
}


static bool __attribute__((warn_unused_result))
append_simple(String *s, const char *a, size_t a_len)
{
  if (!s->realloc_with_extra_if_needed(s->length() + a_len))
  {
    s->q_append(a, a_len);
    return FALSE;
  }

  return TRUE;
}


static inline bool __attribute__((warn_unused_result))
append_simple(String *s, const uchar *a, size_t a_len)
{
  return append_simple(s, (const char *) a, a_len);
}


static void report_bad_chr_note(const char *fname, int n_arg);


/*
  Says whether a key can go between a pair of quotes without reaching
  past them.  A quote ends the string where it stands, a backslash begins
  an escape that may swallow the quote meant to end it, and a character
  below a space is not allowed inside a string unescaped at all.  A key
  holding none of the three is just so many characters of a name.

  Nothing is done about a key that holds one.  Whether the document that
  comes of splicing it can still be read is a property of the WHOLE
  composition and not of the key: a key of `a":1,"b` closes one member
  and opens another, and what comes out is a document that has always
  been given back.  Refusing it here would take that answer away.

  So the one thing this decides is whether the answer can be given
  without being read back.  A key it turns down is spliced exactly as it
  always was, and the reading back settles it exactly as it always did.

  The key is read a character at a time rather than a byte at a time,
  because a byte is not a character in every character set: in ucs2 the
  letter 'b' is written 00 62, and a reading that went by bytes would
  take the 00 for a control character and turn down every key there is.
  A key holding something that is not a character of its own character
  set at all is turned down, nothing being known about what it holds.
*/
static bool json_key_span_is_inert(CHARSET_INFO *cs, const char *key,
                                   size_t key_len)
{
  const uchar *p= (const uchar *) key, *end= p + key_len;

  while (p < end)
  {
    my_wc_t wc;
    int c_len= cs->mb_wc(&wc, p, end);

    if (c_len <= 0 || wc == '"' || wc == '\\' || wc < 0x20)
      return false;

    p+= c_len;
  }

  return true;
}


/*
  Appends the key of a path step to the document being built.

  A path is written in its own character set, which is not necessarily
  the one of the document, so the key may have to be converted before it
  can be spliced in.  Only a conversion that lost nothing is taken: a
  key holding a character the document's character set has no room for
  goes in as it arrived, which is what was done with every key before
  any key was converted at all, and a note says so.

  Whether the document that results can still be read is then decided
  by the document's character set, exactly as it was decided before:
  one that admits every byte keeps the document, and one that does not
  refuses it on the way back out.  Deciding it here instead would take
  away documents that have always been given.

  Nothing else is done to the key: one holding a character that is not
  allowed unescaped inside a JSON string stays invalid, and is caught
  where it was caught before.

  'inert' comes back saying whether the key went in as so many characters
  of a name and nothing more, so that a caller which would rather not
  read its answer back can tell when it has to.
*/
static bool __attribute__((warn_unused_result))
append_json_path_key(String *s, const json_path_step_t *step,
                     CHARSET_INFO *path_cs, String *tmp_key,
                     const char *fname, int n_arg, bool *inert)
{
  size_t key_len= (size_t) (step->key_end - step->key);
  uint errors;

  if (!my_charset_same(path_cs, s->charset()))
  {
    if (tmp_key->copy((const char *) step->key, key_len, path_cs,
                      s->charset(), &errors) ||
        DBUG_IF("json_path_key_out_of_memory"))
      return true; /* Out of memory. */

    if (!errors)
    {
      *inert= json_key_span_is_inert(s->charset(), tmp_key->ptr(),
                                     tmp_key->length());
      return append_simple(s, tmp_key->ptr(), tmp_key->length());
    }

    report_bad_chr_note(fname, n_arg);
    /*
      The key goes in written the way the path was, which is not the way
      the document is.  What those bytes come to mean once they are being
      read as the document cannot be told from the key alone.
    */
    *inert= false;
  }
  else
    *inert= json_key_span_is_inert(path_cs, (const char *) step->key, key_len);

  return append_simple(s, step->key, key_len);
}


/*
  Appends JSON string to the String object taking charsets in
  consideration.
*/
bool st_append_json(String *s,
             CHARSET_INFO *json_cs, const uchar *js, uint js_len)
{
  int str_len= js_len * s->charset()->mbmaxlen;

  if (s->reserve(str_len, 1024))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), str_len);
    return false;
  }

  str_len= json_unescape(json_cs, js, js + js_len, s->charset(),
                         (uchar *) s->end(), (uchar *) s->end() + str_len);
  if (str_len > 0)
    s->length(s->length() + str_len);

  if (str_len >= 0)
    return false;

  if (current_thd)
  {
    if (str_len == JSON_ERROR_OUT_OF_SPACE)
      my_error(ER_OUTOFMEMORY, MYF(0), str_len);
    else if (str_len == JSON_ERROR_ILLEGAL_SYMBOL)
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_JSON_BAD_CHR, ER_THD(current_thd, ER_JSON_BAD_CHR),
                          0, "st_append_json", 0);
  }

  return true;
}


/*
  Appends arbitrary String to the JSON string taking charsets in
  consideration.
*/
json_append_result st_append_escaped(String *s, const String *a)
{
  /*
    In the worst case one character from the 'a' string
    turns into '\uXXXX\uXXXX' which is 12.
  */
  int str_len= a->length() * 12 * s->charset()->mbmaxlen /
               a->charset()->mbminlen;
  if (s->reserve(str_len, 1024) ||
      DBUG_IF("json_escape_reserve_out_of_memory"))
    return JSON_APPEND_OOM;

  str_len= json_escape(a->charset(), (uchar *) a->ptr(), (uchar *)a->end(),
                       s->charset(),
                       (uchar *) s->end(), (uchar *)s->end() + str_len);
  if (str_len < 0)
    return str_len == JSON_ERROR_ILLEGAL_SYMBOL ?
           JSON_APPEND_BAD_CHR : JSON_APPEND_OOM;

  s->length(s->length() + str_len);
  return JSON_APPEND_OK;
}


static const int TAB_SIZE_LIMIT= 8;
static const char tab_arr[TAB_SIZE_LIMIT+1]= "        ";

static int append_tab(String *js, int depth, int tab_size)
{
  if (js->append('\n'))
    return 1;
  for (int i=0; i<depth; i++)
  {
    if (js->append(tab_arr, tab_size))
      return 1;
  }
  return 0;
}

int json_path_parts_compare(
    const json_path_step_t *a, const json_path_step_t *a_end,
    const json_path_step_t *b, const json_path_step_t *b_end,
    enum json_value_types vt, const int *array_sizes)
{
  int res, res2;
  const json_path_step_t *temp_b= b;

  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  return dbug_json_check_min_stack_requirement(););

  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  while (a <= a_end)
  {
    if (b > b_end)
    {
      while (vt != JSON_VALUE_ARRAY &&
             (a->type & JSON_PATH_ARRAY_WILD) == JSON_PATH_ARRAY &&
             a->n_item == 0)
      {
        if (++a > a_end)
          return 0;
      }
      return -2;
    }

    DBUG_ASSERT((b->type & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD)) == 0);

    if (a->type & JSON_PATH_ARRAY)
    {
      if (b->type & JSON_PATH_ARRAY)
      {
        int res = 0;
        if (a->type & JSON_PATH_WILD)
          res = 1;
        else if (a->type & JSON_PATH_ARRAY_RANGE && array_sizes)
        {
            int start = (a->n_item >= 0) ? a->n_item
                         : array_sizes[b - temp_b] + a->n_item;
            int end   = (a->n_item_end >= 0) ? a->n_item_end
                                   : array_sizes[b - temp_b] + a->n_item_end;
            res = (b->n_item >= start && b->n_item <= end);
        }
        else if (a->n_item >= 0)
          res = (a->n_item == b->n_item);
        else if (a->n_item < 0 && array_sizes)
          res = (a->n_item == b->n_item - array_sizes[b - temp_b]);

        if (res)
          goto step_fits;
        goto step_failed;
      }
      if ((a->type & JSON_PATH_WILD) == 0 && a->n_item == 0)
        goto step_fits_autowrap;
      goto step_failed;
    }
    else /* JSON_PATH_KEY */
    {
      if (!(b->type & JSON_PATH_KEY))
        goto step_failed;

      if (!(a->type & JSON_PATH_WILD) &&
          (a->key_end - a->key != b->key_end - b->key ||
           memcmp(a->key, b->key, a->key_end - a->key) != 0))
        goto step_failed;

      goto step_fits;
    }
step_failed:
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
      return -1;
    b++;
    continue;

step_fits:
    b++;
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
    {
      a++;
      continue;
    }

    /* Double wild handling needs recursions. */
    res= json_path_parts_compare(a+1, a_end, b, b_end, vt,
                                 array_sizes ? array_sizes + (b - temp_b) :
                                               NULL);
    if (res == 0)
      return 0;

    res2= json_path_parts_compare(a, a_end, b, b_end, vt,
                                  array_sizes ? array_sizes + (b - temp_b) :
                                                NULL);

    return (res2 >= 0) ? res2 : res;

step_fits_autowrap:
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
    {
      a++;
      continue;
    }

    /* Double wild handling needs recursions. */
    res= json_path_parts_compare(a+1, a_end, b+1, b_end, vt,
                                 array_sizes ? array_sizes + (b - temp_b) :
                                               NULL);
    if (res == 0)
      return 0;

    res2= json_path_parts_compare(a, a_end, b+1, b_end, vt,
                                  array_sizes ? array_sizes + (b - temp_b) :
                                                NULL);

    return (res2 >= 0) ? res2 : res;

  }

  return b <= b_end;
}


int json_path_compare(const json_path_t *a, const json_path_t *b,
                      enum json_value_types vt, const int *array_size)
{
  return json_path_parts_compare(a->steps+1, a->last_step,
                                 b->steps+1, b->last_step, vt, array_size);
}


/*
  How a document is punctuated in the loose form - this much after a
  comma, and this much after a key.

  Named here because two different pieces of code have to agree on it.
  json_nice() writes it when it reads a document back, and the functions
  that BUILD a document write it themselves and never read anything
  back; if those two ever drifted apart, a document said to be in the
  loose form would not be in it.  The compact and detailed forms are
  shorter and are taken as the first characters of the same two strings,
  which is what the lengths below select.
*/
static const char json_loose_comma[]= ", ";
static const char json_loose_colon[]= "\": ";


/*
  Whether a document can be written in this character set at all.

  A document is punctuated with characters that most character sets
  encode the way ASCII does, and a few do not: swe7 puts national letters
  where the brackets, the braces and the backslash belong, so the bytes
  that make an array anywhere else make a word there.  A function that
  writes an array out in such a character set has written something that
  is not a document and cannot be read as one - which is what the server
  has always done with it, and is not for this to change.  What it is
  for is to keep that result from being taken for a document later.

  Only the functions that write their own punctuation have to ask.  One
  that reads its whole result back afterwards reads it in the character
  set it is written in, so a document it accepts is one that reads
  there, and asking would tell it nothing new - which is why the seven
  that always read back never asked, and why they ask now that they read
  back only what their arguments left is_valid or is_nice false for.

  A set that cannot encode the punctuation can still hold a document,
  but only a scalar one: writing a container in it would take the
  brackets it cannot encode.  So a function that has to write a
  container to give its answer cannot give one there at all, and a
  function given a document in such a set was given a scalar.

  What decides it is how narrow a character is, not whether the set is
  ASCII-compatible.  MY_CS_NONASCII marks two unrelated things: sets that
  put other characters at the ASCII code points, and sets that are simply
  too wide to hold ASCII a byte at a time.  Only the first is a problem.
  String::append(char) converts for a set whose characters are never one
  byte, so ucs2 gets a real bracket, encoded as 005B; where a character
  can be a single byte the byte goes in as it stands, and in swe7 that
  byte is a letter.
*/
static inline bool is_json_compatible_charset(CHARSET_INFO *cs)
{
  return cs->mbminlen > 1 || !(cs->state & MY_CS_NONASCII);
}


/*
  Whether the document argument leaves a function free to return what
  it composes, instead of reading the whole answer again at the end to
  find out what it is.

  Three things at once, and each of them is about the argument rather
  than about anything composed from it.  The value has to read as a
  document, which is what the argument is asked.  It has to be written
  the way this writes, because what is composed keeps the argument's own
  spacing between the pieces the function writes itself.  And the set it
  is written in has to be able to encode the punctuation that gets added
  - nothing composed in a set that cannot is a document, however sound
  the pieces were.

  Asked once, before anything is composed, because what is written early
  is written before the later arguments have been looked at, so the
  answer cannot wait for them.  A later argument that turns out not to
  be attested to does not come back here; it clears the splice marks
  instead, and the answer goes through the reading back after all.
*/

static inline bool document_arg_composes_final(const Item *arg,
                                               const String *js)
{
  return is_json_compatible_charset(js->charset()) &&
         arg->is_valid_json() && arg->is_nice_json();
}


/*
  Steps over the space standing at 'str', if one is standing there, and
  returns where the next character begins.

  Composing writes some of its punctuation and copies the rest out of
  the document it is working from, and where the two meet one side has
  to give up the space between them.  Which bytes that is depends on the
  set the document is written in: a space is one byte in utf8, two in
  ucs2 (0020), four in utf32 - and in the wide ones the FIRST of those
  bytes is a zero.  So a byte compared against ' ' answers no in exactly
  the sets that can encode a document but do not write one character in
  one byte, and the space is left where it stood, doubled.

  Reading one character and asking what it is answers the question the
  way it was meant to be asked.
*/
static const uchar *json_skip_space(CHARSET_INFO *cs,
                                    const uchar *str, const uchar *end)
{
  my_wc_t wc;
  int len;

  if (str >= end)
    return str;

  len= cs->mb_wc(&wc, str, end);
  return (len > 0 && wc == (my_wc_t) ' ') ? str + len : str;
}


/* Written out below, and named here because the loose form goes to it. */
static int json_walk_nice_value(json_engine_t *je, String *to,
                                int &max_level);


/*
  'deepest', where a caller asks for it, comes back holding how many
  structures the deepest part of what was written nests inside: 0 for a
  scalar, 1 for an array of scalars, and so on.  The count is free here -
  the walk keeps it anyway, to know how far to indent - and it is the
  only place a whole document is measured without being walked twice.
*/
static int json_nice(json_engine_t *je, String *nice_js,
                     Item_func_json_format::formats mode,
                     uint *deepest= NULL, int tab_size=4)
{
  int depth= 0;
  int reached= 0;
  static const char *comma= json_loose_comma, *colon= json_loose_colon;
  uint comma_len, colon_len;
  int first_value= 1;
  int value_size = 0;
  int curr_state= -1;
  int64_t value_len= 0;
  String curr_str{};

  nice_js->length(0);
  nice_js->set_charset(je->s.cs);

  if (nice_js->alloc(je->s.str_end - je->s.c_str + 32))
    goto error;

  DBUG_ASSERT(mode != Item_func_json_format::DETAILED ||
              (tab_size >= 0 && tab_size <= TAB_SIZE_LIMIT));

  if (mode == Item_func_json_format::LOOSE)
  {
    /*
      The loose form is written by the walk, which is this loop with the
      other two modes taken out of it and stopped where the value ends
      instead of where the document does.  Written out twice they would
      have to go on agreeing byte for byte, and what says whether they
      still do is a check that measures a value by writing it through
      HERE - so the day the two drifted apart, the drift is what would
      be approved.

      The walk wants the value's head read, and returns where the
      value ended.  A document is a value with nothing after it, so what
      is left is read through to find out whether anything is.
    */
    if (json_read_value(je) || json_walk_nice_value(je, nice_js, reached))
    {
      /*
        Nothing was read wrong, so what went wrong was the writing:
        there is no room for the answer and no answer to give.
      */
      if (!je->s.error)
        return 1;
    }
    else
    {
      while (json_scan_next(je) == 0)
      {}
    }
    goto done;
  }

  /*
    The buffer above is a guess, and the indented form can outgrow it:
    it spends a line and an indent where the compact form spends
    nothing, so an input with more than sixteen separators in it needs
    more room than was asked for.  Everything written below therefore
    may have to grow the buffer, and this is where a test arranges for
    that growing to fail.  Disarmed at both ways out, so that only the
    writing done here is affected.
  */
  DBUG_EXECUTE_IF("json_nice_append_out_of_memory",
                  DBUG_SET("+d,simulate_realloc_out_of_memory"););

  /*
    What is left is the compact form and the indented one, which put the
    same comma between values and differ over the space after a colon.
  */
  comma_len= 1;
  colon_len= (mode == Item_func_json_format::DETAILED) ? 3 : 2;

  do
  {
    curr_state= je->state;
    switch (je->state)
    {
    case JST_KEY:
      {
        const uchar *key_start= je->s.c_str;
        const uchar *key_end;

        do
        {
          key_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);
        
        if (unlikely(je->s.error))
          goto error;

        if (!first_value && nice_js->append(comma, comma_len))
          goto error;

        if (mode == Item_func_json_format::DETAILED &&
            append_tab(nice_js, depth, tab_size))
          goto error;

        if (nice_js->append('"') ||
            append_simple(nice_js, key_start, key_end - key_start) ||
            nice_js->append(colon, colon_len))
          goto error;
      }
      /* now we have key value to handle, so no 'break'. */
      DBUG_ASSERT(je->state == JST_VALUE);
      goto handle_value;

    case JST_VALUE:
      if (!first_value && nice_js->append(comma, comma_len))
        goto error;

      if (mode == Item_func_json_format::DETAILED &&
          depth > 0 &&
          append_tab(nice_js, depth, tab_size))
        goto error;

handle_value:
      if (json_read_value(je))
        goto error;
      if (json_value_scalar(je))
      {
        if (append_simple(nice_js, je->value_begin,
                          je->value_end - je->value_begin))
          goto error;
        
        if (curr_str.copy((const char *)je->value_begin,
                          je->value_end - je->value_begin, je->s.cs))
          goto error;
        value_len= je->value_end - je->value_begin;
        first_value= 0;
        if (value_size != -1)
          value_size++;
      }
      else
      {
        if (mode == Item_func_json_format::DETAILED &&
            depth > 0 && !(curr_state != JST_KEY) &&
            append_tab(nice_js, depth, tab_size))
          goto error;
        if (nice_js->append((je->value_type == JSON_VALUE_OBJECT) ?
                            "{" : "[", 1))
          goto error;
        first_value= 1;
        value_size= (je->value_type == JSON_VALUE_OBJECT) ? -1: 0;
        depth++;
        if (depth > reached)
          reached= depth;
      }

      break;

    case JST_OBJ_END:
    case JST_ARRAY_END:
      depth--;
      if (mode == Item_func_json_format::DETAILED && (value_size > 1 || value_size == -1) &&
          append_tab(nice_js, depth, tab_size))
        goto error;
        
      if (mode == Item_func_json_format::DETAILED && 
          value_size == 1 && je->state != JST_OBJ_END)
      {
        nice_js->length(nice_js->length() - value_len);
        for (auto i = 0; i < (depth + 1) * tab_size + 1; i++)
          nice_js->chop();
        if (nice_js->append(curr_str))
          goto error;
      }

      if (nice_js->append((je->state == JST_OBJ_END) ? "}": "]", 1))
        goto error;
      first_value= 0;
      value_size= -1;
      break;

    default:
      break;
    };
  } while (json_scan_next(je) == 0);

  DBUG_EXECUTE_IF("json_nice_append_out_of_memory",
                  DBUG_SET("-d,simulate_realloc_out_of_memory"););

done:
  /*
    Said only where the walk finished, a count off a walk that stopped
    early being about the part of the document it got through rather
    than about the document.
  */
  if (deepest && !(je->s.error || *je->killed_ptr))
    *deepest= (uint) reached;
  return je->s.error || *je->killed_ptr;

error:
  DBUG_EXECUTE_IF("json_nice_append_out_of_memory",
                  DBUG_SET("-d,simulate_realloc_out_of_memory"););
  return 1;
}


/*
  The room to write in, made to run out for as long as one of these is
  standing.

  json_nice() arms the failure of its own appends around its own
  writing, so that a test can see what happens when one of them fails.
  The writing has moved out of it for the functions that no longer read
  their result back, and the way of failing it had to move with it -
  otherwise the checking of these appends would stop being tested by the
  thing that was testing it, without anybody noticing that it did.

  Disarmed however the writing is left, so that only the writing done
  while this stands is affected.
*/
class Json_room_made_to_run_out
{
public:
  Json_room_made_to_run_out()
  {
    DBUG_EXECUTE_IF("json_nice_append_out_of_memory",
                    DBUG_SET("+d,simulate_realloc_out_of_memory"););
  }
  ~Json_room_made_to_run_out()
  {
    DBUG_EXECUTE_IF("json_nice_append_out_of_memory",
                    DBUG_SET("-d,simulate_realloc_out_of_memory"););
  }
};


/*
  Writes the value the scanner is sitting on to the end of 'to', in the
  loose form, walking the scanner over the value as it goes.

  This is json_nice() taken apart and put back together to be usable
  during a reading instead of after one.  json_nice() empties its output
  and reads to the end of the document, which suits a function that has
  finished and wants its whole result formatted again; it is no use to one
  that is partway through a document and wants THIS value written into
  what it has written so far.  The formatting itself is the same, and has
  to be: the two write the same punctuation from the same constants, so
  a value put out by either is put out identically.

  The scanner must have read the value's head already - which is where
  json_get_path_next() leaves it - and is left on the value's last
  token, so the caller can carry on from there exactly as if it had
  walked over the value itself.

  A scalar has no punctuation of its own and so is already written the
  loose way wherever it came from; it is copied across as it stands,
  which is what json_nice() does with one too.

  'max_level' comes back holding the deepest the walk went, counted the
  way a plain reading of the same value counts it - which is what a
  caller that has to say how deep its answer ends up would otherwise
  have to take a second reading to find out.  It is raised and never
  lowered, so a caller starting one walk where the last left off gets
  the deepest of them all.

  Every writing of a value goes through here, which is what makes this
  the one place the failure of these appends has to be arranged.

  Returns non-zero on a write that failed or a value that did not end.
*/
static int json_walk_nice_value(json_engine_t *je, String *to,
                                int &max_level)
{
  Json_room_made_to_run_out room;
  int depth;
  int first_value= 1;

  if (je->stack_p > max_level)
    max_level= je->stack_p;

  if (json_value_scalar(je))
    return append_simple(to, je->value_begin,
                         je->value_end - je->value_begin);

  if (to->append((je->value_type == JSON_VALUE_OBJECT) ? "{" : "[", 1))
    return 1;
  depth= 1;

  while (json_scan_next(je) == 0)
  {
    if (je->stack_p > max_level)
      max_level= je->stack_p;

    /*
      The step above is what a killed query is stopped by here, and it
      is the same step, taken the same number of times, that the reading
      back used to be stopped by - so nothing is asked of this loop that
      the reading it replaces was not already asked.

      A kill arriving before the writing begins is stopped by whichever
      reading got here, which is the only kill the tests could reach
      until now.  This arms one that arrives partway through, so that
      what stops it is the step above and nothing else.
    */
    DBUG_EXECUTE_IF("json_kill_while_emitting",
                    { current_thd->set_killed(KILL_QUERY); });

    switch (je->state)
    {
    case JST_KEY:
      {
        const uchar *key_start= je->s.c_str;
        const uchar *key_end;

        do
        {
          key_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);

        if (unlikely(je->s.error))
          return 1;

        if (!first_value && to->append(json_loose_comma, 2))
          return 1;

        if (to->append('"') ||
            append_simple(to, key_start, key_end - key_start) ||
            to->append(json_loose_colon, 3))
          return 1;
      }
      /* The key's value comes next, so there is no break here. */
      DBUG_ASSERT(je->state == JST_VALUE);
      goto handle_value;

    case JST_VALUE:
      if (!first_value && to->append(json_loose_comma, 2))
        return 1;

handle_value:
      if (json_read_value(je))
        return 1;

      if (je->stack_p > max_level)
        max_level= je->stack_p;

      if (json_value_scalar(je))
      {
        if (append_simple(to, je->value_begin,
                          je->value_end - je->value_begin))
          return 1;
        first_value= 0;
      }
      else
      {
        if (to->append((je->value_type == JSON_VALUE_OBJECT) ? "{" : "[", 1))
          return 1;
        first_value= 1;
        depth++;
      }
      break;

    case JST_OBJ_END:
    case JST_ARRAY_END:
      if (to->append((je->state == JST_OBJ_END) ? "}" : "]", 1))
        return 1;
      first_value= 0;
      if (--depth == 0)
        return je->s.error != 0;
      break;

    default:
      break;
    }
  }

  /* The document ended in the middle of the value. */
  return 1;
}


#ifdef DBUG_ASSERT_EXISTS
/*
  Reads a value the way whoever receives it will read it: in the
  character set the value says it is written in, from one end of it to
  the other.
*/
bool json_value_reads_as_document(const String *str)
{
  /*
    Which is the question json_valid() is, so it is asked rather than
    asked again here.  The reading it does begins at json_scan_start()
    like every other, so it is counted like every other and taken back
    off afterwards - the same two steps json_scan_start_unbilled() makes,
    in the order the borrowed reading leaves them in.
  */
  bool valid= json_valid(str->ptr(), str->length(), str->charset()) != 0;

  json_uncount_scan();
  return valid;
}


/*
  A kill is not simulated in a reading this file makes for itself.

  The arming that raises one partway through a walk is aimed at the
  walks a statement makes on its way to an answer.  A debug build makes
  one walk MORE than a release build does - the one just below, which
  writes a value out again to find out whether it was already written
  that way - and a kill raised in THAT walk would be a debug build
  ending a statement a release build finishes.  What is checked here is
  the code, and a check that changes the answer is not one.

  So it is held off for as long as the reading lasts, the way the
  reading itself is held off the count of readings.
*/
class Json_kill_unsimulated
{
  bool m_armed;
public:
  Json_kill_unsimulated() : m_armed(DBUG_IF("json_kill_while_emitting"))
  {
    if (m_armed)
      DBUG_SET("-d,json_kill_while_emitting");
  }
  ~Json_kill_unsimulated()
  {
    if (m_armed)
      DBUG_SET("+d,json_kill_while_emitting");
  }
};


/*
  Writes the value out again in the loose form and asks whether that
  changed anything.  A value already written that way comes back byte
  for byte; one written any other way does not.
*/
bool json_value_is_nice(const String *str)
{
  Json_kill_unsimulated unkilled;
  json_engine_t je;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> nice;

  json_scan_start_unbilled(&je, str->charset(), (const uchar *) str->ptr(),
                           (const uchar *) str->ptr() + str->length());
  if (json_nice(&je, &nice, Item_func_json_format::LOOSE))
    return false;

  return !stringcmp(&nice, str);
}


/*
  How deep the value actually goes, read off the value itself.  A
  claimed depth is only ever compared against this one - a claim that
  is larger than the truth is allowed, that being what an item saying
  nothing amounts to, and one that is smaller is the failure this
  exists to catch.
*/
uint json_value_depth(const String *str)
{
  json_engine_t je;
  uint deepest= 0;

  json_scan_start_unbilled(&je, str->charset(), (const uchar *) str->ptr(),
                           (const uchar *) str->ptr() + str->length());
  while (json_scan_next(&je) == 0)
  {
    if ((uint) je.stack_p > deepest)
      deepest= (uint) je.stack_p;
  }

  return deepest;
}
#endif


/*
  A document being edited is walked once to find the place to edit, and
  the pieces of it that go into the answer are copied from where that
  walk left off - so the walk's pointers into it stay live across
  everything that happens in between.  What happens in between includes
  working out a path and working out a value, and either can be any
  expression a caller cares to write.

  Nothing here owns the document.  It can be a table's row, a routine's
  variable or another statement's user variable, and what an expression
  writes is not this function's to know.  That the bytes stay put is
  true today because of the shapes an expression can take and not
  because anything makes it so, which is exactly the kind of thing that
  stops being true quietly.  So a debug build holds on to a copy and
  says whether it still matches.

  A release build keeps nothing and asks nothing, this being a check on
  the code rather than on the data.
*/
class Json_source_watch
{
#ifndef DBUG_OFF
  String m_held;
  bool m_holding;
#endif
public:
#ifdef DBUG_OFF
  void take(const String *) {}
  bool unchanged(const String *) const { return true; }
#else
  Json_source_watch() : m_holding(false) {}
  void take(const String *js)
  {
    /*
      A copy that cannot be made says nothing either way, and must not
      turn into a complaint: the room to make it is made to run out on
      purpose by the tests that check what happens when it does.

      A document that is not there says nothing either.  Asking for one
      is how a function finds out whether there is one, and the watch is
      taken before anything else can run, so the two orders would
      otherwise have to be told apart by every caller.
    */
    m_holding= js && !m_held.copy(js->ptr(), js->length(), js->charset());
  }
  bool unchanged(const String *js) const
  { return !m_holding || !stringcmp(js, &m_held); }
#endif
};


/*
  A mark is a promise about a run of bytes, and the bytes are right
  here, so a debug build keeps the promise by reading them.

  Nothing else ever will.  The marks appear in no result and change no
  output, so one of them being wrong shows up nowhere at all until
  something starts acting on it - and by then the function that made the
  promise is a long way from the code that believed it.
*/
void Json_result_marks::set(const String *str, bool valid, bool nice,
                            uint depth)
{
  /*
    The loose form is a formatting of a document, so there is no such
    thing as a value formatted that way which is not one.  Said before the
    two below because it holds whatever the bytes turn out to be, and
    because a caller that arrives here with the pair the wrong way round
    has lost track of which question it was answering.
  */
  DBUG_ASSERT(!nice || valid);
  DBUG_ASSERT(!valid || json_value_reads_as_document(str));
  DBUG_ASSERT(!nice || json_value_is_nice(str));
  /*
    A depth is only about a value that reads as a document, and is only
    ever wrong in one direction - see json_value_depth().
  */
  DBUG_ASSERT(depth == JSON_DEPTH_UNKNOWN || !valid ||
              json_value_depth(str) <= depth);
  m_valid= valid;
  m_nice= nice;
  m_depth= depth;
}


#define report_json_error(js, je, n_param) \
  report_json_error_ex(js->ptr(), je, func_name(), n_param, \
      Sql_condition::WARN_LEVEL_WARN)

void report_json_error_ex(const char *js, json_engine_t *je,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv)
{
  THD *thd= current_thd;
  int position= (int)((const char *) je->s.c_str - js);
  uint code;

  n_param++;

  switch (je->s.error)
  {
  case JE_BAD_CHR:
    code= ER_JSON_BAD_CHR;
    break;

  case JE_NOT_JSON_CHR:
    code= ER_JSON_NOT_JSON_CHR;
    break;

  case JE_EOS:
    code= ER_JSON_EOS;
    break;

  case JE_SYN:
  case JE_STRING_CONST:
    code= ER_JSON_SYNTAX;
    break;

  case JE_ESCAPING:
    code= ER_JSON_ESCAPING;
    break;

  case JE_DEPTH:
    code= ER_JSON_DEPTH;
    if (lv == Sql_condition::WARN_LEVEL_ERROR)
      my_error(code, MYF(0), JSON_DEPTH_LIMIT, n_param, fname, position);
    else
      push_warning_printf(thd, lv, code, ER_THD(thd, code), JSON_DEPTH_LIMIT,
                          n_param, fname, position);
    return;

  case JE_KILLED:
    thd->send_kill_message();
    return;

  default:
    return;
  }

  if (lv == Sql_condition::WARN_LEVEL_ERROR)
    my_error(code, MYF(0), n_param, fname, position);
  else
    push_warning_printf(thd, lv, code, ER_THD(thd, code),
                        n_param, fname, position);
}


/*
  Says that a character had nowhere to go, in the cases where that is
  all that is being said: the bytes are passed on unchanged, as they
  always were, and this note is the only sign that anything was amiss.

  A note and not a warning.  A warning becomes an error inside a
  statement running under strict mode, and every caller of this is
  somewhere the released server said nothing at all, so a warning would
  stop statements that used to finish.

  The argument is numbered the way the caller numbers it in its other
  diagnostics, and the position is the beginning of it: what is
  reported here is that the writing stopped, not where it stopped.
*/
static void report_bad_chr_note(const char *fname, int n_arg)
{
  THD *thd= current_thd;

  if (thd)
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_JSON_BAD_CHR,
                        ER_THD(thd, ER_JSON_BAD_CHR), n_arg, fname, 0);
}



#define NO_WILDCARD_ALLOWED 1
#define SHOULD_END_WITH_ARRAY 2
#define TRIVIAL_PATH_NOT_ALLOWED 3

#define report_path_error(js, je, n_param) \
  report_path_error_ex(js->ptr(), je, func_name(), n_param,\
      Sql_condition::WARN_LEVEL_WARN)

void report_path_error_ex(const char *ps, json_path_t *p,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv)
{
  THD *thd= current_thd;
  int position= (int)((const char *) p->s.c_str - ps + 1);
  uint code;

  n_param++;

  switch (p->s.error)
  {
  case JE_BAD_CHR:
  case JE_NOT_JSON_CHR:
  case JE_SYN:
    code= ER_JSON_PATH_SYNTAX;
    break;

  case JE_EOS:
    code= ER_JSON_PATH_EOS;
    break;

  case JE_DEPTH:
    code= ER_JSON_PATH_DEPTH;
    if (lv == Sql_condition::WARN_LEVEL_ERROR)
      my_error(code, MYF(0), JSON_DEPTH_LIMIT, n_param, fname, position);
    else
      push_warning_printf(thd, lv, code, ER_THD(thd, code),
                          JSON_DEPTH_LIMIT, n_param, fname, position);
    return;

  case NO_WILDCARD_ALLOWED:
    code= ER_JSON_PATH_NO_WILDCARD;
    break;

  case TRIVIAL_PATH_NOT_ALLOWED:
    code= ER_JSON_PATH_EMPTY;
    break;


  default:
    return;
  }
  if (lv == Sql_condition::WARN_LEVEL_ERROR)
    my_error(code, MYF(0), n_param, fname, position);
  else
    push_warning_printf(thd, lv, code, ER_THD(thd, code),
                        n_param, fname, position);
}


/*
  Checks if the path has '.*' '[*]' or '**' constructions
  and sets the NO_WILDCARD_ALLOWED error if the case.
*/
__attribute__((nonnull, warn_unused_result))
static int path_setup_nwc(json_path_t *p, CHARSET_INFO *i_cs,
                          const uchar *str, const uchar *end)
{
  if (!json_path_setup(p, i_cs, str, end))
  {
    if ((p->types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD |
                          JSON_PATH_ARRAY_RANGE)) == 0)
      return 0;
    p->s.error= NO_WILDCARD_ALLOWED;
  }

  return 1;
}

static inline
CHARSET_INFO *def_path_charset(CHARSET_INFO *cs, CHARSET_INFO *alt)
{
  if (cs) return cs;
  if (alt) return alt;
  return &my_charset_utf8mb4_bin;
}

bool Item_func_json_valid::val_bool()
{
  String *js= args[0]->val_json(&tmp_value);
  THD *thd;
  json_engine_t je;

  if ((null_value= args[0]->null_value))
    return 0;

  /*
    What this function asks is what a value answers about itself, so
    where the value has answered there is nothing left to find out.

    It is the same reading the check constraint leaves unrun at the
    field boundary, left unrun here at the most direct site there is,
    and the two cannot disagree: a mark says the characters read back
    as a document, which is exactly what the walk below would go and
    see.  Nothing is said either way, the answer being true, and the
    reading is the only thing that could have said anything.
  */
  if (args[0]->is_valid_json())
    return true;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (json_valid_engine(&je, js->ptr(), js->length(), js->charset()))
    return true;
  /* Sql_condition::WARN_LEVEL_WARN becomes an error in check constraints */
  report_json_error_ex(js->ptr(), &je, func_name(), 0, Sql_condition::WARN_LEVEL_NOTE);
  return false;
}


bool Item_func_json_equals::fix_length_and_dec(THD *thd)
{
  if (Item_bool_func::fix_length_and_dec(thd))
    return TRUE;
  set_maybe_null();
  return FALSE;
}


bool Item_func_json_equals::val_bool()
{
  longlong result= 0;
  int arg_num= 0;
  String a_tmp, b_tmp;
  THD *thd;
  json_engine_t je;
  Json_source_watch watch;

  String *a= args[0]->val_json(&a_tmp);
  if ((null_value= a == nullptr || args[0]->null_value))
    return 1;
  watch.take(a);
  String *b= args[1]->val_json(&b_tmp);
  if ((null_value= b == nullptr || args[1]->null_value))
    return 1;

  DYNAMIC_STRING a_res;
  if (init_dynamic_string(&a_res, NULL, 0, 0))
  {
    null_value= 1;
    return 1;
  }

  DYNAMIC_STRING b_res;
  if (init_dynamic_string(&b_res, NULL, 0, 0))
  {
    dynstr_free(&a_res);
    null_value= 1;
    return 1;
  }

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);
  je.killed_ptr= (uint32_t *) &thd->killed;

  DBUG_ASSERT(watch.unchanged(a));
  if (json_normalize_engine(&je, &a_res, a->ptr(), a->length(), a->charset()))
    goto return_null;

  arg_num++;
  if (json_normalize_engine(&je, &b_res, b->ptr(), b->length(), b->charset()))
    goto return_null;

  result= strcmp(a_res.str, b_res.str) ? 0 : 1;
  goto end;

return_null:
  null_value= 1;

end:
  if (je.s.error)
  {
    /* looks convoluted, but report_json_error is a macro */
    if (arg_num != 0)
       a= b;
    report_json_error(a, &je, arg_num);
  }
  dynstr_free(&b_res);
  dynstr_free(&a_res);
  return result;
}


bool Item_func_json_exists::fix_length_and_dec(THD *thd)
{
  if (Item_bool_func::fix_length_and_dec(thd))
    return TRUE;
  set_maybe_null();
  path.set_constant_flag(args[1]->const_item());
  return FALSE;
}


bool Item_func_json_exists::val_bool()
{
  json_engine_t je;
  Json_source_watch watch;
  int array_counters[JSON_DEPTH_LIMIT]= {0};
  THD *thd= current_thd;

  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  String *js= args[0]->val_json(&tmp_js);

  watch.take(js);
  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        json_path_setup(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
      goto err_return;
    path.parsed= path.constant;
  }

  if ((null_value= args[0]->null_value || args[1]->null_value))
  {
    null_value= 1;
    return 0;
  }

  null_value= 0;
  DBUG_ASSERT(watch.unchanged(js));
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  path.cur_step= path.p.steps;
  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto js_error;
    return 0;
  }

  return 1;

js_error:
  report_json_error(js, &je, 0);
err_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_value::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_constant_flag(args[1]->const_item());
  set_maybe_null();
  return FALSE;
}


bool Item_func_json_query::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_constant_flag(args[1]->const_item());
  set_maybe_null();
  return FALSE;
}


bool Json_path_extractor::extract(String *str, Item *item_js, Item *item_jp,
                                  CHARSET_INFO *cs, LEX_CSTRING func_name,
                                  bool allow_wildcard)
{
  String *js= item_js->val_json(&tmp_js);
  int error= 0;
  int array_counters[JSON_DEPTH_LIMIT]= {0};
  Json_source_watch watch;

  /* Taken here rather than where they are used - see m_js_nice. */
  m_js_nice= item_js->is_nice_json();
  m_js_depth= item_js->last_depth();

  watch.take(js);
  if (!parsed)
  {
    String *s_p= item_jp->val_str(&tmp_path);

    if (!s_p)
      return true;
    if (allow_wildcard)
    {
      if (!s_p->charset() ||
        json_path_setup(&p, s_p->charset(), (const uchar *) s_p->ptr(),
                       (const uchar *) s_p->ptr() + s_p->length()))
        error= true;
    }
    else
    {
      if (path_setup_nwc(&p, def_path_charset(s_p->charset(), cs),
                         (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
        error= true;
    }

    if (error)
    {
      report_path_error_ex(s_p->ptr(), &p, func_name.str, 1, Sql_condition::WARN_LEVEL_WARN);
      return true;
    }

    parsed= constant;
  }

  if (item_js->null_value || item_jp->null_value)
    return true;

  DBUG_ASSERT(watch.unchanged(js));
  Json_engine_scan je(*js);
  str->length(0);
  str->set_charset(cs);

  cur_step= p.steps;
continue_search:
  if (json_find_path(&je, &p, &cur_step, array_counters))
    return true;

  if (json_read_value(&je))
    return true;

  if (je.value_type == JSON_VALUE_NULL)
    return true;

  if (unlikely(check_and_get_value(&je, str, &error)))
  {
    if (error)
      return true;
    goto continue_search;
  }

  return false;
}


bool Json_engine_scan::check_and_get_value_scalar(String *res, int *error)
{
  CHARSET_INFO *json_cs;
  const uchar *js;
  uint js_len;

  if (!json_value_scalar(this))
  {
    /* We only look for scalar values! */
    if (json_skip_level(this) || json_scan_next(this))
      *error= 1;
    return true;
  }

  if (value_type == JSON_VALUE_TRUE ||
      value_type == JSON_VALUE_FALSE)
  {
    json_cs= &my_charset_utf8mb4_bin;
    js= (const uchar *) ((value_type == JSON_VALUE_TRUE) ? "1" : "0");
    js_len= 1;
  }
  else
  {
    json_cs= s.cs;
    js= value;
    js_len= value_len;
  }

  if (st_append_json(res, json_cs, js, js_len))
  {
    *error= 1;
    return true;
  }
  return false;
}


bool Json_engine_scan::check_and_get_value_complex(String *res, int *error)
{
  if (json_value_scalar(this))
  {
    /* We skip scalar values. */
    if (json_scan_next(this))
      *error= 1;
    return true;
  }

  const uchar *tmp_value= value;
  if (json_skip_level(this))
  {
    *error= 1;
    return true;
  }

  res->set((const char *) value, (uint32)(s.c_str - tmp_value), s.cs);
  return false;
}


bool Item_func_json_quote::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb4_bin);
  /*
    Odd but realistic worst case is when every character of the argument
    has to be escaped.  json_escape() writes a backslash, a 'u' and the
    hex of the UTF-16 form, which my_uni_utf16() gives as two bytes for a
    character of the first plane and four for any other: eight figures at
    the very worst, so ten characters and never more.
  */
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * 10 + 2);
  return FALSE;
}


String *Item_func_json_quote::val_str(String *str)
{
  String *s= args[0]->val_str(&tmp_s);
  json_append_result rc;

  if ((null_value= (args[0]->null_value ||
                    args[0]->result_type() != STRING_RESULT)))
    return NULL;

  str->length(0);
  str->set_charset(&my_charset_utf8mb4_bin);

  if (str->append('"') || DBUG_IF("json_quote_open_out_of_memory"))
    goto error;

  if ((rc= st_append_escaped(str, s)))
  {
    /*
      A character no document can carry stops the writing, and this
      function has always answered NULL for it.  What it has never done
      is SAY so: the three other JSON functions that write a value
      through st_append_escaped() all report the note, and a value moved
      from one of them to this one lost the only signal there is for the
      condition.  The optimizer trace calls it as well and reports
      nothing, a trace not being an answer anybody asked for.  The answer
      is unchanged - only the silence is.
    */
    if (rc == JSON_APPEND_BAD_CHR)
      report_bad_chr_note(func_name(), 1);
    goto error;
  }

  if (str->append('"') || DBUG_IF("json_quote_close_out_of_memory"))
    goto error;

  return str;

error:
  null_value= 1;
  return 0;
}


bool Item_func_json_unquote::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb4_bin,
                DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  max_length= args[0]->max_char_length() * collation.collation->mbmaxlen;
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_unquote::read_json(json_engine_t *je)
{
  String *js= args[0]->val_json(&tmp_s);

  if ((null_value= args[0]->null_value))
    return 0;

  /* no json_scan_next called so not interuptable */
  json_scan_start(je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (json_read_value(je))
    goto error;

  return js;

error:
  if (je->value_type == JSON_VALUE_STRING)
    report_json_error(js, je, 0);
  return js;
}


/*
  Returns the argument unchanged, but in the character set this item
  declares.  The bytes and the label have to agree: a value encoded one
  way and labelled another is read wrongly by everything downstream of
  it, and the character set is settled at fix time, so it is the bytes
  that have to move.

  An argument that carries no character set at all is read one byte to
  one character, which is how json_unescape() already reads it when the
  value turns out to be a string.  Both ways out of the function then
  say the same thing about the same argument.
*/
String *Item_func_json_unquote::return_as_is(String *str, String *js)
{
  CHARSET_INFO *from_cs= js->charset();
  uint errors;

  if (my_charset_same(from_cs, collation.collation))
    return js;

  if (from_cs == &my_charset_bin)
  {
    /*
      An argument that carries no character set is read one byte to one
      character, which is the reading json_unescape() gives it on the
      other way out: my_charset_bin's mb_wc returns the byte itself.
      Reading it any other way would have the two exits of this function
      name the same byte as two different characters.  latin1 in
      particular is NOT that reading - MariaDB's latin1 is cp1252, which
      differs from it at 27 of the 32 positions in 0x80-0x9F.

      Converted rather than copied, and not through String::copy(),
      which would not convert: needs_conversion() answers false for a
      binary source whose length divides the destination's mbminlen, and
      utf8mb4's is 1, so the bytes would go across untouched and be
      relabelled - 0x80 would leave here as a lone continuation byte,
      which is not a character of the set the result says it is in.
      copy_and_convert() runs the conversion that short-circuit skips.
    */
    uint32 room= (uint32) (collation.collation->mbmaxlen * js->length());

    if (str->alloc(room) || DBUG_IF("json_unquote_as_is_out_of_memory"))
    {
      null_value= 1;
      return NULL;
    }
    str->length(copy_and_convert((char *) str->ptr(), room,
                                 collation.collation, js->ptr(),
                                 js->length(), from_cs, &errors));
    str->set_charset(collation.collation);
  }
  else if (str->copy(js->ptr(), js->length(), from_cs,
                     collation.collation, &errors) ||
           DBUG_IF("json_unquote_as_is_out_of_memory"))
  {
    null_value= 1;
    return NULL;
  }

  /*
    Only a conversion that lost nothing is worth having.  A character
    with nowhere to go comes out of the conversion as a substitute for
    itself, and returning that would answer differently than this
    call has always answered - the argument would come back altered
    rather than merely mislabelled.  The label is the lesser of the two
    wrongs, so the bytes stay as they arrived and the note says why.
  */
  if (errors)
  {
    report_bad_chr_note("unquote", 0);
    return js;
  }

  return str;
}


String *Item_func_json_unquote::val_str(String *str)
{
  json_engine_t je;
  int c_len= JSON_ERROR_OUT_OF_SPACE;
  String *js;

  if (!(js= read_json(&je)))
    return NULL;

  if (unlikely(je.s.error) || je.value_type != JSON_VALUE_STRING)
    return return_as_is(str, js);

  int buf_len= je.value_len;
  if (js->charset()->cset != my_charset_utf8mb4_bin.cset)
  {
    /*
      json_unquote() will be transcoding between charsets. We don't know
      how much buffer space we'll need. Assume that each byte in the source
      will require mbmaxlen bytes in the output.
    */
    buf_len *= my_charset_utf8mb4_bin.mbmaxlen;
  }

  str->length(0);
  str->set_charset(&my_charset_utf8mb4_bin);

  if (str->realloc_with_extra_if_needed(buf_len) ||
      (c_len= json_unescape(js->charset(),
        je.value, je.value + je.value_len,
        &my_charset_utf8mb4_bin,
        (uchar *) str->ptr(), (uchar *) (str->ptr() + buf_len))) < 0)
    goto error;

  str->length(c_len);
  return str;

error:
  if (current_thd)
  {
    if (c_len == JSON_ERROR_OUT_OF_SPACE)
      my_error(ER_OUTOFMEMORY, MYF(0), buf_len);
    else if (c_len == JSON_ERROR_ILLEGAL_SYMBOL)
      push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_JSON_BAD_CHR, ER_THD(current_thd, ER_JSON_BAD_CHR),
                          0, "unquote", 0);
  }
  return return_as_is(str, js);
}


static int alloc_tmp_paths(THD *thd, uint n_paths,
                           json_path_with_flags **paths, String **tmp_paths)
{
  if (n_paths > 0)
  {
    if (*tmp_paths == 0)
    {
      MEM_ROOT *root= thd->active_stmt_arena_to_use()->mem_root;

      *paths= (json_path_with_flags *) alloc_root(root,
          sizeof(json_path_with_flags) * n_paths);

      *tmp_paths= new (root) String[n_paths];
      if (*paths == 0 || *tmp_paths == 0)
        return 1;

      for (uint c_path=0; c_path < n_paths; c_path++)
        (*tmp_paths)[c_path].set_charset(&my_charset_utf8mb3_general_ci);
    }

    return 0;
  }

  /* n_paths == 0 */
  *paths= 0;
  *tmp_paths= 0;
  return 0;
}


static void mark_constant_paths(json_path_with_flags *p,
                                Item** args, uint n_args)
{
  uint n;
  for (n= 0; n < n_args; n++)
    p[n].set_constant_flag(args[n]->const_item());
}


Item_json_str_multipath::~Item_json_str_multipath()
{
  if (tmp_paths)
  {
    for (uint i= n_paths; i>0; i--)
      tmp_paths[i-1].free();
  }
}


bool Item_json_str_multipath::fix_fields(THD *thd, Item **ref)
{
  if (!tmp_paths)
  {
    /*
      Remember the number of paths and allocate required memory on first time
      the method fix_fields() is invoked. For prepared statements the method
      fix_fields can be called several times for the same item because its
      clean up is performed every item a prepared statement finishing its
      execution. In result, the data member fixed is reset and the method
      fix_field() is invoked on next time the same prepared statement be
      executed. On the other side, any memory allocations on behalf of
      the prepared statement must be performed only once on its first execution.
      The data member tmp_path is kind a guard to do these activities only once
      on first time the method fix_field() is called.
    */
    n_paths= get_n_paths();

    if (alloc_tmp_paths(thd, n_paths, &paths, &tmp_paths))
      return true;
  }

#ifdef PROTECT_STATEMENT_MEMROOT
  /*
   Check that the number of paths remembered on first run of a statement
   never changed later.
  */
  DBUG_ASSERT(n_paths == get_n_paths());
#endif

  return Item_str_func::fix_fields(thd, ref);
}


bool Item_func_json_extract::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);

  /* *2 accounts for LOOSE json_nice() formatting (spaces after : and ,). */
  ulonglong char_length=
    (ulonglong) args[0]->max_char_length() * (arg_count - 1) * 2;

  if (arg_count > 2)
  {
    /* Multiple paths: result is wrapped as [val1, val2, ...]. */
    char_length+= 2 + (arg_count - 2) * 2;
  }

  fix_char_length_ulonglong(char_length);
  mark_constant_paths(paths, args+1, arg_count-1);
  set_maybe_null();
  return FALSE;
}


static int path_exact(const json_path_with_flags *paths_list, int n_paths,
                       const json_path_t *p, json_value_types vt,
                       const int *array_size_counter)
{
  int count_path= 0;
  for (; n_paths > 0; n_paths--, paths_list++)
  {
    if (json_path_compare(&paths_list->p, p, vt, array_size_counter) == 0)
      count_path++;
  }
  return count_path;
}


static bool path_ok(const json_path_with_flags *paths_list, int n_paths,
                    const json_path_t *p, json_value_types vt,
                    const int *array_size_counter)
{
  for (; n_paths > 0; n_paths--, paths_list++)
  {
    if (json_path_compare(&paths_list->p, p, vt, array_size_counter) >= 0)
      return TRUE;
  }
  return FALSE;
}


String *Item_func_json_extract::read_json(String *str,
                                          json_value_types *type,
                                          char **out_val, int *value_len)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, sav_je;
  json_path_t p;
  const uchar *value;
  uint32 copy_start, copy_len;
  int not_first_value= 0, count_path= 0;
  uint n_arg;
  int possible_multiple_values;
  /* How far down the deepest of the values written out goes. */
  int deepest_value= 0;
  /* The same, where the answer is read back instead of counted. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;
  int array_size_counter[JSON_DEPTH_LIMIT];
  uint has_negative_path= 0;
  THD *thd;
  Json_source_watch watch;

  m_marks.clear();

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;

  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  watch.take(js);
  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    if (!c_path->parsed)
    {
      c_path->p.types_used= JSON_PATH_KEY_NULL;
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-1));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto return_null;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }

    if (args[n_arg]->null_value)
      goto return_null;
  }

  possible_multiple_values= arg_count > 2 ||
    (paths[0].p.types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD |
                              JSON_PATH_ARRAY_RANGE));

  *type= possible_multiple_values ? JSON_VALUE_ARRAY : JSON_VALUE_NULL;

  if (str)
  {
    str->set_charset(js->charset());
    str->length(0);

    if (possible_multiple_values && str->append('['))
      goto error;
  }

  DBUG_ASSERT(watch.unchanged(js));
  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);
  /*
    This walk is now the only one there is, so it is the one that has to
    notice being killed.  It used to be worth noticing only over the
    result, that being read separately afterwards; the reading of the
    document itself went uninterrupted however long the document was.
  */
  je.killed_ptr= (uint32_t *) &thd->killed;

  while (json_get_path_next(&je, &p) == 0)
  {
    if (has_negative_path && je.value_type == JSON_VALUE_ARRAY &&
        json_skip_array_and_count(&je,
                                  array_size_counter + (p.last_step - p.steps)))
      goto error;

    if (!(count_path= path_exact(paths, arg_count-1, &p, je.value_type,
                                 array_size_counter)))
      continue;

    value= je.value_begin;

    if (*type == JSON_VALUE_NULL)
    {
      *type= je.value_type;
      *out_val= (char *) je.value;
      *value_len= je.value_len;
    }
    if (!str)
    {
      /* If str is NULL, we only care about the first found value. */
      goto return_ok;
    }

    if ((not_first_value && str->append(STRING_WITH_LEN(json_loose_comma))) ||
        (not_first_value && DBUG_IF("json_extract_comma_out_of_memory")))
      goto error;

    copy_start= str->length();

    if (json_value_scalar(&je))
    {
      /*
        A scalar is punctuated with nothing, so however it is written in
        the document is how it is written in the loose form as well, and
        it is copied across as it stands - which is the copy json_nice()
        used to make of it, made here instead.  Copied and not written,
        so it must not be converted on the way: it came out of the
        document, and the document is in the character set the result is
        being built in.
      */
      if (append_simple(str, value, je.value_end - value) ||
          DBUG_IF("json_extract_scalar_out_of_memory"))
        goto error;
    }
    else
    {
      /*
        A container is written out rather than copied.  How it looks in
        the document is not how the loose form looks, and finding that
        out by writing the whole result and reading it back again is the
        reading being done away with here.

        The walk over the value is the walk this loop was going to make
        anyway - json_skip_level() went over exactly these characters
        without writing anything down.
      */
      if (possible_multiple_values)
        sav_je= je;
      /*
        How far down this value goes, counted from itself rather than
        from the document it was cut out of.  Reading a container has
        already put it on the stack, so the level here is the value's
        own outermost one, and what the walk reaches above that is what
        it holds.  The bracket written round the whole answer sits one
        further out again - see where it is closed.
      */
      {
        int base= je.stack_p, reached= je.stack_p;

        if (json_walk_nice_value(&je, str, reached))
          goto error;
        if (reached - base + 1 > deepest_value)
          deepest_value= reached - base + 1;
      }
      if (possible_multiple_values)
        je= sav_je;
    }

    /*
      A value that matched more than one of the paths asked for is
      written once and then repeated from where it was written, rather
      than being formatted again or held anywhere of its own.  The room is
      taken first, so that the read below is from a buffer that has
      already finished moving.
    */
    copy_len= str->length() - copy_start;
    while (--count_path)
    {
      if (str->append(STRING_WITH_LEN(json_loose_comma)) ||
          str->reserve(copy_len))
        goto error;
      str->q_append(str->ptr() + copy_start, copy_len);
    }

    not_first_value= 1;

    if (!possible_multiple_values)
    {
      /*
        The rest of the document is parsed only to check that it is one.
        Not done at all where the item has already attested that the
        value is_valid.
      */
      if (!args[0]->is_valid_json())
      {
        while (json_scan_next(&je) == 0) {}
      }
      break;
    }
  }

  if (unlikely(je.s.error))
    goto error;

  if (!not_first_value)
  {
    /* Nothing was found. */
    goto return_null;
  }

  if (possible_multiple_values && str->append(']'))
    goto error; /* Out of memory. */

  /*
    In a character set that cannot encode the punctuation this function
    writes - see is_json_compatible_charset() - what has been composed is
    not a document and no reasoning about it can make it one, the
    brackets holding the values having come out as national letters.
    Such a set can still hold a document, so the walk above succeeded
    and there is nothing else to notice it by.  Reading the result back
    is what has always noticed it, and where it cannot encode that
    reading stays exactly where it was.
  */
  /*
    And where the bracket written round the values takes the answer
    deeper than a document is allowed to go.  That bracket is a level
    this function adds: every value in it was measured against the
    document it came out of, where the bracket did not exist.  A
    released server found this out by reading the answer back and being
    refused, so being refused by the same reading is what it is owed -
    working it out here and complaining directly would put the
    complaint somewhere else in the text.
  */
  if (!is_json_compatible_charset(str->charset()) ||
      (possible_multiple_values && deepest_value + 1 >= JSON_DEPTH_LIMIT))
  {
    js= str;
    json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uint32_t *) &thd->killed;

    if (json_nice(&je, &tmp_js, Item_func_json_format::LOOSE, &read_back_depth))
      goto error;

    /*
      Both marks come from the reading back, for the reason given where
      Item_func_json_insert::val_str() marks the same answer, and so
      does the depth.
    */
    m_marks.set(&tmp_js, true, true, read_back_depth);
    return &tmp_js;
  }

  /*
    Nothing is read back here.  Every value in the result was written
    out in the loose form while the document was being walked, and the
    brackets that hold them are written the same way, so the result is
    already written as reading it back would have written it.  And it is
    a document because each part of it was read as one on the way in.

    Unlike the six that EDIT a document, this asks nothing of the
    argument: nothing of it is kept.  What comes back is written here
    out of values this function itself walked to and read, so the only
    thing that can be wrong with it is what holds those values
    together - which is why the one condition above is whether the
    brackets can be written at all, and why there is no trust predicate
    here to ask.  m_marks.set() reads it back in a debug build all the
    same.

    This is also why the result is returned in the caller's own
    buffer now.  It used to be returned in tmp_js, which is the
    scratch the argument may have been read into - so the answer was
    being written over the document it was made from, and only got away
    with it because the document had been finished with.

    How deep it goes was counted as it was written: each value put in
    was walked, and the deepest of them is the deepest the answer goes,
    plus the bracket written round them where there is one.
  */
  m_marks.set(str, true, true,
              (uint) deepest_value + (possible_multiple_values ? 1 : 0));
  return str;

return_ok:
  /*
    A caller that passed no buffer wanted the value picked out, not
    written out, and reads it through out_val.  What comes back only has
    to say that something was found.
  */
  return &tmp_js;

error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}


String *Item_func_json_extract::val_str(String *str)
{
  json_value_types type;
  char *value;
  int value_len;
  return read_json(str, &type, &value, &value_len);
}


longlong Item_func_json_extract::val_int()
{
  json_value_types type;
  char *value;
  int value_len;
  longlong i= 0;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_NUMBER:
      case JSON_VALUE_STRING:
      {
        char *end;
        int err;
        i= collation.collation->strntoll(value, value_len, 10, &end, &err);
        break;
      }
      case JSON_VALUE_TRUE:
        i= 1;
        break;
      default:
        i= 0;
        break;
    };
  }
  return i;
}


double Item_func_json_extract::val_real()
{
  json_value_types type;
  char *value;
  int value_len;
  double d= 0.0;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_STRING:
      case JSON_VALUE_NUMBER:
      {
        char *end;
        int err;
        d= collation.collation->strntod(value, value_len, &end, &err);
        break;
      }
      case JSON_VALUE_TRUE:
        d= 1.0;
        break;
      default:
        break;
    };
  }

  return d;
}


my_decimal *Item_func_json_extract::val_decimal(my_decimal *to)
{
  json_value_types type;
  char *value;
  int value_len;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_STRING:
      case JSON_VALUE_NUMBER:
      {
        my_decimal *res= decimal_from_string_with_check(to, collation.collation,
                                                        value,
                                                        value + value_len);
        null_value= res == NULL;
        return res;
      }
      case JSON_VALUE_TRUE:
        int2my_decimal(E_DEC_FATAL_ERROR, 1, false/*unsigned_flag*/, to);
        return to;
      case JSON_VALUE_OBJECT:
      case JSON_VALUE_ARRAY:
      case JSON_VALUE_FALSE:
      case JSON_VALUE_UNINITIALIZED:
      case JSON_VALUE_NULL:
        int2my_decimal(E_DEC_FATAL_ERROR, 0, false/*unsigned_flag*/, to);
        return to;
    };
  }
  DBUG_ASSERT(null_value);
  return 0;
}



bool Item_func_json_contains::fix_length_and_dec(THD *thd)
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  set_maybe_null();
  if (arg_count > 2)
    path.set_constant_flag(args[2]->const_item());
  return Item_bool_func::fix_length_and_dec(thd);
}


static int find_key_in_object(json_engine_t *j, json_string_t *key)
{
  const uchar *c_str= key->c_str;

  while (json_scan_next(j) == 0 && j->state != JST_OBJ_END)
  {
    DBUG_ASSERT(j->state == JST_KEY);
    if (json_key_matches(j, key))
      return TRUE;
    if (json_skip_key(j))
      return FALSE;
    key->c_str= c_str;
  }

  return FALSE;
}


static int check_contains(json_engine_t *js, json_engine_t *value)
{
  json_engine_t loc_js;
  bool set_js;
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  return dbug_json_check_min_stack_requirement(););
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  switch (js->value_type)
  {
  case JSON_VALUE_OBJECT:
  {
    json_string_t key_name;

    if (value->value_type != JSON_VALUE_OBJECT)
      return FALSE;

    loc_js= *js;
    set_js= FALSE;
    json_string_set_cs(&key_name, value->s.cs);
    while (json_scan_next(value) == 0 && value->state != JST_OBJ_END)
    {
      const uchar *k_start, *k_end;

      DBUG_ASSERT(value->state == JST_KEY);
      k_start= value->s.c_str;
      do
      {
        k_end= value->s.c_str;
      } while (json_read_keyname_chr(value) == 0);

      if (unlikely(value->s.error) || json_read_value(value))
        return FALSE;

      if (set_js)
        *js= loc_js;
      else
        set_js= TRUE;

      json_string_set_str(&key_name, k_start, k_end);
      if (!find_key_in_object(js, &key_name) ||
          json_read_value(js) ||
          !check_contains(js, value))
        return FALSE;
    }

    return value->state == JST_OBJ_END && !json_skip_level(js);
  }
  case JSON_VALUE_ARRAY:
    if (value->value_type != JSON_VALUE_ARRAY)
    {
      loc_js= *value;
      set_js= FALSE;
      while (json_scan_next(js) == 0 && js->state != JST_ARRAY_END)
      {
        int c_level, v_scalar;
        DBUG_ASSERT(js->state == JST_VALUE);
        if (json_read_value(js))
          return FALSE;

        if (!(v_scalar= json_value_scalar(js)))
          c_level= json_get_level(js);

        if (set_js)
          *value= loc_js;
        else
          set_js= TRUE;

        if (check_contains(js, value))
        {
          if (json_skip_level(js))
            return FALSE;
          return TRUE;
        }
        if (unlikely(value->s.error) || unlikely(js->s.error) ||
            (!v_scalar && json_skip_to_level(js, c_level)))
          return FALSE;
      }
      return FALSE;
    }
    /* else */
    loc_js= *js;
    set_js= FALSE;
    while (json_scan_next(value) == 0 && value->state != JST_ARRAY_END)
    {
      DBUG_ASSERT(value->state == JST_VALUE);
      if (json_read_value(value))
        return FALSE;

      if (set_js)
        *js= loc_js;
      else
        set_js= TRUE;
      if (!check_contains(js, value))
        return FALSE;
    }

    return value->state == JST_ARRAY_END;

  case JSON_VALUE_STRING:
    if (value->value_type != JSON_VALUE_STRING)
      return FALSE;
    /*
       TODO: make proper json-json comparison here that takes excipient
             into account.
     */
    return value->value_len == js->value_len &&
           memcmp(value->value, js->value, value->value_len) == 0;
  case JSON_VALUE_NUMBER:
    if (value->value_type == JSON_VALUE_NUMBER)
    {
      double d_j, d_v;
      char *end;
      int err;

      d_j= js->s.cs->strntod((char *) js->value, js->value_len, &end, &err);;
      d_v= value->s.cs->strntod((char *) value->value, value->value_len, &end, &err);;

      return (fabs(d_j - d_v) < 1e-12);
    }
    else
      return FALSE;

  default:
    break;
  }

  /*
    We have these not mentioned in the 'switch' above:

    case JSON_VALUE_TRUE:
    case JSON_VALUE_FALSE:
    case JSON_VALUE_NULL:
  */
  return value->value_type == js->value_type;
}


bool Item_func_json_contains::val_bool()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, ve;
  int result;
  THD *thd;
  Json_source_watch watch;

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  watch.take(js);
  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  DBUG_ASSERT(watch.unchanged(js));
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (arg_count>2) /* Path specified. */
  {
    int array_counters[JSON_DEPTH_LIMIT]= {0};
    if (!path.parsed)
    {
      String *s_p= args[2]->val_str(&tmp_path);
      if (!s_p)
        goto return_null;
      if (path_setup_nwc(&path.p,
                         def_path_charset(s_p->charset(), js->charset()),
                         (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->end()))
      {
        report_path_error(s_p, &path.p, 2);
        goto return_null;
      }
      path.parsed= path.constant;
    }
    if (args[2]->null_value)
      goto return_null;

    path.cur_step= path.p.steps;
    DBUG_ASSERT(watch.unchanged(js));
    if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
    {
      if (je.s.error)
      {
        ve.s.error= 0;
        goto error;
      }

      return FALSE;
    }
  }

  json_scan_start(&ve, val->charset(),(const uchar *) val->ptr(),
                  (const uchar *) val->end());
  ve.killed_ptr= (uint32_t *) &thd->killed;

  if (json_read_value(&je) || json_read_value(&ve))
    goto error;

  result= check_contains(&je, &ve);
  if (unlikely(je.s.error || ve.s.error))
    goto error;

  return result;

error:
  if (je.s.error)
    report_json_error(js, &je, 0);
  if (ve.s.error)
    report_json_error(val, &ve, 1);
return_null:
  null_value= 1;
  return 0;
}


bool Item_func_json_contains_path::fix_fields(THD *thd, Item **ref)
{
  /*
    See comments on Item_json_str_multipath::fix_fields regarding
    the aim of the condition 'if (!tmp_paths)'.
  */
  if (!tmp_paths)
  {
    if (alloc_tmp_paths(thd, arg_count-2, &paths, &tmp_paths) ||
        (p_found= (bool *) alloc_root(thd->active_stmt_arena_to_use()->mem_root,
                                       (arg_count-2)*sizeof(bool))) == NULL)
      return true;
  }

  return Item_int_func::fix_fields(thd, ref);
}


bool Item_func_json_contains_path::fix_length_and_dec(THD *thd)
{
  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;
  set_maybe_null();
  mark_constant_paths(paths, args+2, arg_count-2);
  return Item_bool_func::fix_length_and_dec(thd);
}

Item_func_json_contains_path::~Item_func_json_contains_path()
{
  if (tmp_paths)
  {
    for (uint i= arg_count-2; i>0; i--)
      tmp_paths[i-1].free();
    tmp_paths= 0;
  }
}


static int parse_one_or_all(const Item_func *f, Item *ooa_arg,
                            bool *ooa_parsed, bool ooa_constant, bool *mode_one)
{
  if (!*ooa_parsed)
  {
    char buff[20];
    String *res, tmp(buff, sizeof(buff), &my_charset_bin);
    if ((res= ooa_arg->val_str(&tmp)) == NULL)
      return TRUE;

    *mode_one=eq_ascii_string(res->charset(), "one",
                             res->ptr(), res->length());
    if (!*mode_one)
    {
      if (!eq_ascii_string(res->charset(), "all", res->ptr(), res->length()))
      {
        THD *thd= current_thd;
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_JSON_ONE_OR_ALL, ER_THD(thd, ER_JSON_ONE_OR_ALL),
                            f->func_name());
        *mode_one= TRUE;
        return TRUE;
      }
    }
    *ooa_parsed= ooa_constant;
  }
  return FALSE;
}


#ifdef DUMMY
longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;
  THD *thd;

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto return_null;

  result= !mode_one;
  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    int array_counters[JSON_DEPTH_LIMIT]= {0};
    json_path_with_flags *c_path= paths + n_arg - 2;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-2));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto null_return;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }

    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uint32_t *) &thd->killed;

    c_path->cur_step= c_path->p.steps;
    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto js_error;

      if (!mode_one)
      {
        result= 0;
        break;
      }
    }
    else if (mode_one)
    {
      result= 1;
      break;
    }
  }


  return result;

js_error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}
#endif /*DUMMY*/

bool Item_func_json_contains_path::val_bool()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;
  json_path_t p;
  /*
    Initialization force not required after gcc 13.3 where it
    correctly sees that an uninitialized read of n_found doesn't occur
    with mode_one being true.
  */
  int UNINIT_VAR(n_found);
  int array_sizes[JSON_DEPTH_LIMIT];
  uint has_negative_path= 0;
  /* Asked once: the walk below reaches it on every step. */
  bool js_attested;
  THD *thd;
  Json_source_watch watch;

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  watch.take(js);
  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto null_return;;

  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 2;
    c_path->p.types_used= JSON_PATH_KEY_NULL;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-2));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto null_return;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }
    if (args[n_arg]->null_value)
      goto null_return;
  }

  DBUG_ASSERT(watch.unchanged(js));
  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (!mode_one)
  {
    bzero(p_found, (arg_count-2) * sizeof(bool));
    n_found= arg_count - 2;
  }

  result= 0;
  js_attested= args[0]->is_valid_json();
  while (json_get_path_next(&je, &p) == 0)
  {
    int n_path= arg_count - 2;
    if (has_negative_path && je.value_type == JSON_VALUE_ARRAY &&
        json_skip_array_and_count(&je, array_sizes + (p.last_step - p.steps)))
    {
      result= 1;
      break;
    }

    json_path_with_flags *c_path= paths;
    for (; n_path > 0; n_path--, c_path++)
    {
      if (json_path_compare(&c_path->p, &p, je.value_type, array_sizes) >= 0)
      {
        if (mode_one)
        {
          result= 1;
          break;
        }
        /* mode_all */
        if (p_found[n_path-1])
          continue; /* already found */
        if (--n_found == 0)
        {
          result= 1;
          break;
        }
        p_found[n_path-1]= TRUE;
      }
    }

    /*
      Nothing left that could unsettle the answer.  The walk goes on past
      a settled one only so that a fault later in the document can take
      it back, and a value the item has attested is_valid has no such
      fault to be found.
    */
    if (result && js_attested)
      break;
  }

  if (likely(je.s.error == 0))
    return result;

  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


/*
  This reproduces behavior according to the former
  Item_func_conv_charset::is_json_type() which returned args[0]->is_json_type().
  JSON functions with multiple string input with different character sets
  wrap some arguments into Item_func_conv_charset. So the former
  Item_func_conv_charset::is_json_type() took the JSON propery from args[0],
  i.e. from the original argument before the conversion.
  This is probably not always correct because an *explicit*
  `CONVERT(arg USING charset)` is actually a general purpose string
  expression, not a JSON expression.
*/
bool is_json_type(const Item *item)
{
  for ( ; ; )
  {
    if (Type_handler_json_common::is_json_type_handler(item->type_handler()))
      return true;
    const Item_func_conv_charset *func;
    if (!(func= dynamic_cast<const Item_func_conv_charset*>(item->real_item())))
      return false;
    item= func->arguments()[0];
  }
  return false;
}


/*
  What splicing values into a document has left the document able to say
  about itself.  Starts out saying both and is only ever cleared: one
  value that cannot be attested to is enough, however many others went
  in cleanly.

  Named for what holds rather than for what went wrong, and named after
  the questions an item answers, so that a value's own answer and what
  became of it here read the same way round.  A document is what the
  the loose form is a form OF a document, so nothing clears the second
  without clearing the first.

  Every caller keeps a set, so every value put in is accounted for
  whether or not the caller ends up believing what they say.  A caller
  that reads its whole answer back afterwards has a stronger answer than
  anything collected on the way and takes that instead; it still hands
  one of these over, there being nowhere else for a value's own answer
  to go.
*/
struct Json_splice_marks
{
  bool is_valid;  /* everything that went in read as a document */
  bool is_nice;   /* everything that went in was written the loose way */
  /*
    Set when the two above were cleared because a value goes past the
    depth limit only once it is in place.  Kept apart from the other
    reasons, so that a caller which still reads its whole answer back
    can check the depth it reckoned at the splice against what that
    reading found: a reading that reached the end is an answer inside
    the limit, and nothing should have said otherwise.

    Only that check ever reads it, so it is only there where it can be
    read.  That is not the same set of builds as the debug one: an
    assertion survives into a build with DBUG_OFF set where it was asked
    to print rather than to stop, and the expression it is given is
    compiled there.
  */
#ifdef DBUG_ASSERT_EXISTS
  bool is_deep;
#endif
  /*
    The deepest any value put in reaches, counted from the outside of
    the document being composed rather than from the value itself.  A
    caller starts it at the depth of whatever it is writing the values
    inside, so that it holds for a document with no values in it too,
    and it is then the depth of the whole composed answer - every other
    part of that answer being punctuation the caller wrote itself.

    Wrong only ever upwards, like the answers it is made of: a value
    taken on trust contributes the bound the trusting was done against
    rather than a measurement, and a bound is not smaller than the
    truth.
  */
  uint deepest;

  Json_splice_marks(uint depth)
   : is_valid(true), is_nice(true), deepest(depth)
  {
#ifdef DBUG_ASSERT_EXISTS
    is_deep= false;
#endif
  }
};


/*
  Appends a value whose type is JSON, which is spliced into the result
  as it stands instead of being quoted as a string.  Any complete JSON
  value counts, a bare number or string as much as an object: what the
  type buys the value is that its own punctuation is kept rather than
  escaped away.

  Being typed as JSON is not the same as being JSON.  The type comes
  from a check constraint, the constraint can be switched off for the
  duration of a statement, and the bytes it would have rejected stay in
  the column afterwards.  Nothing on the way from the column to here
  reads them.

  So the value is parsed as it is copied, but a value that does not
  parse is copied ALL THE SAME, with a note saying so.  It has always
  been copied, callers have always been able to see the result, and
  what a released server does is not something to take away from the
  people relying on it; the note is what is new.  Reading the result
  back is where a caller finds out, exactly as before.  What the parse
  is for is to know WHETHER to say anything - and, once the trust
  predicates exist, to keep an unparseable value from ever being taken
  on trust by a later reader.

  A value written in another character set is converted first, because
  the result is read in the character set it is being built in, not in
  the one the value arrived in.  The quoting arm converts the same way,
  through json_escape(); a value spliced as JSON cannot go through
  json_escape() without its punctuation being escaped along with
  everything else, so it is converted whole and then parsed as whatever
  it has become.  A character set that cannot hold the value leaves the
  bytes where they were, again because that is what was done before.

  'depth' is how many structures the value will sit inside once it has
  been spliced.  Its own nesting adds to that, and it is the total that
  the scanner's limit applies to.

  'marks' is where the parse goes, the bytes having gone in whatever it
  found.  See Json_splice_marks.

  'is_valid' and 'is_nice' are what the item handing the value over
  says about it.  is_valid true is the one case where the parse can be
  left undone; is_nice false is what the writing out further down is
  for.  'need_nice' is the caller saying that what it composes is what
  it returns, so a value that is not is_nice has to be made so.

  'value_depth' is how deep the item says the value goes, or
  JSON_DEPTH_UNKNOWN where it does not say.  It is only ever read
  alongside is_valid.
*/
static int append_json_typed_value(String *str, const String *sv, uint depth,
                                   const char *fname, int n_param,
                                   Json_splice_marks &marks,
                                   bool caller_reads_back, bool is_valid,
                                   bool is_nice, bool need_nice,
                                   uint value_depth)
{
  StringBuffer<STRING_BUFFER_USUAL_SIZE> cnv;
  const char *ptr= sv->ptr();
  size_t length= sv->length();
  json_engine_t je;
  int max_level= 0;
  bool reformat;
  uint32 sav_len;
  /*
    Whether the bytes at 'ptr' are written in the character set the
    result is being built in.  What an item answers about a value is
    about the characters it holds, so this is what says whether the
    answer is about the bytes that are going in.
  */
  bool in_result_charset= my_charset_same(sv->charset(), str->charset());

  /*
    Bytes that carry no character set are left where they are, in both
    directions and for opposite reasons.  Going into a result that is
    bytes, there is no character set to convert to.  Coming out of a
    value that is bytes, String::copy() cannot convert at all -
    needs_conversion() answers false across that boundary, so the copy
    would relabel the bytes and change nothing else, which is the one
    thing this must not do.  The scan below still reads whatever gets
    appended in the character set of the result, so bytes that do not
    encode a JSON value there are still refused.
  */
  if (!in_result_charset &&
      sv->charset() != &my_charset_bin && str->charset() != &my_charset_bin)
  {
    uint errors;

    if (cnv.copy(sv->ptr(), sv->length(), sv->charset(), str->charset(),
                 &errors) ||
        DBUG_IF("json_splice_convert_out_of_memory"))
      return 1; /* Out of memory. */

    /*
      Only a conversion that lost nothing is worth having.  One that
      substituted a character has changed the value, and putting a
      changed value in would be worse than leaving the bytes as they
      arrived, which is what was always done with them.  The parse
      below reads them as the result will be read and says so.

      One that lost nothing kept the characters it was given, and being
      a document is a property of characters, so a value that arrived as
      one is still one after it - and still formatted the way it was, the
      loose form's spacing being characters like any other.  So what was
      answered about it before the conversion is answered about it
      after, and the shortcut below can have it.
    */
    if (!errors)
    {
      ptr= cnv.ptr();
      length= cnv.length();
      in_result_charset= true;
    }
  }

  /*
    A value somebody has already attested to is copied in without being
    read, which is the whole of what attesting buys.  Three things
    have to hold besides.

    It must be written the way the result is going to be written, or the
    caller must not care.  A value copied in as it stands brings its own
    spacing with it, so a result that has to come out in the loose form
    cannot take one that is not in it without reading it - which is what
    the writing out further down is for.

    It must be going in in the character set the result is read in -
    either because it arrived in it, or because it was converted into it
    just above without losing anything.  A lossy conversion, and equally
    the untouched copy made when there is no conversion to be had, leave
    bytes that nothing has attested to and fall through to the parse.

    And it must not make the result too deep to read back.  The value's
    own nesting is what the parse below measures, and the shortcut is not
    doing that - so it takes the smallest thing it has that the nesting
    cannot be more than.

    There are two such things and either alone would do.  The item may
    have worked the depth out while it was writing the value, in which
    case it says so.  And a value can only nest as deeply as it is long,
    whatever it holds: every level takes a character to open and one to
    close, so a value of n characters reaches at most n/2 levels, and
    there are at most length/mbminlen characters in it.  That second one
    is exact for the values where being wrong would matter most - a run
    of nothing but brackets is all opening and closing - and hopeless for
    a long shallow document, which is what the first one is for.

    Where the smaller of them still leaves the result inside the limit,
    the parse could not have found a breach to report and skipping it
    takes nothing away.  Where it does not, the value is read as before:
    giving up costs a reading and nothing else.
  */
  if (is_valid && (is_nice || !need_nice) && in_result_charset)
  {
    /* The most levels the value can turn out to have - see above. */
    uint bound= MY_MIN(value_depth,
                       (uint) (length / (2 * str->charset()->mbminlen)));

    if (depth + bound < JSON_DEPTH_LIMIT)
    {
      if (!is_nice)
        marks.is_nice= false;
      /*
        What the value was let in on, rather than what it turned out to
        be - nothing measured it.  A bound is not smaller than the
        truth, which is all a caller composing from these needs of it.
      */
      if (depth + bound > marks.deepest)
        marks.deepest= depth + bound;
      return append_simple(str, ptr, length);
    }
  }

  /*
    The deepest the value goes is read off the scanner as it runs rather
    than charged to it in advance.  Starting the scan with stack_p
    already raised looks like the shorter way to ask the same question,
    but json_scan_start() marks stack[0] as the end of the document and
    the scanner stops when it pops back to that mark, so raising the
    pointer past it leaves unwritten slots underneath and the scan never
    finishes.
  */
  /*
    A value that is not already written the loose way is written out
    again as it is read, where the caller has said that what it composes
    is what it returns.  The reading has to happen either way, and a
    reading that writes as it goes costs nothing more than one that does
    not - so the value arrives in the form the result needs instead of
    the caller reading its whole answer afterwards to put it in that
    form.

    Where the caller does NOT say that, the value is left exactly as it
    arrived.  Two quite different callers say nothing here.  One builds
    an array or an object and returns what it built, so a value put
    in as it stands is what a released server returns too and writing
    it out afresh would change an answer.  The other reads its whole
    result back before answering, and that reading is where the spacing
    gets settled - and it complains, when it has to, about positions in
    the text it read, so a value that arrived any longer or shorter than
    it used to would move them.

    A character set that cannot encode the punctuation cannot be written
    into at all, and is left alone for the same reason it is everywhere
    else here.
  */
  reformat= need_nice && !is_nice &&
           is_json_compatible_charset(str->charset());
  sav_len= str->length();

  json_scan_start(&je, str->charset(), (const uchar *) ptr,
                  (const uchar *) ptr + length);
  /*
    Every reading here has to let go of a killed query, and this one is
    no different for being over a value rather than over a document.
    json_scan_start() leaves the scanner deaf to a kill - it points at a
    word that is always zero - so a reading that is meant to hear one
    says so on the line after, as every other reading in this file does.

    It matters more here than the size of a value suggests.  A released
    server does no reading at all at this point: it copies the bytes in
    and goes on, so a kill arriving while a value is going in is heard
    at the very next step.  The reading is ours, and an unheard kill
    would be ours too - a stretch of work, as long as the value, that
    nothing could interrupt and that nobody had before.
  */
  je.killed_ptr= (uint32_t *) &current_thd->killed;

  if (reformat)
  {
    if (json_read_value(&je) || json_walk_nice_value(&je, str, max_level))
    {
      /*
        Nothing was read wrong, so what went wrong was the writing:
        there is no room for the answer and no answer to give.
      */
      if (!je.s.error)
        return 1;
    }
    else
    {
      /*
        The walk stops where the value ends, and a value is only a
        document when nothing follows it.
      */
      while (json_scan_next(&je) == 0)
      {}
    }

    /*
      Written out again only if it came out whole and fits where it is
      going.  Either way the caller is owed what a released server
      returns, so anything else is taken back off and the bytes go in
      as they arrived - which is also what leaves the complaint below
      about the same text it has always been about.
    */
    if (je.s.error || max_level + (int) depth >= JSON_DEPTH_LIMIT)
    {
      str->length(sav_len);
      reformat= false;
    }
  }
  else
  {
    while (json_scan_next(&je) == 0)
    {
      if (je.stack_p > max_level)
        max_level= je.stack_p;
    }
  }

  /*
    Spliced as it stands, so the document being built is written the
    loose way only if this value was.  Nothing is measured to find that
    out - the item was asked, and one that says nothing is taken at its
    word.
  */
  if (!reformat && !is_nice)
    marks.is_nice= false;

  /*
    Too deep only once it is in place.  There is nothing wrong with the
    value itself, so a caller that reads its whole result back says it
    about the answer rather than about this one value - which is what it
    has always said, and where it has always said it.  All that is owed
    here is to stop such an answer being taken on trust.

    A caller that does not read back has nobody else to hear it from, so
    for that one the scanner is put into error and the note goes out as
    it always has.
  */
  if (je.s.error == 0 && max_level + (int) depth >= JSON_DEPTH_LIMIT)
  {
    marks.is_valid= marks.is_nice= false;
#ifdef DBUG_ASSERT_EXISTS
    marks.is_deep= true;
#endif
    if (!caller_reads_back)
      je.s.error= JE_DEPTH;
  }
  /*
    Measured rather than reckoned, this being the reading the shortcut
    above was for going without.  Only where the reading finished: what
    a walk that stopped early reached is about the part of the value it
    got through, and there is nothing to compose out of a value that
    did not read as one anyway.
  */
  else if (je.s.error == 0 && depth + (uint) max_level > marks.deepest)
    marks.deepest= depth + (uint) max_level;

  /*
    Said here rather than left to the caller: the callers report through
    the engine they scanned the DOCUMENT with, which knows nothing about
    what went wrong with a value, and reports nothing at all when that
    engine is not itself in error.

    A note and not a warning, because a warning becomes an error inside
    a statement running under strict mode, and a statement that used to
    finish would then stop finishing.  What is being added here is
    something to read, not a new way to fail.  json_valid() says the
    same thing at the same level for the same reason.
  */
  if (je.s.error)
  {
    marks.is_valid= marks.is_nice= false;
    report_json_error_ex(ptr, &je, fname, n_param,
                         Sql_condition::WARN_LEVEL_NOTE);
  }

  return reformat ? 0 : append_simple(str, ptr, length);
}


/*
  Writes a value that is not itself a document, as a JSON string when
  the value is one and bare otherwise.

  A character that cannot be written into a document at all leaves the
  value part written and stops there, which is what has always happened
  to it.  The caller is told which of the two failures it was, because
  only one of them is a reason to give up on the whole result.
*/
static json_append_result __attribute__((warn_unused_result))
append_escaped_value(String *str, const String *sv, bool quoted,
                     const char *fname, int n_param)
{
  json_append_result rc;

  if ((quoted && str->append('"')) ||
      DBUG_IF("json_value_open_quote_out_of_memory"))
    return JSON_APPEND_OOM;

  if ((rc= st_append_escaped(str, sv)))
  {
    if (rc == JSON_APPEND_BAD_CHR)
      report_bad_chr_note(fname, n_param + 1);
    return rc;
  }

  if ((quoted && str->append('"')) ||
      DBUG_IF("json_value_close_quote_out_of_memory"))
    return JSON_APPEND_OOM;

  return JSON_APPEND_OK;
}


__attribute__((warn_unused_result))
/*
  Appends one value to the document being built.

  'depth' is how many structures the value will end up inside, and it is
  consulted for EVERY value rather than only for one whose type is JSON:
  it is recorded before the type is looked at, because a value goes in
  where the caller says whatever it turns out to be.  The functions that
  BUILD a document pass 1, the value going directly inside the array or
  object they are making.  The functions that EDIT one pass the depth
  the path reached, taken off the scanner as je.stack_p and raised by
  whatever wrap they are adding - none of them passes 0.

  'fname' and 'n_param' name the function and the argument a complaint
  is about, and they reach BOTH arms: a value spliced in as a document
  complains through the trailing-text and depth notes, and one written
  out as a string complains through the bad-character note.
*/
static int append_json_value(String *str, Item *item, String *tmp_val,
                             uint depth, const char *fname, int n_param,
                             Json_splice_marks &marks,
                             bool caller_reads_back, bool need_nice)
{
  /*
    Said before anything is looked at, because it holds for whatever the
    value turns out to be: it is going in where the caller says, so the
    answer reaches at least that far down.  Only a value that is a
    document of its own reaches further, and that is the one arm below
    that has anything to add.

    It is not the caller's own depth restated.  A caller that wraps -
    putting a new array where a scalar was and the new value beside it -
    passes a depth one further down than anything it has written,
    and a value QUOTED at that depth would otherwise be counted nowhere
    at all.
  */
  if (depth > marks.deepest)
    marks.deepest= depth;

  /*
    And too deep before anything is looked at either, for the same
    reason.  A value put where the limit has already been reached is
    past it whatever it turns out to be: the arms below add whatever
    nesting the value has of its own and none of them takes any away.
    Only the arm that READS the value can say how much further down it
    reaches, so this is the whole of the question for the three that
    read nothing - a bare word, a null, and a value written out as a
    string.

    Saying so is all that is owed here.  A value only arrives this deep
    through a function that EDITS a document, by putting a new array
    where a value already sat; every one of those returns what it
    composed only while these marks stand, so taking them away puts it
    back on the reading that a released server always did, and that
    reading complains about the answer in the words it has always used.
    The functions that BUILD a document put their values one level down
    and cannot get here.
  */
  if (depth >= JSON_DEPTH_LIMIT)
  {
    marks.is_valid= marks.is_nice= false;
#ifdef DBUG_ASSERT_EXISTS
    marks.is_deep= true;
#endif
  }

  if (item->type_handler()->is_bool_type())
  {
    longlong v_int= item->val_int();
    const char *t_f;
    int t_f_len;

    if (item->null_value)
      goto append_null;

    if (v_int)
    {
      t_f= "true";
      t_f_len= 4;
    }
    else
    {
      t_f= "false";
      t_f_len= 5;
    }

    return str->append(t_f, t_f_len);
  }
  {
    String *sv= item->val_json(tmp_val);
    int rc;

    if (item->null_value)
      goto append_null;
    if (is_json_type(item))
      return append_json_typed_value(str, sv, depth, fname, n_param, marks,
                                     caller_reads_back, item->is_valid_json(),
                                     item->is_nice_json(), need_nice,
                                     item->last_depth());

    /*
      Being a document is what gets a value spliced rather than quoted,
      so an item answering for one it is not going to be asked about has
      lost track of which of the two it hands back.
    */
    DBUG_ASSERT(!item->is_valid_json());

    rc= append_escaped_value(str, sv, item->result_type() == STRING_RESULT,
                             fname, n_param);
    /*
      A character that could not be written leaves the value half
      written and the quote around it unclosed.  A caller that gives up
      here does not care; one that carries on is building something that
      no longer reads as a document.
    */
    if (rc == JSON_APPEND_BAD_CHR)
      marks.is_valid= marks.is_nice= false;
    return rc;
  }

append_null:
  return str->append(STRING_WITH_LEN("null"));
}


/* The same, reading the value out of a row rather than off an Item. */
__attribute__((warn_unused_result))
static int append_json_value_from_field(String *str,
  Item *i, Field *f, const uchar *key, size_t offset, String *tmp_val,
  uint depth, const char *fname, int n_param, Json_splice_marks &marks)
{
  /* Said up front for the reason given in append_json_value(). */
  if (depth > marks.deepest)
    marks.deepest= depth;

  if (i->type_handler()->is_bool_type())
  {
    longlong v_int= f->val_int(key + offset);
    const char *t_f;
    int t_f_len;

    if (f->is_null_in_record(key))
      goto append_null;

    if (v_int)
    {
      t_f= "true";
      t_f_len= 4;
    }
    else
    {
      t_f= "false";
      t_f_len= 5;
    }

    return str->append(t_f, t_f_len);
  }
  {
    String *sv= f->val_str(tmp_val, key + offset);
    int rc;

    if (f->is_null_in_record(key))
      goto append_null;
    if (is_json_type(i))
      /*
        The bytes come out of a record rather than off an item, so there
        is nobody here to ask about them - but the column they came out
        of was asked, once for all its rows.  A column of a table the
        server built for itself can say that its values read as
        documents, one producer having filled every row of it and every
        store into it having kept what it was given, and it can say that
        none of them arrived written any way but the loose one.  A column
        of any other table says neither, having nowhere to keep a yes.

        Nothing is written out again here.  The only callers that read a
        value out of a record are building a document and returning
        what they built, and a value put in as it stands is what a
        released server returns.

        It can say how deep they go as well, and that answer is a figure
        rather than a yes: the deepest any value written into the column
        has gone, which is at least as deep as this one.  What it saves
        is the only thing left to read a trusted value for - the reading
        that is skipped elsewhere is a validating one, and this one
        counts brackets and nothing else.  Without it the caller falls
        back on how long the value is, nothing nesting deeper than half
        its length, which is exact for a run of brackets and says
        nothing whatever about a long shallow document.
      */
      return append_json_typed_value(str, sv, depth, fname, n_param, marks,
                                     false, f->is_valid_json_static(),
                                     f->is_nice_json_static(), false,
                                     f->json_static_depth());

    rc= append_escaped_value(str, sv, i->result_type() == STRING_RESULT,
                             fname, n_param);
    if (rc == JSON_APPEND_BAD_CHR)
      marks.is_valid= marks.is_nice= false;
    return rc;
  }

append_null:
  return str->append(STRING_WITH_LEN("null"));
}


/*
  Writes a key, which is a JSON string and nothing else.

  A character that cannot be written into a document at all leaves the
  key part written and stops there, the same as it does in a value -
  and the caller is told which of the two failures it was for the same
  reason.  What the caller then does with a key it could not write is
  its own business: a constructor gives up on the whole document and an
  aggregate finishes the pair around what went in, but neither of them
  can say so without being told, and a key that could not be written is
  worth as much saying as a value that could not.
*/
static json_append_result append_json_keyname(String *str, Item *item,
                                              String *tmp_val,
                                              const char *fname, int n_param)
{
  json_append_result rc;
  String *sv= item->val_str(tmp_val);
  if (item->null_value)
    goto append_null;

  if (str->append('"') || DBUG_IF("json_keyname_quote_out_of_memory"))
    return JSON_APPEND_OOM;

  if ((rc= st_append_escaped(str, sv)))
  {
    if (rc == JSON_APPEND_BAD_CHR)
      report_bad_chr_note(fname, n_param + 1);
    return rc;
  }

  return (str->append(STRING_WITH_LEN(json_loose_colon)) ||
          DBUG_IF("json_keyname_colon_out_of_memory")) ?
         JSON_APPEND_OOM : JSON_APPEND_OK;

append_null:
  return (str->append('"') ||
          str->append(STRING_WITH_LEN(json_loose_colon))) ?
         JSON_APPEND_OOM : JSON_APPEND_OK;
}


/*
  How much room a value needs in a document being built, before any
  value has been seen and so out of what the argument says about itself.

  A value that is already a document goes in as it stands, but it is
  written out again with the rest and a space arrives after every
  separator it brought with it, so the room for it covers the writing
  the same way JSON_REMOVE asks for it to.  One written as text and not
  a document is written as a JSON string, and a character of one costs
  ten at the very worst - the allowance
  Item_func_json_quote::fix_length_and_dec() works out and the same one
  that applies here; a string carries no separators of its own, so the
  spacing has nothing to add to it.  A boolean is written as one of the
  two words 'true' and 'false', the longer of which is five characters.
  What is left is a number, which is written as itself.  A value that is
  not there at all is written as 'null', so nothing is ever shorter than
  four characters.

  The punctuation that goes around it is the caller's to ask for.

  Whether it IS a document has to be the question the writing asks, and
  not a shorter one that happens to agree most of the time.  Aggregating
  the arguments into one character set wraps whichever of them has to
  move, and the wrapper attests to itself when asked for a type handler
  - so a document that arrived wrapped would be priced as a string, at
  ten characters apiece for an escaping that the writing, which looks
  through the wrapper, is never going to do.  is_json_type() is that
  same look.
*/
static ulonglong json_value_reserve(Item *arg)
{
  const bool is_document= is_json_type(arg);
  ulonglong length;

  if (arg->result_type() == STRING_RESULT && !is_document)
    length= static_cast<ulonglong>(arg->max_char_length()) * 10 + 2;
  else if (arg->type_handler()->is_bool_type())
    length= 5;
  else if (is_document)
    length= static_cast<ulonglong>(arg->max_char_length()) * 2;
  else
    length= arg->max_char_length();

  return length < 4 ? 4 : length;
}


bool Item_func_json_array::fix_length_and_dec(THD *thd)
{
  ulonglong char_length= 2;
  uint n_arg;

  result_limit= 0;

  if (arg_count == 0)
  {
    THD* thd= current_thd;
    collation.set(thd->variables.collation_connection,
                  DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
    tmp_val.set_charset(thd->variables.collation_connection);
    max_length= 2;
    return FALSE;
  }

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return TRUE;

  for (n_arg=0 ; n_arg < arg_count ; n_arg++)
    char_length+= json_value_reserve(args[n_arg]) + 4;

  fix_char_length_ulonglong(char_length);
  tmp_val.set_charset(collation.collation);
  return FALSE;
}


String *Item_func_json_array::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint n_arg;
  /*
    The array written here is one structure of its own, which is where
    the values go inside - so that is where the reckoning starts, and an
    array with nothing in it is one deep for the brackets alone.
  */
  Json_splice_marks marks(1);

  m_marks.clear();

  str->length(0);
  str->set_charset(collation.collation);

  if (str->append('[') ||
      ((arg_count > 0) &&
       append_json_value(str, args[0], &tmp_val, 1, func_name(), 0, marks,
                          false, false)))
    goto err_return;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    if (str->append(STRING_WITH_LEN(json_loose_comma)) ||
        append_json_value(str, args[n_arg], &tmp_val, 1, func_name(),
                          (int) n_arg, marks, false, false))
      goto err_return;
  }

  if (str->append(']'))
    goto err_return;

  if (result_limit == 0)
    result_limit= current_thd->variables.max_allowed_packet;

  if (str->length() <= result_limit)
  {
    /*
      Nothing was read back, so what can be said about the array is only
      what was learned putting it together: the brackets and separators
      are written here in the loose form, the values that were
      quoted came out of json_escape() and are documents on their own,
      and the values that went in as they stand were either read as they
      went or passed by something that had already read them.
      Which of the two it was does not matter here - marks.is_valid and
      marks.is_nice are cleared by whichever of them did not hold, and
      that is the whole of what this has to go on.

      All of which holds only where the brackets written above are
      brackets - see is_json_compatible_charset().
    */
    bool is_json_compatible= is_json_compatible_charset(str->charset());

    m_marks.set(str, is_json_compatible && marks.is_valid,
                is_json_compatible && marks.is_nice, marks.deepest);
    return str;
  }

  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
      func_name(), result_limit);

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


bool Item_func_json_array_append::fix_length_and_dec(THD *thd)
{
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  /*
    The document is written out again around what is added to it, a
    space arriving after every separator that is copied, so the room for
    it has to cover the writing and not only the reading - the same
    allowance JSON_REMOVE asks for, adding the same spacing.
  */
  char_length= static_cast<ulonglong>(args[0]->max_char_length()) * 2;

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg/2].set_constant_flag(args[n_arg]->const_item());
    char_length+= json_value_reserve(args[n_arg+1]) + 4;
  }

  fix_char_length_ulonglong(char_length);
  set_maybe_null();
  return FALSE;
}


/*
  Returns the document in 'from' as this function's answer instead of
  reading it again to find out what it is.

  A caller gets here only where everything that went into the answer is
  attested to, so what it returns is a document written the loose
  way and nothing has to look at it to say so.  Which of the two
  the answer is already sitting in is the one thing the callers differ
  in, so the copy is made only where it is owed.

  The depth is the caller's to work out - it is the only part of this
  that depends on what the function did to the document - and 'to' is
  what comes back, or NULL where the buffer would not grow.  Reporting
  that is the caller's as well: they do not all report it in the same
  place, some of them having a document engine to complain through and
  some not.
*/

String *Item_json_func::return_json(String *to, const String *from,
                                       uint depth)
{
  if ((from != to && to->copy(from->ptr(), from->length(), from->charset())) ||
      DBUG_IF("json_return_out_of_memory"))
    return NULL;

  null_value= 0;
  m_marks.set(to, true, true, depth);
  return to;
}


String *Item_func_json_array_append::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  size_t str_rest_len;
  const uchar *ar_end;
  const char *js_end;
  THD *thd;
  String *const to= str;
  bool compose_final;
  uint depth;
  /* How deep the argument item attested its document to go. */
  uint js_depth;
  /* How deep the reading back below found the answer to go. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;
  Json_source_watch watch;
  /*
    Cleared by anything spliced in that leaves is_valid or is_nice
    false for the answer, the depth a path reaches among it.  See
    Item_func_json_insert::val_str() for what the fourth of them
    starts at and why.
  */
  Json_splice_marks splice(0);

  DBUG_ASSERT(fixed());
  m_marks.clear();

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  /*
    Whether what is composed here is what will be returned, or only
    what a reading back at the end will be made from.  Asked once,
    before anything is composed, for the reason given where
    Item_func_json_insert::val_str() asks it.
  */
  compose_final= document_arg_composes_final(args[0], js);
  /* Taken here for the reason given at the same place there. */
  js_depth= args[0]->last_depth();

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT]= {0};
    json_path_with_flags *c_path= paths + n_path;

    /*
      Taken before the path is worked out, and afresh every time round.
      See Item_func_json_insert::val_str().
    */
    watch.take(js);

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (!s_p)
        goto return_null;
      if (path_setup_nwc(&c_path->p,
                         def_path_charset(s_p->charset(), js->charset()),
                         (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &c_path->p, n_arg);
        goto return_null;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    DBUG_ASSERT(watch.unchanged(js));
    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uint32_t *) &thd->killed;

    /*
      Where the document ends, read where the walk over it starts rather
      than after the value has been worked out, so that the end copied
      from is the end that was walked to.  See Json_source_watch.
    */
    js_end= js->end();

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;

      goto return_null;
    }

    if (json_read_value(&je))
      goto js_error;

    /*
      How many structures the value will sit inside once it is in
      place.  Read off here because walking to the end of the array
      below pops the scanner back out of it.

      Reading a container puts it on the stack; reading a scalar does
      not.  So for an array, and for an object about to be wrapped in a
      new one, the scanner is already counting the structure the value
      is going into, and counting it again would refuse a document that
      fits.  Only a scalar being wrapped needs the one added, the new
      array being a structure the scanner has never seen.
    */
    depth= (uint) je.stack_p + (json_value_scalar(&je) ? 1 : 0);

    str->length(0);
    str->set_charset(js->charset());
    if (str->reserve(js->length() + 8, 1024))
      goto return_null; /* Out of memory. */

    if (je.value_type == JSON_VALUE_ARRAY)
    {
      int n_items;
      if (json_skip_level_and_count(&je, &n_items))
        goto js_error;

      ar_end= je.s.c_str - je.sav_c_len;
      str_rest_len= js->length() - (ar_end - (const uchar *) js->ptr());
      str->q_append(js->ptr(), ar_end-(const uchar *) js->ptr());
      if (n_items)
        str->append(", ", 2);
      if (append_json_value(str, args[n_arg+1], &tmp_val, depth, func_name(),
                            (int) n_arg + 1, splice,
                            !compose_final, compose_final))
        goto return_null; /* Out of memory. */

      if (str->reserve(str_rest_len, 1024))
        goto return_null; /* Out of memory. */
      str->q_append((const char *) ar_end, str_rest_len);
    }
    else
    {
      const uchar *c_from, *c_to;

      /* Wrap as an array. */
      str->q_append(js->ptr(), (const char *) je.value_begin - js->ptr());
      c_from= je.value_begin;

      if (je.value_type == JSON_VALUE_OBJECT)
      {
        /*
          Wrapping puts what was already there inside a new array, so
          the KEPT value goes down a level too - and how far down it
          already reached was never measured, it having been copied
          rather than read.  Say so and let the answer be read back.
        */
        splice.is_valid= splice.is_nice= false;
        if (json_skip_level(&je))
          goto js_error;
        c_to= je.s.c_str;
      }
      else
        c_to= je.value_end;

      /*
        The brackets and the comma are written, and go through a String
        that converts them; the value and the rest of the document are
        copied, and must not.  They are already in the character set
        being built in - they came out of the document, which is what
        the result is being made from - so converting them writes them
        a second time.  The arm above copies its two pieces of document
        the same way, with q_append().
      */
      if (str->append('[') ||
          append_simple(str, c_from, c_to - c_from) ||
          str->append(", ", 2) ||
          append_json_value(str, args[n_arg+1], &tmp_val, depth, func_name(),
                            (int) n_arg + 1, splice,
                            !compose_final, compose_final) ||
          str->append(']') ||
          append_simple(str, je.s.c_str,
                        js_end - (const char *) je.s.c_str))
        goto return_null; /* Out of memory. */
    }
    DBUG_ASSERT(watch.unchanged(js));
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  /*
    A document that was already written the loose way, with a value
    written that way put inside it, is written that way already.  See
    Item_func_json_insert::val_str() for why the answer is in js, and
    for what makes this safe to believe - and for how deep it goes,
    which is worked out the same way here.  Wrapping puts a new array
    where the value it holds already sat, and the value going in beside
    it is counted from inside that array, so the new level is counted
    with it.
  */
  if (compose_final && splice.is_valid && splice.is_nice)
  {
    if (!return_json(to, js, MY_MAX(js_depth, splice.deepest)))
      goto return_null; /* Out of memory. */

    return to;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE, &read_back_depth))
    goto js_error;

  /*
    Both marks come from the reading back, and so does the depth, for
    the reason given where Item_func_json_insert::val_str() marks the
    same answer.
  */
  m_marks.set(str, true, true, read_back_depth);
  return str;

js_error:
  report_json_error(js, &je, 0);

return_null:
  null_value= 1;
  return 0;
}


String *Item_func_json_array_insert::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  const char *js_end;
  THD *thd;
  String *const to= str;
  bool compose_final;
  uint depth;
  /* How deep the argument item attested its document to go. */
  uint js_depth;
  /* How deep the reading back below found the answer to go. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;
  Json_source_watch watch;
  /*
    Cleared by anything spliced in that leaves is_valid or is_nice
    false for the answer, the depth a path reaches among it.  See
    Item_func_json_insert::val_str() for what the fourth of them
    starts at and why.
  */
  Json_splice_marks splice(0);

  DBUG_ASSERT(fixed());
  m_marks.clear();

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  /*
    Whether what is composed here is what will be returned, or only
    what a reading back at the end will be made from.  Asked once,
    before anything is composed, for the reason given where
    Item_func_json_insert::val_str() asks it.
  */
  compose_final= document_arg_composes_final(args[0], js);
  /* Taken here for the reason given at the same place there. */
  js_depth= args[0]->last_depth();

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT]= {0};
    json_path_with_flags *c_path= paths + n_path;
    const char *item_pos;
    int n_item, corrected_n_item;

    /*
      Taken before the path is worked out, and afresh every time round.
      See Item_func_json_insert::val_str().
    */
    watch.take(js);

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (!s_p)
        goto return_null;
      if (!s_p->charset())
         goto path_err;

      if (path_setup_nwc(&c_path->p,
                         def_path_charset(s_p->charset(), js->charset()),
                         (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()) ||
           c_path->p.last_step - 1 < c_path->p.steps ||
           c_path->p.last_step->type != JSON_PATH_ARRAY)
      {
        if (c_path->p.s.error == 0)
          c_path->p.s.error= SHOULD_END_WITH_ARRAY;
path_err:
        report_path_error(s_p, &c_path->p, n_arg);

        goto return_null;
      }
      c_path->parsed= c_path->constant;
      c_path->p.last_step--;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    DBUG_ASSERT(watch.unchanged(js));
    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uint32_t *) &thd->killed;

    /*
      Where the document ends, read where the walk over it starts.  See
      Json_source_watch.
    */
    js_end= js->end();

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;

      /* Can't find the array to insert. */
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    if (je.value_type != JSON_VALUE_ARRAY)
    {
      /* Must be an array. */
      continue;
    }

    /*
      How many structures the value will sit inside once it is in
      place: the ones holding the array, and the array itself.  Reading
      the array has already put it on the stack, so the scanner is
      counting it; read off here, before the walk below moves the
      scanner about inside it.
    */
    depth= (uint) je.stack_p;

    item_pos= 0;
    n_item= 0;
    corrected_n_item= c_path->p.last_step[1].n_item;
    if (corrected_n_item < 0)
    {
      int array_size;
      if (json_skip_array_and_count(&je, &array_size))
        goto js_error;
      corrected_n_item+= array_size + 1;
    }

    while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
    {
      DBUG_ASSERT(je.state == JST_VALUE);

      if (n_item == corrected_n_item)
      {
        item_pos= (const char *) je.s.c_str;
        break;
      }
      n_item++;

      if (json_read_value(&je) ||
          (!json_value_scalar(&je) && json_skip_level(&je)))
        goto js_error;
    }

    if (unlikely(je.s.error || *je.killed_ptr))
      goto js_error;

    str->length(0);
    str->set_charset(js->charset());
    if (item_pos)
    {
      my_ptrdiff_t size= item_pos - js->ptr();
      if (append_simple(str, js->ptr(), size))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) size);
        goto return_null; /* Out of memory. */
      }
      if (n_item > 0 && str->append(" ", 1))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), 1);
        goto return_null; /* Out of memory. */
      }
      if (append_json_value(str, args[n_arg+1], &tmp_val, depth, func_name(),
                            (int) n_arg + 1, splice,
                            !compose_final, compose_final))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), tmp_val.length());
        goto return_null; /* Out of memory. */
      }
      if (str->append(",", 1))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), 1);
        goto return_null; /* Out of memory. */
      }
      if (n_item == 0 && str->append(" ", 1))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), 1);
        goto return_null; /* Out of memory. */
      }
      size= js_end - item_pos;
      if (append_simple(str, item_pos, size))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) size);
        goto return_null; /* Out of memory. */
      }
    }
    else
    {
      my_ptrdiff_t size;
      /* Insert position wasn't found - append to the array. */
      DBUG_ASSERT(je.state == JST_ARRAY_END);
      item_pos= (const char *) (je.s.c_str - je.sav_c_len);
      size= item_pos - js->ptr();
      if (append_simple(str, js->ptr(), size))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int) size);
        goto return_null; /* Out of memory. */
      }
      if (n_item > 0 && str->append(", ", 2))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), 2);
        goto return_null; /* Out of memory. */
      }
      if (append_json_value(str, args[n_arg+1], &tmp_val, depth, func_name(),
                            (int) n_arg + 1, splice,
                            !compose_final, compose_final))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), tmp_val.length());
        goto return_null; /* Out of memory. */
      }
      size= js_end - item_pos;
      if (append_simple(str, item_pos, size))
      {
         my_error(ER_OUTOFMEMORY, MYF(0), (int) size);
         goto return_null; /* Out of memory. */
      }
    }

    DBUG_ASSERT(watch.unchanged(js));
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  /*
    A document that was already written the loose way, with a value
    written that way put inside it, is written that way already.  See
    Item_func_json_insert::val_str() for why the answer is in js, and
    for what makes this safe to believe - and for how deep it goes,
    which is worked out the same way here.  Wrapping puts a new array
    where the value it holds already sat, and the value going in beside
    it is counted from inside that array, so the new level is counted
    with it.
  */
  if (compose_final && splice.is_valid && splice.is_nice)
  {
    if (!return_json(to, js, MY_MAX(js_depth, splice.deepest)))
      goto return_null; /* Out of memory. */

    return to;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE, &read_back_depth))
    goto js_error;

  /*
    Both marks come from the reading back, and so does the depth, for
    the reason given where Item_func_json_insert::val_str() marks the
    same answer.
  */
  m_marks.set(str, true, true, read_back_depth);
  return str;

js_error:
  report_json_error(js, &je, 0);
return_null:
  thd->check_killed(); // to get the error message right
  null_value= 1;
  return 0;
}


String *Item_func_json_object::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  uint n_arg;
  /* One deep for the braces alone - see Item_func_json_array::val_str(). */
  Json_splice_marks marks(1);

  m_marks.clear();

  str->length(0);
  str->set_charset(collation.collation);

  if (str->append('{') ||
      (arg_count > 0 &&
       (append_json_keyname(str, args[0], &tmp_val, func_name(), 0) ||
        append_json_value(str, args[1], &tmp_val, 1, func_name(), 1, marks,
                           false, false))))
    goto err_return;

  for (n_arg=2; n_arg < arg_count; n_arg+=2)
  {
    if (str->append(STRING_WITH_LEN(json_loose_comma)) ||
        append_json_keyname(str, args[n_arg], &tmp_val, func_name(),
                            (int) n_arg) ||
        append_json_value(str, args[n_arg+1], &tmp_val, 1, func_name(),
                          (int) n_arg + 1, marks, false, false))
      goto err_return;
  }

  if (str->append('}'))
    goto err_return;

  if (result_limit == 0)
    result_limit= current_thd->variables.max_allowed_packet;

  if (str->length() <= result_limit)
  {
    /* As in the sister constructor above. */
    bool is_json_compatible= is_json_compatible_charset(str->charset());

    m_marks.set(str, is_json_compatible && marks.is_valid,
                is_json_compatible && marks.is_nice, marks.deepest);
    return str;
  }

  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
      func_name(), result_limit);

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


/*
  'wrapped' is raised where the merging puts what it was given inside a
  new array.  That array is a level of its own, and how deep either
  document already went was never measured - they were copied, not read.
  Whoever is composing an answer out of this therefore cannot say how
  deep the answer is, and has to have it read back rather than guess.
*/
static int do_merge(String *str, json_engine_t *je1, json_engine_t *je2,
                    bool *wrapped, uint colon_len)
{
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  return dbug_json_check_min_stack_requirement(););
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  if (json_read_value(je1) || json_read_value(je2))
    return 1;

  if (je1->value_type == JSON_VALUE_OBJECT &&
      je2->value_type == JSON_VALUE_OBJECT)
  {
    json_engine_t sav_je1= *je1;
    json_engine_t sav_je2= *je2;

    int first_key= 1;
    json_string_t key_name;
  
    json_string_set_cs(&key_name, je1->s.cs);

    if (str->append('{'))
      return 3;
    while (json_scan_next(je1) == 0 &&
           je1->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      /* Loop through the Json_1 keys and compare with the Json_2 keys. */
      DBUG_ASSERT(je1->state == JST_KEY);
      key_start= je1->s.c_str;
      do
      {
        key_end= je1->s.c_str;
      } while (json_read_keyname_chr(je1) == 0);

      if (unlikely(je1->s.error))
        return 1;

      if (first_key)
        first_key= 0;
      else
      {
        if (str->append(", ", 2))
          return 3;
        *je2= sav_je2;
      }

      /*
        Written the compact way on purpose, but only half of what
        follows earns it.  A key only this document holds is copied out
        of it below from where the space after the colon stands, so
        writing one here would write it twice.  A key both documents
        hold is composed instead, and a composed value begins at the
        value itself - so that arm writes the space it needs before it
        starts, and it is the only one that has to.

        It writes one only where the answer is going out as it is
        composed, which is what colon_len says.  Where the answer is to
        be read back, the reading writes the whole of it anyway, and
        writing a space here would only move every offset a released
        server reports - including the ones it reports about documents
        it then refuses.
      */
      if (str->append('"') ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append("\":", 2))
        return 3;

      while (json_scan_next(je2) == 0 &&
          je2->state != JST_OBJ_END)
      {
        int ires;
        DBUG_ASSERT(je2->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je2, &key_name))
        {
          if (je2->s.error || json_skip_key(je2))
            return 2;
          continue;
        }

        /* Json_2 has same key as Json_1. Merge them. */
        if (colon_len == 3 && str->append(' '))
          return 3;
        if ((ires= do_merge(str, je1, je2, wrapped, colon_len)))
          return ires;
        goto merged_j1;
      }
      if (unlikely(je2->s.error))
        return 2;

      key_start= je1->s.c_str;
      /* Just append the Json_1 key value. */
      if (json_skip_key(je1))
        return 1;
      if (append_simple(str, key_start, je1->s.c_str - key_start))
        return 3;

merged_j1:
      continue;
    }

    *je2= sav_je2;
    /*
      Now loop through the Json_2 keys.
      Skip if there is same key in Json_1
    */
    while (json_scan_next(je2) == 0 &&
           je2->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      DBUG_ASSERT(je2->state == JST_KEY);
      key_start= je2->s.c_str;
      do
      {
        key_end= je2->s.c_str;
      } while (json_read_keyname_chr(je2) == 0);

      if (unlikely(je2->s.error))
        return 1;

      *je1= sav_je1;
      while (json_scan_next(je1) == 0 &&
             je1->state != JST_OBJ_END)
      {
        DBUG_ASSERT(je1->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je1, &key_name))
        {
          if (unlikely(je1->s.error || json_skip_key(je1)))
            return 2;
          continue;
        }
        if (json_skip_key(je2) || json_skip_level(je1))
          return 1;
        goto continue_j2;
      }

      if (unlikely(je1->s.error))
        return 2;

      if (first_key)
        first_key= 0;
      else if (str->append(", ", 2))
        return 3;

      if (json_skip_key(je2))
        return 1;

      if (str->append('"') ||
          append_simple(str, key_start, je2->s.c_str - key_start))
        return 3;

continue_j2:
      continue;
    }

    if (str->append('}'))
      return 3;
  }
  else
  {
    const uchar *end1, *beg1, *end2, *beg2;
    int n_items1=1, n_items2= 1;

    beg1= je1->value_begin;

    /* Merge as a single array. */
    if (je1->value_type == JSON_VALUE_ARRAY)
    {
      if (json_skip_level_and_count(je1, &n_items1))
        return 1;

      end1= je1->s.c_str - je1->sav_c_len;
    }
    else
    {
      /* A level of its own - see where this is declared. */
      *wrapped= true;
      if (str->append('['))
        return 3;
      if (je1->value_type == JSON_VALUE_OBJECT)
      {
        if (json_skip_level(je1))
          return 1;
        end1= je1->s.c_str;
      }
      else
        end1= je1->value_end;
    }

    /*
      Copied, not written: these bytes came out of a document and are
      already in the character set the answer is being built in.  The
      appends that convert are for punctuation written here, which
      arrives as plain ASCII and has to be turned into the set; putting
      a copied span through one of those writes it a second time, and in
      a set that spends more than one byte on a character what comes out
      is not a document at all.
    */
    if (append_simple(str, beg1, end1 - beg1))
      return 3;

    if (json_value_scalar(je2))
    {
      beg2= je2->value_begin;
      end2= je2->value_end;
    }
    else
    {
      if (je2->value_type == JSON_VALUE_OBJECT)
      {
        beg2= je2->value_begin;
        if (json_skip_level(je2))
          return 2;
      }
      else
      {
        beg2= je2->s.c_str;
        if (json_skip_level_and_count(je2, &n_items2))
          return 2;
      }
      end2= je2->s.c_str;
    }

    /* The comma is written and converts; the span is copied and must not. */
    if ((n_items1 && n_items2 && str->append(", ", 2)) ||
        append_simple(str, beg2, end2 - beg2))
      return 3;

    if (je2->value_type != JSON_VALUE_ARRAY &&
        str->append(']'))
      return 3;
  }

  return 0;
}


/*
  How deep the deepest of the document arguments goes, for the two
  functions whose answer is composed out of nothing else.  Taken one
  argument at a time, beside where that argument is evaluated.

  What an argument answers is about the value it has just passed,
  and by the end of the loop it need not be about that value any more.
  An argument that reads a variable answers out of what is in the
  variable now, so a later argument that assigns to the same variable
  moves the first one's answer out from under the bytes that went into
  the merge - and a document written over by a shallower one then
  answers a depth smaller than the answer really goes, which is the one
  direction a depth must never be wrong in.  The other two marks are
  already taken this way; this is the third of them, and was the only
  one left to the end.

  One argument that will not say makes the answer not say either: what
  is being asked is how deep the deepest part of the result is, and a
  part nothing measured could be anywhere.
*/
static uint deepest_document_argument(uint deepest, Item *arg)
{
  uint arg_depth= arg->last_depth();

  if (deepest == JSON_DEPTH_UNKNOWN || arg_depth == JSON_DEPTH_UNKNOWN)
    return JSON_DEPTH_UNKNOWN;
  return MY_MAX(deepest, arg_depth);
}


/*
  Says what is wrong with a document argument that was read only as far
  as its first value, when anything is.

  These two functions read a document argument no further than that.
  What they compose comes out of that value and nothing else, so neither
  text standing after it nor a break the reading never got as far as
  reaches the answer, and neither has ever stopped an answer being
  given.  What is there is still not a document, and every other JSON
  function says so about the very same characters.

  Saying so here takes nothing away, which is why it is a note.  A
  warning becomes an error under a strict mode, and that would take away
  an answer that has always been given back.  An argument the merging
  broke off inside arrives here already refused, and is reported from
  where it was refused: an engine that has been refused does not move
  again, so carrying the reading on cannot relocate the complaint.

  The reading is carried on from where the merging left off rather than
  started again, so an argument with nothing after its value pays one
  step and no more.  It is carried on over a copy, because a reading
  done to find out whether to say something must not be the reason
  anything else is said: the caller reports its own failures out of its
  own engine, and this must leave that engine where it found it.
*/
static void report_json_trailing_note(const json_engine_t *je,
                                      const String *js,
                                      const char *fname, int n_param)
{
  json_engine_t tail= *je;

  while (json_scan_next(&tail) == 0)
    /* There is nothing left to compose from, only to read. */;

  if (tail.s.error)
    report_json_error_ex(js->ptr(), &tail, fname, n_param,
                         Sql_condition::WARN_LEVEL_NOTE);
}


String *Item_func_json_merge::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  json_engine_t je1, je2;
  String *js1= args[0]->val_json(&tmp_js1), *js2=NULL;
  uint n_arg;
  THD *thd;
  String *const to= str;
  bool compose_final;
  /*
    The formatting of the punctuation written here, settled from the first
    document alone and before anything is composed - see below.
  */
  uint colon_len;
  /* Raised where the merging puts what it was given inside a new array. */
  bool wrapped= false;
  /* How deep the deepest document argument goes, taken as each is read. */
  uint deepest= 0;
  /* How deep the reading back below found the answer to go. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;
  Json_source_watch watch;

  m_marks.clear();

  if (args[0]->null_value)
    goto null_return;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  /*
    Whether what is composed here is what will be returned.  Every
    document merged in has a say, and they are only read one at a time,
    so the answer is settled over the loop.  The FORMATTING cannot wait
    for that - what is written early is written before the later
    arguments have been looked at - so it follows the first document
    alone, which is known here.  A later argument answering is_valid or
    is_nice false then sends the answer through the reading back after
    all, and the
    positions it reports are offsets into text this function composed
    rather than into anything a caller wrote.
  */
  compose_final= document_arg_composes_final(args[0], js1);
  deepest= deepest_document_argument(deepest, args[0]);
  colon_len= compose_final ? 3 : 2;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    str->set_charset(js1->charset());
    str->length(0);

    /*
      The document on the left is not read until below, and working out
      the one on the right can be any expression at all.  See
      Json_source_watch.
    */
    watch.take(js1);
    js2= args[n_arg]->val_json(&tmp_js2);
    DBUG_ASSERT(watch.unchanged(js1));
    if (args[n_arg]->null_value)
      goto null_return;

    /*
      Asked after the argument has been evaluated, what it answers being
      about the value it has just passed.
    */
    if (!args[n_arg]->is_valid_json() || !args[n_arg]->is_nice_json())
      compose_final= false;
    deepest= deepest_document_argument(deepest, args[n_arg]);

    json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                    (const uchar *) js1->ptr() + js1->length());
    je1.killed_ptr= (uint32_t *) &thd->killed;

    json_scan_start(&je2, js2->charset(),(const uchar *) js2->ptr(),
                    (const uchar *) js2->ptr() + js2->length());
    je2.killed_ptr= (uint32_t *) &thd->killed;

    if (do_merge(str, &je1, &je2, &wrapped, colon_len))
      goto error_return;

    /*
      Putting the two inside a new array makes the answer a level deeper
      than either of them, and how deep either of them went was never
      measured.  Have it read back, the same reading a released server
      always did, which is also what puts the complaint in the place
      that server put it.
    */
    if (wrapped)
      compose_final= false;

    /*
      Both were read as far as their first value and no further.  Only
      the first turn of the loop has a document argument on the left:
      after it, the left is what this function itself composed, and
      nothing stands after that.
    */
    if (n_arg == 1)
      report_json_trailing_note(&je1, js1, func_name(), 0);
    report_json_trailing_note(&je2, js2, func_name(), (int) n_arg);

    {
      /* Swap str and js1. */
      if (str == &tmp_js1)
      {
        str= js1;
        js1= &tmp_js1;
      }
      else
      {
        js1= str;
        str= &tmp_js1;
      }
    }
  }

  /*
    Documents that were all written the loose way, joined with
    punctuation written the same way, make a document written that way.
    See Item_func_json_insert::val_str() for why the answer is in js1,
    and for what makes this safe to believe.

    Nothing is spliced in here - every piece of the answer is a document
    argument, and each of them has already had its say in compose_final
    as it came round the loop - so there is no set of splice marks to
    ask, and the one condition is the whole of it.

    Which is also what says how deep the answer goes, ONE LEVEL OUT.
    Merging two arrays lays their members side by side and merging two
    objects puts their keys together, and neither of those moves
    anything further inside than it was.  But merging an array with
    anything else makes that other thing a MEMBER of the array, and a
    member sits one level inside - so the deepest of the arguments is
    not enough on its own, which the reading back said the first time
    this was written without the level.

    Which of the two happened is not worth telling apart here: a level
    more than the answer needs turns down a splice that could have been
    taken, and costs a reading nobody is owed an answer about.
  */
  if (compose_final)
  {
    if (!return_json(to, js1,
                        deepest == JSON_DEPTH_UNKNOWN ? deepest : deepest + 1))
      goto error_return;

    return to;
  }

  json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                  (const uchar *) js1->ptr() + js1->length());
  je1.killed_ptr= (uint32_t *) &thd->killed;

  if (json_nice(&je1, str, Item_func_json_format::LOOSE, &read_back_depth))
    goto error_return;

  null_value= 0;
  /*
    Both marks come from the reading back, and so does the depth, for
    the reason given where Item_func_json_insert::val_str() marks the
    same answer.
  */
  m_marks.set(str, true, true, read_back_depth);
  return str;

error_return:
  if (je1.s.error)
    report_json_error(js1, &je1, 0);
  if (je2.s.error)
    report_json_error(js2, &je2, n_arg);
null_return:
  null_value= 1;
  return NULL;
}


static int copy_value_patch(String *str, json_engine_t *je)
{
  int first_key= 1;

  if (je->value_type != JSON_VALUE_OBJECT)
  {
    const uchar *beg, *end;

    beg= je->value_begin;

    if (!json_value_scalar(je))
    {
      if (json_skip_level(je))
        return 1;
      end= je->s.c_str;
    }
    else
      end= je->value_end;

    if (append_simple(str, beg, end-beg))
      return 1;

    return 0;
  }
  /* JSON_VALUE_OBJECT */

  if (str->append('{'))
    return 1;
  while (json_scan_next(je) == 0 && je->state != JST_OBJ_END)
  {
    const uchar *key_start;
    /* Loop through the Json_1 keys and compare with the Json_2 keys. */
    DBUG_ASSERT(je->state == JST_KEY);
    key_start= je->s.c_str;

    if (json_read_value(je))
      return 1;

    if (je->value_type == JSON_VALUE_NULL)
      continue;

    if (!first_key)
    {
      if (str->append(", ", 2))
        return 3;
    }
    else
      first_key= 0;

    if (str->append('"') ||
        append_simple(str, key_start, je->value_begin - key_start) ||
        copy_value_patch(str, je))
      return 1;
  }
  if (str->append('}'))
    return 1;

  return 0;
}


static int do_merge_patch(String *str, json_engine_t *je1, json_engine_t *je2,
                          bool *empty_result, uint colon_len)
{
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  return dbug_json_check_min_stack_requirement(););
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  if (json_read_value(je1) || json_read_value(je2))
    return 1;

  if (je1->value_type == JSON_VALUE_OBJECT &&
      je2->value_type == JSON_VALUE_OBJECT)
  {
    json_engine_t sav_je1= *je1;
    json_engine_t sav_je2= *je2;

    int first_key= 1;
    json_string_t key_name;
    size_t sav_len;
    bool mrg_empty;

    *empty_result= FALSE;
    json_string_set_cs(&key_name, je1->s.cs);

    if (str->append('{'))
      return 3;

    while (json_scan_next(je1) == 0 &&
           je1->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      /* Loop through the Json_1 keys and compare with the Json_2 keys. */
      DBUG_ASSERT(je1->state == JST_KEY);
      key_start= je1->s.c_str;
      do
      {
        key_end= je1->s.c_str;
      } while (json_read_keyname_chr(je1) == 0);

      if (je1->s.error)
        return 1;

      sav_len= str->length();

      if (!first_key)
      {
        if (str->append(", ", 2))
          return 3;
        *je2= sav_je2;
      }

      if (str->append('"') ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append(json_loose_colon, colon_len))
        return 3;

      while (json_scan_next(je2) == 0 &&
          je2->state != JST_OBJ_END)
      {
        int ires;
        DBUG_ASSERT(je2->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je2, &key_name))
        {
          if (je2->s.error || json_skip_key(je2))
            return 2;
          continue;
        }

        /* Json_2 has same key as Json_1. Merge them. */
        if ((ires= do_merge_patch(str, je1, je2, &mrg_empty, colon_len)))
          return ires;

        if (mrg_empty)
          str->length(sav_len);
        else
          first_key= 0;

        goto merged_j1;
      }

      if (je2->s.error)
        return 2;

      key_start= je1->s.c_str;
      /* Just append the Json_1 key value. */
      if (json_skip_key(je1))
        return 1;
      /*
        This span begins where the colon left off, and in a document
        written the loose way that is the space after it - the space
        just written above.  The other way out of this loop has the
        value written afresh, with no space of its own, which is why
        the colon writes one at all.
      */
      if (colon_len == 3)
        key_start= json_skip_space(je1->s.cs, key_start, je1->s.c_str);
      if (append_simple(str, key_start, je1->s.c_str - key_start))
        return 3;
      first_key= 0;

merged_j1:
      continue;
    }

    *je2= sav_je2;
    /*
      Now loop through the Json_2 keys.
      Skip if there is same key in Json_1
    */
    while (json_scan_next(je2) == 0 &&
           je2->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      DBUG_ASSERT(je2->state == JST_KEY);
      key_start= je2->s.c_str;
      do
      {
        key_end= je2->s.c_str;
      } while (json_read_keyname_chr(je2) == 0);

      if (je2->s.error)
        return 1;

      *je1= sav_je1;
      while (json_scan_next(je1) == 0 &&
             je1->state != JST_OBJ_END)
      {
        DBUG_ASSERT(je1->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je1, &key_name))
        {
          if (je1->s.error || json_skip_key(je1))
            return 2;
          continue;
        }
        if (json_skip_key(je2) ||
            json_skip_level(je1))
          return 1;
        goto continue_j2;
      }

      if (je1->s.error)
        return 2;


      sav_len= str->length();

      if (!first_key && str->append(", ", 2))
        return 3;

      if (str->append('"') ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append(json_loose_colon, colon_len))
        return 3;

      if (json_read_value(je2))
        return 1;

      if (je2->value_type == JSON_VALUE_NULL)
        str->length(sav_len);
      else
      {
        if (copy_value_patch(str, je2))
          return 1;
        first_key= 0;
      }

continue_j2:
      continue;
    }

    if (str->append('}'))
      return 3;
  }
  else
  {
    if (!json_value_scalar(je1) && json_skip_level(je1))
      return 1;

    *empty_result= je2->value_type == JSON_VALUE_NULL;
    if (!(*empty_result) && copy_value_patch(str, je2))
      return 1;
  }

  return 0;
}


String *Item_func_json_merge_patch::val_str(String *str)
{
  DBUG_ASSERT(fixed());
  json_engine_t je1, je2;
  String *js1= args[0]->val_json(&tmp_js1), *js2=NULL;
  uint n_arg;
  bool empty_result, merge_to_null;
  THD *thd= current_thd;
  String *const to= str;
  bool compose_final;
  uint colon_len;
  Json_source_watch watch;
  /* How deep the deepest document argument goes, taken as each is read. */
  uint deepest= 0;
  /* How deep the reading back below found the answer to go. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;
  /*
    Cleared by anything that goes into the answer that the answer cannot
    be said to hold: a document taken over without having been read on
    the way in, and a document whose item attests is_nice false.  Nothing is
    spliced into a patched document - every piece of the answer is a
    document argument - so the fourth of them is never moved off nothing
    here.
  */
  Json_splice_marks splice(0);

  m_marks.clear();

  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  /* To report errors properly if some JSON is invalid. */
  je1.s.error= je2.s.error= 0;
  merge_to_null= args[0]->null_value;

  /*
    The formatting follows the first document alone, for the reason given
    where Item_func_json_merge::val_str() picks its own.  A first
    argument that is SQL NULL contributes nothing to the answer and is
    not asked, and leaves nothing to read the character set off either.
  */
  compose_final= !merge_to_null && document_arg_composes_final(args[0], js1);
  colon_len= compose_final ? 3 : 2;
  deepest= deepest_document_argument(deepest, args[0]);
  if (!compose_final)
    splice.is_nice= false;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    /*
      The document on the left is not read until below, and working out
      the one on the right can be any expression at all.  See
      Json_source_watch.  A left-hand side that is SQL NULL has no bytes
      to keep an eye on.
    */
    if (!merge_to_null)
      watch.take(js1);
    js2= args[n_arg]->val_json(&tmp_js2);
    DBUG_ASSERT(merge_to_null || watch.unchanged(js1));
    /*
      Taken before the arms below part, an argument that is dropped
      being asked here just as one that goes in is.  A depth folded in
      from an argument the answer does not hold is a depth larger than
      the answer needs, which is the side a depth is allowed to be
      wrong on; asking only the ones that stay would make the reading
      turn on which arm was taken, and the arm is settled by later
      arguments.
    */
    deepest= deepest_document_argument(deepest, args[n_arg]);
    if (args[n_arg]->null_value)
    {
      merge_to_null= true;
      goto cont_point;
    }

    /*
      Asked after the argument has been evaluated, what it answers being
      about the value it has just passed.  A document merged in is
      read as it is copied, so nothing here is about whether it is a
      document - only about how it is written.
    */
    if (!args[n_arg]->is_nice_json())
      splice.is_nice= false;

    json_scan_start(&je2, js2->charset(),(const uchar *) js2->ptr(),
                    (const uchar *) js2->ptr() + js2->length());
    je2.killed_ptr= (uint32_t *) &thd->killed;

    if (merge_to_null)
    {
      if (json_read_value(&je2))
        goto error_return;

      /*
        Said before it is settled whether the argument goes into the
        answer at all.  An object merged onto SQL NULL contributes
        nothing and is dropped just below, and being dropped makes what
        stands after it no more a document than it was; the very same
        characters are spoken for in every other argument position.

        Only the first value was read, so getting to the end of it comes
        first here, where the merging below does it for itself.  A value
        that cannot be got to the end of is one the merging will report
        on in its own words; there is nothing to add to that.

        Not asked at all of a value already attested to.
        Being a document is a statement about the whole of it - a value
        with anything standing after it is not one - so the walk could
        only ever come back saying what is already known, and it is the
        walk this whole exercise is about not making.  It is the one the
        released server did not make either: it took the first value and
        copied the rest in unread.
      */
      if (!args[n_arg]->is_valid_json())
      {
        json_engine_t adopted= je2;

        if (json_value_scalar(&adopted) || !json_skip_level(&adopted))
          report_json_trailing_note(&adopted, js2, func_name(), (int) n_arg);
      }

      if (je2.value_type == JSON_VALUE_OBJECT)
        goto cont_point;

      merge_to_null= false;
      /*
        Taken over by copying it rather than by pointing at it.  The next
        argument is read into the very buffer this one came in, and a
        buffer that has to grow to hold it is not the buffer it was: what
        was pointed at is gone by the time it comes to be read.
      */
      if (str->copy(js2->ptr(), js2->length(), js2->charset()))
        goto error_return;

      /*
        The whole of it goes in and only its first value was read, so
        whatever stands after that value goes in unread.  Text standing
        there is no reason to refuse the document: what comes of it is
        a matter for whatever the answer turns out to be, and a later
        argument merging over this one leaves it out of the answer
        altogether, which is an answer that has always been given back.

        A document whose item attests is_valid has nothing against it
        to begin with, and goes in the way it was written.  Anything
        else has to be read back, so say here that it has to be.
      */
      if (!args[n_arg]->is_valid_json())
        splice.is_valid= splice.is_nice= false;
      else if (!args[n_arg]->is_nice_json())
        splice.is_nice= false;

      goto cont_point;
    }

    str->set_charset(js1->charset());
    str->length(0);


    json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                    (const uchar *) js1->ptr() + js1->length());
    je1.killed_ptr= (uint32_t *) &thd->killed;

    if (do_merge_patch(str, &je1, &je2, &empty_result, colon_len))
      goto error_return;

    /*
      Both were read as far as their first value and no further.  Only
      the first turn of the loop has a document argument on the left:
      after it, the left is what this function itself composed, or a
      document taken over, which was spoken for where it was taken.
    */
    if (n_arg == 1)
      report_json_trailing_note(&je1, js1, func_name(), 0);
    report_json_trailing_note(&je2, js2, func_name(), (int) n_arg);

    if (empty_result)
      str->append(STRING_WITH_LEN("null"));

cont_point:
    {
      /* Swap str and js1. */
      if (str == &tmp_js1)
      {
        str= js1;
        js1= &tmp_js1;
      }
      else
      {
        js1= str;
        str= &tmp_js1;
      }
    }
  }

  if (merge_to_null)
    goto null_return;

  /*
    Documents that were all written the loose way, patched with
    punctuation written the same way, make a document written that way.
    See Item_func_json_insert::val_str() for why the answer is in js1,
    and for what makes this safe to believe.

    Asked of the splice marks alone, where the others ask compose_final
    too, and it comes to the same thing: what the others keep in that
    variable this one keeps in colon_len, which is the wider formatting
    only where the first document answers is_valid and is_nice and can
    be written -
    and anything else clears is_nice on the spot.  A document taken over
    whole clears both marks where it was taken.

    How deep it goes is read off the arguments, and WITHOUT the extra
    level Item_func_json_merge::val_str() takes.  Patching is not
    merging: the two documents are walked in step, a key of one going
    among the keys of the other at the level both were at, and a value
    replacing a value where that value stood.  Nothing is ever made a
    member of anything, which is the case that costs the level there,
    and a key the patch drops only takes levels away.
  */
  if (splice.is_valid && splice.is_nice)
  {
    if (!return_json(to, js1, deepest))
      goto error_return;

    return to;
  }

  json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                  (const uchar *) js1->ptr() + js1->length());
  je1.killed_ptr= (uint32_t *) &thd->killed;
  if (json_nice(&je1, str, Item_func_json_format::LOOSE, &read_back_depth))
    goto error_return;

  null_value= 0;
  /*
    Both marks come from the reading back, and so does the depth, for
    the reason given where Item_func_json_insert::val_str() marks the
    same answer.
  */
  m_marks.set(str, true, true, read_back_depth);
  return str;

error_return:
  if (je1.s.error)
    report_json_error(js1, &je1, 0);
  if (je2.s.error)
    report_json_error(js2, &je2, n_arg);
null_return:
  null_value= 1;
  return NULL;
}


bool Item_func_json_length::fix_length_and_dec(THD *thd)
{
  if (arg_count > 1)
    path.set_constant_flag(args[1]->const_item());
  set_maybe_null();
  max_length= 10;
  return FALSE;
}


longlong Item_func_json_length::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  Json_source_watch watch;
  uint length= 0;
  int array_counters[JSON_DEPTH_LIMIT]= {0};
  int err;
  THD *thd;

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  watch.take(js);
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (arg_count > 1)
  {
    /* Path specified - let's apply it. */
    if (!path.parsed)
    {
      String *s_p= args[1]->val_str(&tmp_path);
      if (!s_p)
        goto null_return;
      if (path_setup_nwc(&path.p,
                         def_path_charset(s_p->charset(), js->charset()),
                         (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &path.p, 1);
        goto null_return;
      }
      path.parsed= path.constant;
    }
    if (args[1]->null_value)
      goto null_return;

    path.cur_step= path.p.steps;
    DBUG_ASSERT(watch.unchanged(js));
    if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
    {
      if (je.s.error)
        goto err_return;
      goto null_return;
    }
  }
  

  if (json_read_value(&je))
    goto err_return;

  if (json_value_scalar(&je))
    return 1;

  while (!(err= json_scan_next(&je)) &&
         je.state != JST_OBJ_END && je.state != JST_ARRAY_END)
  {
    switch (je.state)
    {
    case JST_VALUE:
    case JST_KEY:
      length++;
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      if (json_skip_level(&je))
        goto err_return;
      break;
    default:
      break;
    };
  }

  /*
    The rest of the document is parsed only to check that it is one.
    Not done at all where the item has already attested that the value
    is_valid: the parse could only report what is already known.
  */
  if (!err && !args[0]->is_valid_json())
  {
    while (json_scan_next(&je) == 0) {}
  }

  if (likely(!je.s.error))
    return length;

err_return:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


longlong Item_func_json_depth::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint depth= 0, c_depth= 0;
  bool inc_depth= TRUE;
  THD *thd;

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  do
  {
    switch (je.state)
    {
    case JST_VALUE:
    case JST_KEY:
      if (inc_depth)
      {
        c_depth++;
        inc_depth= FALSE;
        if (c_depth > depth)
          depth= c_depth;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      inc_depth= TRUE;
      break;
    case JST_OBJ_END:
    case JST_ARRAY_END:
      if (!inc_depth)
        c_depth--;
      inc_depth= FALSE;
      break;
    default:
      break;
    }
  } while (json_scan_next(&je) == 0);

  if (likely(!je.s.error))
    return depth;

  report_json_error(js, &je, 0);
  null_value= 1;
  return 0;
}


bool Item_func_json_type::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb3_general_ci);
  max_length= 12 * collation.collation->mbmaxlen;
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_type::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  const char *type;
  THD *thd;

  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  je.killed_ptr= (uint32_t *) &thd->killed;

  /*
    Returns the type of the document's first value, or NULL if the
    document does not parse all the way through.  Parsing it through
    comes first, then: a type worked out for text that turns out not to
    be a document is a type that is never returned.

    Not done at all where the item has already attested that the value
    is_valid.  The parse could only report what is already known, and it
    is the parse this whole change is about not making.

    What follows it is not that parse repeated.  json_read_value() stops
    at the bracket that opens a container - see read_obj() - so for any
    document but a bare scalar it is one step and not a second walk.
  */
  if (!args[0]->is_valid_json() &&
      !json_valid_engine(&je, js->ptr(), js->length(), js->charset()))
    goto error;

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (json_read_value(&je))
    goto error;

  switch (je.value_type)
  {
  case JSON_VALUE_OBJECT:
    type= "OBJECT";
    break;
  case JSON_VALUE_ARRAY:
    type= "ARRAY";
    break;
  case JSON_VALUE_STRING:
    type= "STRING";
    break;
  case JSON_VALUE_NUMBER:
    type= (je.num_flags & JSON_NUM_FRAC_PART) ?  "DOUBLE" : "INTEGER";
    break;
  case JSON_VALUE_TRUE:
  case JSON_VALUE_FALSE:
    type= "BOOLEAN";
    break;
  default:
    type= "NULL";
    break;
  }

  str->set(type, strlen(type), &my_charset_utf8mb3_general_ci);
  return str;

error:
  report_json_error(js, &je, 0);
  null_value= 1;
  return 0;
}


bool Item_func_json_insert::fix_length_and_dec(THD *thd)
{
  uint n_arg;
  ulonglong char_length;

  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  collation.set(args[0]->collation);
  /*
    The document is written out again around what is put into it, a
    space arriving after every separator that is copied, so the room for
    it has to cover the writing and not only the reading - the same
    allowance JSON_REMOVE asks for, adding the same spacing.
  */
  char_length= static_cast<ulonglong>(args[0]->max_char_length()) * 2;

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg/2].set_constant_flag(args[n_arg]->const_item());
    /*
      In the resulting JSON we can insert the property
      name from the path, and the value itself.
    */
    char_length+= static_cast<ulonglong>(args[n_arg]->max_char_length()) + 6;
    char_length+= json_value_reserve(args[n_arg+1]) + 4;
  }

  fix_char_length_ulonglong(char_length);
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_insert::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  json_string_t key_name;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> tmp_key;
  const char *js_end;
  THD *thd;
  String *const to= str;
  bool compose_final;
  uint colon_len;
  /* How deep the argument item attested its document to go. */
  uint js_depth;
  /* How deep the reading back below found the answer to go. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;
  Json_source_watch watch;
  /*
    Cleared by anything spliced in that leaves is_valid or is_nice false
    for the answer, the depth a path reaches among it.  The deepest a
    spliced value ends up starts at nothing: this writes no structure of
    its own, so where the answer is deepest is either somewhere a value
    went in or somewhere the document already was.
  */
  Json_splice_marks splice(0);

  DBUG_ASSERT(fixed());
  m_marks.clear();

  if ((null_value= args[0]->null_value))
    return 0;

  /*
    Whether what is composed here is what will be returned, or only
    what a reading back at the end will be made from.  Asked once,
    before anything is composed, because the composing has to know it:
    an answer that is going to be written out again must be composed
    exactly as it always was, positions reported against it being
    offsets into it.  A document whose item attests neither is_valid nor
    is_nice is such an answer, and it is the one a caller wrote out by
    hand.

    A character set that cannot encode the punctuation written here is
    the other one - see is_json_compatible_charset().  Nothing composed in
    such a set is a document, however sound the pieces were, and until
    the reading back went away it was the only thing that ever noticed.
    So it is kept exactly where it was.
  */
  compose_final= document_arg_composes_final(args[0], js);
  colon_len= compose_final ? 3 : 2;
  /*
    And how deep the document goes, taken here rather than where it is
    used, which is after every other argument has been worked out.

    THIS IS THE CANONICAL SITE for this too.  The three answers are
    about one value and have to be taken over one moment: an argument
    can be any expression a caller cares to write, a stored function
    among them, and one of those can assign the very thing args[0] reads
    - a stored program's variable, say - between the two readings.  The
    document in hand is still the one this composed from, so a depth
    read afterwards can be the depth of a value this answer is not
    about, and too small is the one direction a depth must never be
    wrong in.
  */
  js_depth= args[0]->last_depth();

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  str->set_charset(collation.collation);
  tmp_js.set_charset(collation.collation);

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT]= {0};
    json_path_with_flags *c_path= paths + n_path;
    const char *v_to;
    json_path_step_t *lp;
    int corrected_n_item;

    /*
      Taken before the path is worked out.  A path is any expression a
      caller cares to write, and it is the first thing here that can
      reach the document; once the walk below has started, its pointers
      into the document are live and there is nothing left to catch.

      Taken afresh every time round, too: the end of this loop swaps
      what was composed here into js, so what the document IS changes
      between one path and the next, on purpose.
    */
    watch.take(js);

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p)
      {
        if (path_setup_nwc(&c_path->p,
                           def_path_charset(s_p->charset(), js->charset()),
                           (const uchar *) s_p->ptr(),
                           (const uchar *) s_p->ptr() + s_p->length()))
        {
          report_path_error(s_p, &c_path->p, n_arg);
          goto return_null;
        }

        /* We search to the last step. */
        c_path->p.last_step--;
      }
      else
        goto return_null;
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    DBUG_ASSERT(watch.unchanged(js));
    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uint32_t *) &thd->killed;

    /*
      Where the document ends, read where the walk over it starts.  See
      Json_source_watch.
    */
    js_end= js->end();

    if (c_path->p.last_step < c_path->p.steps)
      goto v_found;

    c_path->cur_step= c_path->p.steps;

    if (c_path->p.last_step >= c_path->p.steps &&
        json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    lp= c_path->p.last_step+1;
    if (lp->type & JSON_PATH_ARRAY)
    {
      int n_item= 0;

      if (je.value_type != JSON_VALUE_ARRAY)
      {
        const uchar *v_from= je.value_begin;
        int do_array_autowrap;

        if (mode_insert)
        {
          if (mode_replace)
            do_array_autowrap= lp->n_item > 0;
          else
          {
            if (lp->n_item == 0)
              continue;
            do_array_autowrap= 1;
          }
        }
        else
        {
          if (lp->n_item)
            continue;
          do_array_autowrap= 0;
        }


        str->length(0);
        /* Wrap the value as an array. */
        if (append_simple(str, js->ptr(), (const char *) v_from - js->ptr()) ||
            (do_array_autowrap && str->append('[')))
          goto js_error; /* Out of memory. */

        if (je.value_type == JSON_VALUE_OBJECT)
        {
          /*
            Wrapping puts what was already there inside a new array, so
            the KEPT value goes down a level as well as the one being
            put in - and how far down the kept one already reached was
            never measured, it having been copied rather than read.  Say
            so and let the answer be read back.  A scalar has nothing
            inside it and so needs no saying.
          */
          if (do_array_autowrap)
            splice.is_valid= splice.is_nice= false;
          if (json_skip_level(&je))
            goto js_error;
        }

        if ((do_array_autowrap &&
             (append_simple(str, v_from, je.s.c_str - v_from) ||
              str->append(", ", 2))) ||
            append_json_value(str, args[n_arg+1], &tmp_val,
                              (uint) je.stack_p + (do_array_autowrap ? 1 : 0),
                              func_name(), (int) n_arg + 1, splice,
                              !compose_final, compose_final) ||
            (do_array_autowrap && str->append(']')) ||
            append_simple(str, je.s.c_str, js_end - (const char *) je.s.c_str))
          goto js_error; /* Out of memory. */

        goto continue_point;
      }
      corrected_n_item= lp->n_item;
      if (corrected_n_item < 0)
      {
        int array_size;
        if (json_skip_array_and_count(&je, &array_size))
          goto js_error;
        corrected_n_item+= array_size;
      }

      while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
      {
        switch (je.state)
        {
        case JST_VALUE:
          if (n_item == corrected_n_item)
            goto v_found;
          n_item++;
          if (json_skip_array_item(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      if (!mode_insert)
        continue;

      v_to= (const char *) (je.s.c_str - je.sav_c_len);
      str->length(0);
      /*
        One deeper than the scanner says.  Getting here means the walk
        ran off the end of the array, and an array is taken off the
        stack BEFORE the state that says it ended is set - so by now the
        array the value is going into is no longer counted, and the
        value goes inside it.  The arm that finds the place it was
        looking for reads the level after skipping back over the value,
        with the array still counted, and so needs no such correction.
      */
      if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
          (n_item > 0 && str->append(", ", 2)) ||
          append_json_value(str, args[n_arg+1], &tmp_val,
                            (uint) je.stack_p + 1,
                            func_name(), (int) n_arg + 1, splice,
                            !compose_final, compose_final) ||
          append_simple(str, v_to, js_end - v_to))
        goto js_error; /* Out of memory. */
    }
    else /*JSON_PATH_KEY*/
    {
      uint n_key= 0;
      bool key_inert= false;

      if (je.value_type != JSON_VALUE_OBJECT)
        continue;

      /*
        The key of the step is written in the character set of the path,
        which is not necessarily the one the document is written in.
        Comparing it against a key of the document compares characters,
        so it has to be read in its own character set.
      */
      json_string_set_cs(&key_name, c_path->p.s.cs);

      while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
      {
        switch (je.state)
        {
        case JST_KEY:
          json_string_set_str(&key_name, lp->key, lp->key_end);
          if (json_key_matches(&je, &key_name))
            goto v_found;
          n_key++;
          if (json_skip_key(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      if (!mode_insert)
        continue;

      v_to= (const char *) (je.s.c_str - je.sav_c_len);
      str->length(0);
      if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
          (n_key > 0 && str->append(", ", 2)) ||
          str->append('"') ||
          DBUG_IF("json_insert_key_out_of_memory"))
        goto js_error; /* Out of memory. */

      if (append_json_path_key(str, lp, c_path->p.s.cs, &tmp_key,
                               func_name(), (int) n_arg + 1, &key_inert) ||
          DBUG_IF("json_insert_path_key_out_of_memory"))
        goto js_error; /* Out of memory. */

      /*
        A key that reaches past the quotes around it can make the answer
        anything at all - unreadable, or a different document that reads
        perfectly well.  Which of the two it is can only be told by
        reading the whole of it, so say here that it has to be.
      */
      if (!key_inert)
        splice.is_valid= splice.is_nice= false;

      /*
        Written the loose way when that is how the answer is going out,
        like the comma above it and the value after it.  A key put in
        here is the only punctuation the editing adds of its own, so it
        is the only place the two formats can differ.
      */
      /*
        One deeper than the scanner says, for the reason given at the
        end of the array above: the object is taken off the stack before
        the state that says it ended, and the new key goes inside it.
      */
      if (str->append(json_loose_colon, colon_len) ||
          append_json_value(str, args[n_arg+1], &tmp_val,
                            (uint) je.stack_p + 1,
                            func_name(), (int) n_arg + 1, splice,
                            !compose_final, compose_final) ||
          append_simple(str, v_to, js_end - v_to))
        goto js_error; /* Out of memory. */
    }

    goto continue_point;

v_found:

    if (!mode_replace)
      continue;

    if (json_read_value(&je))
      goto js_error;

    v_to= (const char *) je.value_begin;
    str->length(0);
    if (!json_value_scalar(&je))
    {
      if (json_skip_level(&je))
        goto js_error;
    }

    if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
        append_json_value(str, args[n_arg+1], &tmp_val, (uint) je.stack_p,
                          func_name(), (int) n_arg + 1, splice,
                          !compose_final, compose_final) ||
        append_simple(str, je.s.c_str, js_end - (const char *) je.s.c_str))
      goto js_error; /* Out of memory. */
continue_point:
    DBUG_ASSERT(watch.unchanged(js));
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  /*
    A document that was already written the loose way, edited with
    pieces that were themselves written that way, is written that way
    already: reading it back would write it exactly as it stands.  So it
    is passed as it stands, and the reading below is the reading
    this whole exercise is about doing away with.

    THIS IS THE CANONICAL SITE.  The other five that edit a document
    point here rather than repeat it.

    What makes the answer A DOCUMENT.  What was kept of the document was
    read as one on the way in - that is compose_final's
    args[0]->is_valid_json() - and is copied out of it whole, at offsets
    the walk above computed rather than guessed.  What was put in was
    either read here as it went in, or attested to by whoever produced
    it, and either way splice.is_valid says so.  What was written here
    is punctuation and a key, and a key that could reach past its own
    quotes clears the same mark rather than being refused.

    What makes it WRITTEN THE LOOSE WAY.  The kept parts were, by
    args[0]->is_nice_json(); the values put in were, by splice.is_nice;
    and the punctuation written here was, colon_len having been picked
    from compose_final before anything was composed.

    And none of that means anything where the punctuation cannot be
    written at all, which is compose_final's third part - see
    is_json_compatible_charset().  A document in such a character set is a
    scalar, because a container would take brackets it does not have.

    Nothing here takes any of it on faith.  m_marks.set() reads the
    answer back in a debug build and stops the server if these
    conditions ever hold over something that is not a document written
    that way - which is the same reading, kept exactly where it is worth
    its cost and nowhere else.

    The answer is in js rather than str: each pass writes into str and
    then swaps the two, so after an odd number of passes str is the
    scratch.  It is returned in the buffer the caller supplied, which
    every path through this function has always done.

    HOW DEEP IT GOES, when nothing read it back to find out.  Editing a
    document puts a value somewhere inside it and copies the rest of it
    across untouched, so the answer is deepest either where a value went
    in - which splice.deepest holds, counted from the outside - or
    somewhere the document already went, which is what the document was
    able to say about itself.  Neither of those can be short: the first
    is a bound the splice was let through on, and the second is
    whatever args[0] attested, which is nothing at all unless something
    measured it.  An item that attests to nothing leaves the answer
    attesting to nothing, which is where this stood before there was
    anything to ask.
  */
  if (compose_final && splice.is_valid && splice.is_nice)
  {
    if (!return_json(to, js, MY_MAX(js_depth, splice.deepest)))
      goto js_error; /* Out of memory. */

    return to;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE, &read_back_depth))
    goto js_error;

  /*
    The reading back just above is what sets both marks.  It also says
    whether the depth reckoned at each splice was reckoned right: a
    reading that got to the end is an answer inside the limit, so
    nothing should have been marked as taking it past one.  The other
    way round is allowed - a value can be marked for reasons that leave
    a readable answer behind, an empty one among them.
  */
  DBUG_ASSERT(!splice.is_deep);

  /*
    Both marks come from the reading back and from it alone: json_nice()
    got to the end of what was composed, which is what makes it a
    document, and what is marked is what json_nice() WROTE rather than
    what it read, which is what makes it written the loose way.  This is
    the reading that stays wherever the document handed in was nobody's
    word - it is not a leftover, and taking it out would take 10.11's
    answers with it.  The depth comes from there too, and is the one
    measurement rather than a bound.
  */
  m_marks.set(str, true, true, read_back_depth);
  return str;

js_error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}


bool Item_func_json_remove::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  /*
    What is left of the document is written out again, with a space after
    every separator that is copied, so what comes back can be longer than
    what went in even though something was taken out of it.  A separator
    has a value on either side of it and the shortest value is one
    character, so at worst every second character gains one - which is
    the allowance JSON_LOOSE asks for, adding the same spacing.
  */
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * 2);

  mark_constant_paths(paths, args+1, arg_count-1);
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_remove::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  json_string_t key_name;
  THD *thd;
  String *const to= str;
  bool compose_final;
  uint comma_len;
  Json_source_watch watch;
  /* How deep the argument item attested its document to go. */
  uint js_depth;
  /* How deep the reading back below found the answer to go. */
  uint read_back_depth= JSON_DEPTH_UNKNOWN;

  DBUG_ASSERT(fixed());
  m_marks.clear();

  if (args[0]->null_value)
    goto null_return;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  str->set_charset(js->charset());

  /*
    Whether what is composed here is what will be returned, or only
    what a reading back at the end will be made from.  Asked once,
    before anything is composed, for the reason given where
    Item_func_json_insert::val_str() asks it.
  */
  compose_final= document_arg_composes_final(args[0], js);
  comma_len= compose_final ? 2 : 1;
  /* Taken here for the reason given at the same place there. */
  js_depth= args[0]->last_depth();

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg++, n_path++)
  {
    int array_counters[JSON_DEPTH_LIMIT]= {0};
    json_path_with_flags *c_path= paths + n_path;
    const char *rem_start= 0, *rem_end;
    json_path_step_t *lp;
    int n_item= 0;

    /*
      Taken afresh every time round: the end of this loop swaps what
      was composed here into js, so what the document IS changes
      between one path and the next, on purpose.
    */
    watch.take(js);

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p)
      {
        if (path_setup_nwc(&c_path->p,
                           def_path_charset(s_p->charset(), js->charset()),
                           (const uchar *) s_p->ptr(),
                           (const uchar *) s_p->ptr() + s_p->length()))
        {
          report_path_error(s_p, &c_path->p, n_arg);
          goto null_return;
        }

        /* We search to the last step. */
        c_path->p.last_step--;
        if (c_path->p.last_step < c_path->p.steps)
        {
          c_path->p.s.error= TRIVIAL_PATH_NOT_ALLOWED;
          report_path_error(s_p, &c_path->p, n_arg);
          goto null_return;
        }
      }
      else
        goto null_return;
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto null_return;

    DBUG_ASSERT(watch.unchanged(js));
    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());
    je.killed_ptr= (uint32_t *) &thd->killed;

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    lp= c_path->p.last_step+1;

    if (lp->type & JSON_PATH_ARRAY)
    {
      int corrected_n_item;
      if (je.value_type != JSON_VALUE_ARRAY)
        continue;

      corrected_n_item= lp->n_item;
      if (corrected_n_item < 0)
      {
        int array_size;
        if (json_skip_array_and_count(&je, &array_size))
          goto js_error;
        corrected_n_item+= array_size;
      }

      while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
      {
        switch (je.state)
        {
        case JST_VALUE:
          if (n_item == corrected_n_item)
          {
            rem_start= (const char *) (je.s.c_str -
                         (n_item ? je.sav_c_len : 0));
            goto v_found;
          }
          n_item++;
          if (json_skip_array_item(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      continue;
    }
    else /*JSON_PATH_KEY*/
    {
      if (je.value_type != JSON_VALUE_OBJECT)
        continue;

      /*
        Set here and not once before the loop: each path is written in
        its own character set, and the key has to be read in the one the
        path it came from was written in.
      */
      json_string_set_cs(&key_name, c_path->p.s.cs);

      while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
      {
        switch (je.state)
        {
        case JST_KEY:
          if (n_item == 0)
            rem_start= (const char *) (je.s.c_str - je.sav_c_len);
          json_string_set_str(&key_name, lp->key, lp->key_end);
          if (json_key_matches(&je, &key_name))
            goto v_found;

          if (json_skip_key(&je))
            goto js_error;

          rem_start= (const char *) je.s.c_str;
          n_item++;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      continue;
    }

v_found:

    if (json_skip_key(&je) || json_scan_next(&je))
      goto js_error;

    rem_end= (je.state == JST_VALUE && n_item == 0) ?
      (const char *) je.s.c_str : (const char *) (je.s.c_str - je.sav_c_len);

    /*
      Taking out the first piece of an array takes the comma after it
      with it, and in a document written the loose way a space stands
      behind that comma.  That space belonged to the piece that has just
      gone; leaving it puts it behind the bracket instead, in front of a
      piece that already has all the spacing it needs.  Every other
      piece ends AT its comma and so leaves nothing over.

      Only where what is composed here is the answer.  Where it is not,
      the reading back at the end settles the spacing and drops that
      space itself, so skipping it changes no answer - but the reading
      complains, when it has to, about a position in the text it was
      given, and a text composed a character shorter than a released
      server composed it moves every such position by one.  This is the
      same reason the comma below is written the width a released
      server writes it.
    */
    if (compose_final && je.state == JST_VALUE && n_item == 0)
      rem_end= (const char *) json_skip_space(je.s.cs, (const uchar *) rem_end,
                                              (const uchar *) js->end());

    str->length(0);

    /*
      What is removed reaches from just before the piece to just before
      the piece after it, so the two ends join without punctuation
      everywhere but between two keys, where one comma has to be put
      back.  It is the only punctuation this function writes, and so
      the only place the two formats can differ.
    */
    if (append_simple(str, js->ptr(), rem_start - js->ptr()) ||
        (je.state == JST_KEY && n_item > 0 &&
         str->append(json_loose_comma, comma_len)) ||
        append_simple(str, rem_end, js->end() - rem_end))
          goto js_error; /* Out of memory. */

    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  /*
    Nothing is spliced in here - what is written is what was read,
    less the piece that was asked for - so a document that was written
    the loose way is still written that way with the piece gone, and
    reading it back would write it exactly as it stands.  See
    Item_func_json_insert::val_str() for why the answer is in js, and
    for what makes this safe to believe.

    Nothing goes in, so nothing can go deeper: whatever the document
    said about its own depth is still true of it with a piece taken
    out, and taking one out is the only thing that happens here.
  */
  if (compose_final)
  {
    if (!return_json(to, js, js_depth))
      goto js_error; /* Out of memory. */

    return to;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;
  if (json_nice(&je, str, Item_func_json_format::LOOSE, &read_back_depth))
    goto js_error;

  null_value= 0;
  /*
    Both marks come from the reading back, and so does the depth, for
    the reason given where Item_func_json_insert::val_str() marks the
    same answer.
  */
  m_marks.set(str, true, true, read_back_depth);
  return str;

js_error:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_keys::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  set_maybe_null();
  if (arg_count > 1)
    path.set_constant_flag(args[1]->const_item());
  return FALSE;
}


/*
  That function is for Item_func_json_keys::val_str exclusively.
  It utilizes the fact the resulting string is in specific format:
        ["key1", "key2"...]
*/
static int check_key_in_list(String *res,
                             const uchar *key, int key_len)
{
  const uchar *c= (const uchar *) res->ptr() + 2; /* beginning '["' */
  const uchar *end= (const uchar *) res->end() - 1; /* ending '"' */

  while (c < end)
  {
    int n_char;
    for (n_char=0; c[n_char] != '"' && n_char < key_len; n_char++)
    {
      if (c[n_char] != key[n_char])
        break;
    }
    if (c[n_char] == '"')
    {
      if (n_char == key_len)
        return 1;
    }
    else
    {
      while (c[n_char] != '"')
        n_char++;
    }
    c+= n_char + 4; /* skip ', "' */
  }
  return 0;
}


String *Item_func_json_keys::val_str(String *str)
{
  json_engine_t je;
  Json_source_watch watch;
  String *js= args[0]->val_json(&tmp_js);
  uint n_keys= 0;
  int array_counters[JSON_DEPTH_LIMIT]= {0};
  THD *thd;

  if ((args[0]->null_value))
    goto null_return;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  watch.take(js);
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (arg_count < 2)
    goto skip_search;

  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (!s_p)
      goto null_return;
    if (path_setup_nwc(&path.p,
                       def_path_charset(s_p->charset(), js->charset()),
                       (const uchar *) s_p->ptr(),
                       (const uchar *) s_p->ptr() + s_p->length()))
    {
      report_path_error(s_p, &path.p, 1);
      goto null_return;
    }
    path.parsed= path.constant;
  }

  if (args[1]->null_value)
    goto null_return;

  path.cur_step= path.p.steps;

  DBUG_ASSERT(watch.unchanged(js));
  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto err_return;

    goto null_return;
  }

skip_search:
  if (json_read_value(&je))
    goto err_return;

  if (je.value_type != JSON_VALUE_OBJECT)
    goto null_return;
  
  str->length(0);
  str->set_charset(collation.collation);
  if (str->append('['))
    goto err_return; /* Out of memory. */
  /* Parse the OBJECT collecting the keys. */
  while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
  {
    const uchar *key_start, *key_end;
    int key_len;

    switch (je.state)
    {
    case JST_KEY:
      key_start= je.s.c_str;
      do
      {
        key_end= je.s.c_str;
      } while (json_read_keyname_chr(&je) == 0);
      if (unlikely(je.s.error))
        goto err_return;
      key_len= (int)(key_end - key_start);

      if (!check_key_in_list(str, key_start, key_len))
      { 
        if ((n_keys > 0 && str->append(", ", 2)) ||
          str->append('"') ||
          append_simple(str, key_start, key_len) ||
          str->append('"'))
        goto err_return;
        n_keys++;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      if (json_skip_level(&je))
        break;
      break;
    default:
      break;
    }
  }

  if (unlikely(je.s.error || str->append(']')))
    goto err_return;

  null_value= 0;
  return str;

err_return:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_search::fix_fields(THD *thd, Item **ref)
{
  if (Item_json_str_multipath::fix_fields(thd, ref))
    return TRUE;

  if (arg_count < 4)
  {
    escape= '\\';
    return FALSE;
  }

  return fix_escape_item(thd, args[3], &tmp_js, true,
                         args[0]->collation.collation, &escape);
}


static const uint SQR_MAX_BLOB_WIDTH= (uint) sqrt(MAX_BLOB_WIDTH);

bool Item_func_json_search::fix_length_and_dec(THD *thd)
{
  collation.set(args[0]->collation);

  /*
    It's rather difficult to estimate the length of the result.
    I believe arglen^2 is the reasonable upper limit.
  */
  if (args[0]->max_length > SQR_MAX_BLOB_WIDTH)
    max_length= MAX_BLOB_WIDTH;
  else
  {
    max_length= args[0]->max_length;
    max_length*= max_length;
  }

  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;

  if (arg_count > 4)
    mark_constant_paths(paths, args+4, arg_count-4);
  set_maybe_null();
  return FALSE;
}


int Item_func_json_search::compare_json_value_wild(json_engine_t *je,
                                                   const String *cmp_str)
{
  if (je->value_type != JSON_VALUE_STRING || !je->value_escaped)
    return collation.collation->wildcmp(
        (const char *) je->value, (const char *) (je->value + je->value_len),
        cmp_str->ptr(), cmp_str->end(), escape, wild_one, wild_many) ? 0 : 1;

  {
    int esc_len;
    if (esc_value.alloced_length() < (uint) je->value_len &&
        esc_value.alloc((je->value_len / 1024 + 1) * 1024))
      return 0;

    esc_len= json_unescape(je->s.cs, je->value, je->value + je->value_len,
                           je->s.cs, (uchar *) esc_value.ptr(),
                           (uchar *) (esc_value.ptr() + 
                                      esc_value.alloced_length()));
    if (esc_len <= 0)
    {
      if (current_thd)
      {
        if (esc_len == JSON_ERROR_OUT_OF_SPACE)
          my_error(ER_OUTOFMEMORY, MYF(0), je->value_len);
        else if (esc_len == JSON_ERROR_ILLEGAL_SYMBOL)
        {
          push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_JSON_BAD_CHR, ER_THD(current_thd, ER_JSON_BAD_CHR),
                              0, "comparison",
                              (int)(je->s.c_str - je->value));
        }
      }
      return 0;
    }

    return collation.collation->wildcmp(
        esc_value.ptr(), esc_value.ptr() + esc_len,
        cmp_str->ptr(), cmp_str->end(), escape, wild_one, wild_many) ? 0 : 1;
  }
}


static int append_json_path(String *str, const json_path_t *p,
                            bool *path_bytes_well_formed)
{
  const json_path_step_t *c;

  if (str->append("\"$", 2))
    return TRUE;

  for (c= p->steps+1; c <= p->last_step; c++)
  {
    if (c->type & JSON_PATH_KEY)
    {
      if (str->append(".", 1) ||
          append_simple(str, c->key, c->key_end-c->key))
        return TRUE;
    }
    else /*JSON_PATH_ARRAY*/
    {
      /*
        The brackets go in through String::append(), which converts them,
        so a set that writes no character in one byte gets a bracket of
        its own width.  The number between them does not go that way: the
        digits are written as themselves, whatever the set.  It is the
        one part of a path not formatted the way the rest of it is, and one
        stray byte is enough to leave every character after it reading
        from the wrong place.
      */
      if (str->charset()->mbminlen > 1)
        *path_bytes_well_formed= false;

      if (str->append('[') ||
          str->append_ulonglong(c->n_item) ||
          str->append(']'))
        return TRUE;
    }
  }

  return str->append('"');
}


String *Item_func_json_search::val_str(String *str)
{
  Json_source_watch watch;
  String *js= args[0]->val_json(&tmp_js);
  watch.take(js);
  String *s_str= args[2]->val_str(&tmp_path);
  json_engine_t je;
  json_path_t p, sav_path;
  uint n_arg;
  int array_sizes[JSON_DEPTH_LIMIT];
  uint has_negative_path= 0;
  bool path_bytes_well_formed= true;

  m_marks.clear();

  if (args[0]->null_value || args[2]->null_value)
    goto null_return;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto null_return;

  n_path_found= 0;
  str->set_charset(js->charset());
  str->length(0);

  for (n_arg=4; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 4;
    c_path->p.types_used= JSON_PATH_KEY_NULL;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-4));
      if (s_p)
      {
       if (json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
       {
         report_path_error(s_p, &c_path->p, n_arg);
         goto null_return;
       }
       c_path->parsed= c_path->constant;
       has_negative_path|= c_path->p.types_used & JSON_PATH_NEGATIVE_INDEX;
      }
    }
    if (args[n_arg]->null_value)
      goto null_return;
  }

  DBUG_ASSERT(watch.unchanged(js));
  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {
    if (has_negative_path && je.value_type == JSON_VALUE_ARRAY &&
        json_skip_array_and_count(&je, array_sizes + (p.last_step - p.steps)))
      goto js_error;

    if (json_value_scalar(&je))
    {
      if ((arg_count < 5 ||
           path_ok(paths, arg_count - 4, &p, je.value_type, array_sizes)) &&
          compare_json_value_wild(&je, s_str) != 0)
      {
        ++n_path_found;
        if (n_path_found == 1)
        {
          sav_path= p;
          sav_path.last_step= sav_path.steps + (p.last_step - p.steps);
        }
        else
        {
          if (n_path_found == 2)
          {
            if (str->append('[') ||
                append_json_path(str, &sav_path, &path_bytes_well_formed))
                goto js_error;
          }
          if (str->append(STRING_WITH_LEN(json_loose_comma)) ||
              append_json_path(str, &p, &path_bytes_well_formed))
            goto js_error;
        }
        if (mode_one)
          goto end;
      }
    }
  }

  if (unlikely(je.s.error))
    goto js_error;

end:
  if (n_path_found == 0)
    goto null_return;
  if (n_path_found == 1)
  {
    if (append_json_path(str, &sav_path, &path_bytes_well_formed))
      goto js_error;
  }
  else
  {
    if (str->append(']'))
      goto js_error;
  }

  /*
    A path is written out as a JSON string, and the keys that go into it
    are copied from the document with whatever escaping they were
    written with there - a key holding a quote was already written with
    that quote escaped, or the document would not have read.  Several
    paths are put in an array with the loose formatting between them.

    How deep it goes is settled by which of those two was written and by
    nothing else: one path is a string and nests nothing, several are an
    array of strings and nest one.  That is a figure this function has
    in hand rather than one it would have to go and measure, and without
    it a caller splicing the answer falls back to guessing the depth
    from the length - which for paths of any size says more levels than
    a document is allowed and sends the whole thing to be read again.

    None of that is worth anything if the bytes do not read at all, and
    a path carrying an array index in a set that writes no character in
    one byte does not - see the number written into it.  A set that can
    write answers only for what went through it, so the path says for
    itself whether it was written, and an unencoded one is passed on as
    the bytes it is rather than as a document.
  */
  {
    bool result_is_document= is_json_compatible_charset(str->charset()) &&
                             path_bytes_well_formed;

    m_marks.set(str, result_is_document, result_is_document,
                n_path_found == 1 ? 0 : 1);
  }
  null_value= 0;
  return str;


js_error:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


LEX_CSTRING Item_func_json_format::func_name_cstring() const
{
  switch (fmt)
  {
  case COMPACT:
    return { STRING_WITH_LEN("json_compact") };
  case LOOSE:
    return { STRING_WITH_LEN("json_loose") };
  case DETAILED:
    return { STRING_WITH_LEN("json_detailed") };
  default:
    DBUG_ASSERT(0);
  };

  return NULL_clex_str;
}


bool Item_func_json_format::fix_length_and_dec(THD *thd)
{
  decimals= 0;
  collation.set(args[0]->collation);
  switch (fmt)
  {
  case COMPACT:
    max_length= args[0]->max_length;
    break;
  case LOOSE:
    max_length= args[0]->max_length * 2;
    break;
  case DETAILED:
    max_length= MAX_BLOB_WIDTH;
    break;
  default:
    DBUG_ASSERT(0);
  };
  set_maybe_null();
  return FALSE;
}


String *Item_func_json_format::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  Json_source_watch watch;
  int tab_size= 4;
  uint deepest= JSON_DEPTH_UNKNOWN;
  THD *thd;

  m_marks.clear();

  watch.take(js);
  if ((null_value= args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  if (fmt == DETAILED)
  {
    if (arg_count > 1)
    {
      tab_size= (int)args[1]->val_int();
      if (args[1]->null_value)
      {
        null_value= 1;
        return 0;
      }
    }
    if (tab_size < 0)
      tab_size= 0;
    else if (tab_size > TAB_SIZE_LIMIT)
      tab_size= TAB_SIZE_LIMIT;
  }

  DBUG_ASSERT(watch.unchanged(js));
  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr()+js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (json_nice(&je, str, fmt, &deepest, tab_size))
  {
    null_value= 1;
    report_json_error(js, &je, 0);
    return 0;
  }

  /*
    Read back and written out again, so is_valid holds whatever the
    argument was - but only json_loose() writes the loose form, the
    other two formats being the whole point of the other two names.
    The depth is what that reading measured, and no formatting changes
    it: all three write the same structures and differ only in what
    they put between them.
  */
  m_marks.set(str, true, fmt == LOOSE, deepest);
  return str;
}


/*
  Nothing is read and nothing is written: the argument's value is handed
  straight on.  So whatever the argument was able to say about it is
  still true of it here, and this function says neither more nor less.
*/
String *Item_func_json_format::val_json(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  m_marks.clear();
  if ((null_value= args[0]->null_value))
    return 0;
  m_marks.set(js, args[0]->is_valid_json(), args[0]->is_nice_json(),
              args[0]->last_depth());
  return js;
}

int Arg_comparator::compare_json_str_basic(Item *j, Item *s)
{
  String *js,*str;
  int c_len;
  json_engine_t je;

  if ((js= j->val_str(&value1)))
  {
    /* doesn't appear to json_scan_next so not interuptable */
    json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                    (const uchar *) js->ptr()+js->length());
     if (json_read_value(&je))
       goto error;
     if (je.value_type == JSON_VALUE_STRING)
     {
       if (value2.realloc_with_extra_if_needed(je.value_len))
       {
         my_error(ER_OUTOFMEMORY, MYF(0), je.value_len);
         goto error;
       }
       if ((c_len= json_unescape(js->charset(), je.value,
                               je.value + je.value_len,
                               &my_charset_utf8mb4_bin,
                               (uchar *) value2.ptr(),
                               (uchar *) (value2.ptr() + je.value_len))) < 0)
       {
         if (current_thd)
         {
           if (c_len == JSON_ERROR_OUT_OF_SPACE)
             my_error(ER_OUTOFMEMORY, MYF(0), je.value_len);
           else if (c_len == JSON_ERROR_ILLEGAL_SYMBOL)
           {
             push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                                 ER_JSON_BAD_CHR, ER_THD(current_thd, ER_JSON_BAD_CHR),
                                 0, "comparison", (int)((const char *) je.s.c_str - js->ptr()));
           }
         }
         goto error;
       }

       value2.length(c_len);
       js= &value2;
       str= &value1;
     }
     else
     {
       str= &value2;
     }


     if ((str= s->val_str(str)))
     {
       if (set_null)
         owner->null_value= 0;
       return sortcmp(js, str, compare_collation());
     }
  }

error:
  if (set_null)
    owner->null_value= 1;
  return -1;
}


int Arg_comparator::compare_e_json_str_basic(Item *j, Item *s)
{
  String *res1,*res2;
  json_value_types type;
  char *value;
  int value_len, c_len;
  Item_func_json_extract *e= (Item_func_json_extract *) j;

  res1= e->read_json(&value1, &type, &value, &value_len);
  res2= s->val_str(&value2);

  if (!res1 || !res2)
    return MY_TEST(res1 == res2);

  if (type == JSON_VALUE_STRING)
  {
    if (value1.realloc_with_extra_if_needed(value_len))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), value_len);
      return 1;
    }
    if ((c_len= json_unescape(value1.charset(), (uchar *) value,
                              (uchar *) value+value_len,
                              &my_charset_utf8mb4_bin,
                              (uchar *) value1.ptr(),
                              (uchar *) (value1.ptr() + value_len))) < 0)
    {
      if (current_thd)
      {
        if (c_len == JSON_ERROR_OUT_OF_SPACE)
          my_error(ER_OUTOFMEMORY, MYF(0), value_len);
        else if (c_len == JSON_ERROR_ILLEGAL_SYMBOL)
        {
          push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_JSON_BAD_CHR, ER_THD(current_thd, ER_JSON_BAD_CHR),
                              0, "equality comparison", 0);
        }
       }
       return 1;
    }
    value1.length(c_len);
    res1= &value1;
  }

  return MY_TEST(sortcmp(res1, res2, compare_collation()) == 0);
}

bool Item_func_json_arrayagg::fix_fields(THD *thd, Item **ref)
{
  bool res= Item_func_group_concat::fix_fields(thd, ref);
  m_tmp_json.set_charset(collation.collation);
  /* account for opening and closing brackets */
  max_length= MY_MIN(max_length + 2*collation.collation->mbminlen, UINT_MAX32);
  return res;
}


/*
  func_name() ends with '(' so that "Row %lu was cut by %s)" reads
  properly.  A diagnostic that names the function on its own wants the
  name without it.
*/
static const char json_arrayagg_name[]= "json_arrayagg";


/*
  Cleared where a group begins rather than where its result is asked
  for.  Without ORDER BY or DISTINCT the rows are written out as they
  arrive, long before anything asks for the result, so clearing the mark
  at that point would throw away what the rows had already reported.
*/
void Item_func_json_arrayagg::clear()
{
  m_bad_element= false;
  m_closed= false;
  m_elements_valid= true;
  m_elements_depth= 1;
  Item_func_group_concat::clear();
}


/*
  Returning nothing for a row is not the same as the row not being
  there.  The separator between one element and the next is written
  before the element is asked for (dump_leaf_key(), item_sum.cc), and it
  stays written whether an element follows it or not, so a row declined
  here leaves a separator with nothing on one side of it.

  That is what has always been returned for a value holding a
  character no document can carry, and it still is: the note is the new
  part, the shape is not.  A row lost to a buffer that would not grow
  is a different matter - nothing about the data asked for it, and the
  statement is over anyway - so the group is marked and refused whole
  when it is asked for.

  A row holding something that does not parse as JSON reaches neither
  of these.  It is written out as it stands, the same as it always was.
*/
String *Item_func_json_arrayagg::get_str_from_item(Item *i, String *tmp)
{
  int rc;
  /*
    One deep for the brackets the group is put inside, which is where
    every element written here sits - see Item_func_json_array::val_str().
    Carried across the elements rather than kept per element: the group
    is as deep as its deepest element, and the elements are written out
    one at a time long before the brackets go on.
  */
  Json_splice_marks marks(1);

  m_tmp_json.length(0);
  rc= append_json_value(&m_tmp_json, i, tmp, 1, json_arrayagg_name, 0, marks,
                        false, false);
  if (!marks.is_valid)
    m_elements_valid= false;
  m_elements_depth= MY_MAX(m_elements_depth, marks.deepest);

  if (rc)
  {
    if (rc == JSON_APPEND_OOM)
      m_bad_element= true;
    return NULL;
  }
  return &m_tmp_json;
}


String *Item_func_json_arrayagg::get_str_from_field(Item *i,Field *f,
    String *tmp, const uchar *key, size_t offset)
{
  int rc;
  /* One deep for the brackets - see get_str_from_item() just above. */
  Json_splice_marks marks(1);

  m_tmp_json.length(0);

  rc= append_json_value_from_field(&m_tmp_json, i, f, key, offset, tmp,
                                   1, json_arrayagg_name, 0, marks);
  if (!marks.is_valid)
    m_elements_valid= false;
  m_elements_depth= MY_MAX(m_elements_depth, marks.deepest);

  if (rc)
  {
    if (rc == JSON_APPEND_OOM)
      m_bad_element= true;
    return NULL;
  }

  return &m_tmp_json;

}


void Item_func_json_arrayagg::cut_max_length(String *result,
       uint old_length, uint max_length) const
{
  if (result->length() == 0)
    return;

  if (result->end()[-1] != '"' || old_length == max_length)
  {
    Item_func_group_concat::cut_max_length(result, old_length, max_length);
    return;
  }

  Item_func_group_concat::cut_max_length(result, old_length, max_length-1);
  result->append('"');
}


Item *Item_func_json_arrayagg::copy_or_same(THD* thd)
{
   return new (thd->mem_root) Item_func_json_arrayagg(thd, this);
}


String* Item_func_json_arrayagg::val_str(String *str)
{
  m_marks.clear();

  if ((str= Item_func_group_concat::val_str(str)))
  {
    String s;

    /*
      The brackets are built up in a buffer of their own which is then
      exchanged with the result, and the exchange carries the character
      set across along with the bytes.  Left at its default the buffer
      would say that what it holds is bytes rather than text, which
      costs twice over: a bracket is then written one byte wide even
      where a character of this result is two or four, leaving
      everything between the two of them a byte out of step; and a
      client asking for a character set other than the one the result
      was computed in is handed the bytes as they stand, where every
      other function here converts them first.
    */
    s.set_charset(collation.collation);

    /*
      A row that could not be written out left the elements it sits
      between joined by nothing but their separator, so there is no
      array here to return.  Reachable only through a failure to
      allocate, which has already raised an error of its own.
    */
    if (m_bad_element)
    {
      null_value= 1;
      return NULL;
    }

    /*
      Asked for a second time, the group is already inside its brackets
      and there is nothing left to do to it.  What can be said about it
      is said either way: the answer is about the value, and the value
      is the same one.
    */
    if (!m_closed)
    {
      /*
        The brackets are put on last, so a buffer that will not take them
        leaves behind something that reads as a value of its own rather
        than as the array it is meant to be.
      */
      if (s.append('['))
        goto bad_result;

      s.swap(*str);
      if (str->append(s) || str->append(']'))
        goto bad_result;

      m_closed= true;
    }

    /*
      A group that was cut to fit the length limit is cut at the byte
      the limit falls on, so what stands between the brackets can be
      half an element - or, where the cut lands just past an opening
      quote and that quote is written back to close the string, an
      element no row of the group ever held.  One that carried
      something not readable as JSON is a group with that in it still.
      Either way the brackets are around something this cannot answer
      for.

      Never said to be nicely spelled: what goes between the elements is
      the separator this function inherits from GROUP_CONCAT, a comma
      with nothing after it, where a nicely spelled document puts a
      space there as well.  There is no asking for another one - the
      grammar takes no SEPARATOR here and writes that comma itself.
    */
    m_marks.set(str, json_charset_can_spell(str->charset()) &&
                     m_elements_valid && !warning_for_row, false,
                m_elements_depth);
  }
  return str;

bad_result:
  null_value= 1;
  return NULL;
}


Item_func_json_objectagg::
Item_func_json_objectagg(THD *thd, Item_func_json_objectagg *item)
  :Item_sum(thd, item), m_bad_pair(false), m_closed(false),
   m_pairs_valid(true), m_pairs_depth(1)
{
  quick_group= FALSE;
  result.set_charset(collation.collation);
}


bool
Item_func_json_objectagg::fix_fields(THD *thd, Item **ref)
{
  uint i;                       /* for loop variable */
  DBUG_ASSERT(fixed() == 0);

  memcpy(orig_args, args, sizeof(Item*) * arg_count);

  if (init_sum_func_check(thd))
    return TRUE;

  set_maybe_null();

  /*
    Fix fields for select list and ORDER clause
  */

  for (i=0 ; i < arg_count ; i++)
  {
    if (args[i]->fix_fields_if_needed_for_scalar(thd, &args[i]))
      return TRUE;
    with_flags|= args[i]->with_flags;
  }

  /* skip charset aggregation for order columns */
  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return 1;

  result.set_charset(collation.collation);
  result_field= 0;
  null_value= 1;
  max_length= (uint32) MY_MIN((ulonglong) thd->gconcat_max_len()
                               / collation.collation->mbminlen
                               * collation.collation->mbmaxlen, UINT_MAX32);


  if (check_sum_func(thd, ref))
    return TRUE;

  base_flags|= item_base_t::FIXED;
  return FALSE;
}


void Item_func_json_objectagg::cleanup()
{
  DBUG_ENTER("Item_func_json_objectagg::cleanup");
  Item_sum::cleanup();

  result.length(0);
  DBUG_VOID_RETURN;
}


Item *Item_func_json_objectagg::copy_or_same(THD* thd)
{
  return new (thd->mem_root) Item_func_json_objectagg(thd, this);
}


/*
  The opening brace is written here rather than where the object is
  built up, because here the result has the character set it will be
  read in and the brace can be written at the width that asks for.  The
  closing one is written when the result is asked for, by which time the
  same is true of it, so the two ends match.
*/
void Item_func_json_objectagg::clear()
{
  result.length(0);
  null_value= 1;
  /*
    The brace is a write into the object like any other, and a group
    that lost one is refused whole rather than returned short.
  */
  m_bad_pair= false;
  m_closed= false;
  m_pairs_valid= true;
  m_pairs_depth= 1;
  if (result.append('{'))
    m_bad_pair= true;
}


bool Item_func_json_objectagg::add()
{
  StringBuffer<MAX_FIELD_WIDTH> buf;
  /*
    One deep for the braces the group is put inside, and carried across
    the pairs - see Item_func_json_arrayagg::get_str_from_item().
  */
  Json_splice_marks marks(1);
  String *key;
  int rc;

  key= args[0]->val_str(&buf);
  if (args[0]->is_null())
    return 0;

  /*
    Whether this pair needs a separator in front of it is whether a pair
    has been written already, which is what null_value says until this
    row changes it.  Comparing the buffer against the opening brace would
    be comparing it against a width that is not always one byte.
  */
  if (!null_value && result.append(STRING_WITH_LEN(", ")))
    goto bad_pair;
  null_value= 0;

  if (result.append('"'))
    goto bad_pair;

  /*
    A key with a character no document can carry has always gone in as
    however much of it could be written, and the pair has always been
    finished around it.  Only a buffer that would not grow gives up.
  */
  if ((rc= st_append_escaped(&result, key)))
  {
    if (rc == JSON_APPEND_OOM)
      goto bad_pair;
    report_bad_chr_note(func_name(), 1);
    /*
      However much of the key could be written went in and the pair was
      finished around it, so the object is complete and its keys are
      not what was asked for.
    */
    m_pairs_valid= false;
  }

  if (result.append(STRING_WITH_LEN("\":")))
    goto bad_pair;

  buf.length(0);
  rc= append_json_value(&result, args[1], &buf, 1, func_name(), 1, marks,
                        false, false);
  if (!marks.is_valid)
    m_pairs_valid= false;
  m_pairs_depth= MY_MAX(m_pairs_depth, marks.deepest);
  if (rc == JSON_APPEND_OOM)
    goto bad_pair;

  return 0;

bad_pair:
  /*
    The pair has been written out in part, and the buffer will not take
    the rest of it.  Whatever else the group goes on to add, the object
    it is building is already broken, so it is refused as a whole
    rather than returned half written.
  */
  m_bad_pair= true;
  return 0;
}


String* Item_func_json_objectagg::val_str(String* str)
{
  DBUG_ASSERT(fixed());
  m_marks.clear();

  if (null_value)
    return 0;

  if (m_bad_pair || (!m_closed && result.append('}')))
  {
    null_value= 1;
    return 0;
  }
  m_closed= true;

  /*
    Never said to be nicely written: a key is followed here by a colon
    and the value straight after it, where the loose form puts a space
    between the two.
  */
  m_marks.set(&result, is_json_compatible_charset(result.charset()) &&
                       m_pairs_valid, false, m_pairs_depth);
  return &result;
}


String *Item_func_json_normalize::val_str(String *buf)
{
  String tmp;
  json_engine_t je;
  THD *thd;
  String *raw_json= args[0]->val_str(&tmp);

  m_marks.clear();

  DYNAMIC_STRING normalized_json;
  if (init_dynamic_string(&normalized_json, NULL, 0, 0))
  {
    null_value= 1;
    return NULL;
  }

  null_value= args[0]->null_value;
  if (null_value)
    goto end;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);
  je.killed_ptr= (uint32_t *) &thd->killed;

  if (json_normalize_engine(&je, &normalized_json,
                            raw_json->ptr(), raw_json->length(),
                            raw_json->charset()))
    goto null_return;

  buf->length(0);
  buf->set_charset(collation.collation);
  if (buf->append(normalized_json.str, normalized_json.length))
    goto null_return;

  /*
    Written out afresh from a document that was read through in full,
    and written in the character set this function declares, whatever
    the argument arrived in.  Not written the loose way, though: the
    normal form puts nothing after a comma or a colon.

    As deep as what it was made from.  Normalising sorts the keys of an
    object and takes the spacing out; it puts nothing inside anything
    and takes nothing out of anything, so every structure that was there
    is there still and no other is.  So whatever the argument could say
    about its own depth is said about this, and where it said nothing
    this says nothing, which is where it stood.
  */
  m_marks.set(buf, true, false, args[0]->last_depth());
  goto end;

null_return:
  null_value= 1;

  if (je.s.error)
    report_json_error(raw_json, &je, 0);
end:
  dynstr_free(&normalized_json);
  return null_value ? NULL : buf;
}


bool Item_func_json_normalize::fix_length_and_dec(THD *thd)
{
  collation.set(&my_charset_utf8mb4_bin);
  /* 0 becomes 0.0E0, thus one character becomes 5 chars */
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * 5);
  set_maybe_null();
  return FALSE;
}


/*
  When the two values match or don't match we need to return true or false.
  But we can have some more elements in the array left or some more keys
  left in the object that we no longer want to compare. In this case,
  we want to skip the current item.
*/
void json_skip_current_level(json_engine_t *js, json_engine_t *value)
{
  json_skip_level(js);
  json_skip_level(value);
}


/*
  Put an engine back where it was remembered from, so the next candidate
  can be tried against it.

  A refusal has to survive that.  The snapshot was taken before the
  engine failed, so restoring over the failure returns an engine that
  reads as healthy, standing somewhere the scanner never reached; the
  walk then goes on questioning a document it can no longer read, and
  the complaint that is finally made - if one is made at all - names
  wherever the walk ran out rather than what was wrong with the
  document.  Nonzero says the walk is over.
*/
static int json_resume_scan(json_engine_t *je, const json_engine_t *saved)
{
  if (je->s.error)
    return 1;
  *je= *saved;
  return 0;
}


/* At least one of the two arguments is a scalar. */
bool json_find_overlap_with_scalar(json_engine_t *js, json_engine_t *value)
{
  if (json_value_scalar(value))
  {
    if (js->value_type == value->value_type)
    {
      if (js->value_type == JSON_VALUE_NUMBER)
      {
        double d_j, d_v;
        char *end;
        int err;

        d_j= js->s.cs->strntod((char *) js->value, js->value_len, &end, &err);
        d_v= value->s.cs->strntod((char *) value->value, value->value_len,
                                   &end, &err);

        return (fabs(d_j - d_v) < 1e-12);
      }
      else if (js->value_type == JSON_VALUE_STRING)
      {
        return value->value_len == js->value_len &&
               memcmp(value->value, js->value, value->value_len) == 0;
      }
    }
    return value->value_type == js->value_type;
  }
  else if (value->value_type == JSON_VALUE_ARRAY)
  {
    while (json_scan_next(value) == 0 && value->state == JST_VALUE)
    {
      if (json_read_value(value))
        return FALSE;
      if (js->value_type == value->value_type)
      {
        int res1= json_find_overlap_with_scalar(js, value);
        if (res1)
          return TRUE;
      }
      if (!json_value_scalar(value))
        json_skip_level(value);
    }
  }
  return FALSE;
}


/*
  Compare when one is object and other is array. This means we are looking
  for the object in the array. Hence, when value type of an element of the
  array is object, then compare the two objects entirely. If they are
  equal return true else return false.
*/
bool json_compare_arr_and_obj(json_engine_t *js, json_engine_t *value)
{
  st_json_engine_t loc_val= *value;
  while (json_scan_next(js) == 0 && js->state == JST_VALUE)
  {
    if (json_read_value(js))
      return FALSE;
    if (js->value_type == JSON_VALUE_OBJECT)
    {
      int res1= json_find_overlap_with_object(js, value, true);
      if (res1)
        return TRUE;
      if (json_resume_scan(value, &loc_val))
        return FALSE;
    }
    if (js->value_type == JSON_VALUE_ARRAY)
      json_skip_level(js);
  }
  return FALSE;
}


bool json_compare_arrays_in_order(json_engine_t *js, json_engine_t *value)
{
  bool res= false;
  while (json_scan_next(js) == 0 && json_scan_next(value) == 0 &&
         js->state == JST_VALUE && value->state == JST_VALUE)
  {
    if (json_read_value(js) || json_read_value(value))
      return FALSE;
    if (js->value_type != value->value_type)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
    res= check_overlaps(js, value, true);
    if (!res)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
  }
  res= (value->state == JST_ARRAY_END || value->state == JST_OBJ_END ?
        TRUE : FALSE);
  json_skip_current_level(js, value);
  return res;
}


int json_find_overlap_with_array(json_engine_t *js, json_engine_t *value,
                                 bool compare_whole)
{
  if (value->value_type == JSON_VALUE_ARRAY)
  {
    if (compare_whole)
      return json_compare_arrays_in_order(js, value);

    json_engine_t loc_value= *value, current_js= *js;

    while (json_scan_next(js) == 0 && js->state == JST_VALUE)
    {
      if (json_read_value(js))
        return FALSE;
      current_js= *js;
      while (json_scan_next(value) == 0 && value->state == JST_VALUE)
      {
        if (json_read_value(value))
          return FALSE;
        if (js->value_type == value->value_type)
        {
          int res1= check_overlaps(js, value, true);
          if (res1)
            return TRUE;
        }
        else
        {
          if (!json_value_scalar(value))
            json_skip_level(value);
        }
        if (json_resume_scan(js, &current_js))
          return FALSE;
      }
      if (json_resume_scan(value, &loc_value))
        return FALSE;
      if (!json_value_scalar(js))
        json_skip_level(js);
    }
    return FALSE;
  }
  else if (value->value_type == JSON_VALUE_OBJECT)
  {
    if (compare_whole)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
    return json_compare_arr_and_obj(js, value);
  }
  else
    return json_find_overlap_with_scalar(value, js);
}


int compare_nested_object(json_engine_t *js, json_engine_t *value)
{
  int result= 0;
  const char *value_begin= (const char*)value->s.c_str-1;
  const char *js_begin= (const char*)js->s.c_str-1;
  /*
    A refusal here means the fragment was not read to its end, so what
    lies between begin and end is a piece of a document rather than one.
    Normalizing that piece finds it unterminated and says so, which
    replaces the reason the scanner gave with a complaint about where it
    was made to stop.  The engine already carries the reason; leave it
    alone and let the caller report it.
  */
  if (json_skip_level(value) || json_skip_level(js))
    return 0;
  const char *value_end= (const char*)value->s.c_str;
  const char *js_end= (const char*)js->s.c_str;
  json_engine_t je;
  je.killed_ptr= js->killed_ptr;

  String a(value_begin, value_end-value_begin,value->s.cs);
  String b(js_begin, js_end-js_begin, js->s.cs);

  DYNAMIC_STRING a_res, b_res;
  if (init_dynamic_string(&a_res, NULL, 4096, 1024) ||
      init_dynamic_string(&b_res, NULL, 4096, 1024))
  { 
    goto error;
  }
  if (json_normalize_engine(&je, &a_res, a.ptr(), a.length(), value->s.cs))
  {
    value->s.error= je.s.error;
    goto error;
  }
  if (json_normalize_engine(&je, &b_res, b.ptr(), b.length(), value->s.cs))
  {
    js->s.error= je.s.error;
    goto error;
  }

  result= strcmp(a_res.str, b_res.str) ? 0 : 1;

error:
  dynstr_free(&a_res);
  dynstr_free(&b_res);

  return MY_TEST(result);
}


int json_find_overlap_with_object(json_engine_t *js, json_engine_t *value,
                                  bool compare_whole)
{
  if (value->value_type == JSON_VALUE_OBJECT)
  {
    if (compare_whole)
    {
      return compare_nested_object(js, value);
    }
    else
    {
      /* Find at least one common key-value pair */
      json_string_t key_name;
      bool found_key= false, found_value= false;
      json_engine_t loc_js= *js;
      const uchar *k_start, *k_end;

      json_string_set_cs(&key_name, value->s.cs);

      while (json_scan_next(value) == 0 && value->state == JST_KEY)
      {
        k_start= value->s.c_str;
        do
        {
          k_end= value->s.c_str;
        } while (json_read_keyname_chr(value) == 0);

        if (unlikely(value->s.error))
          return FALSE;

        json_string_set_str(&key_name, k_start, k_end);
        found_key= find_key_in_object(js, &key_name);
        found_value= 0;

        if (found_key)
        {
          if (json_read_value(js) || json_read_value(value))
            return FALSE;

          /*
            The value of key-value pair can be an be anything. If it is an object
            then we need to compare the whole value and if it is an array then
            we need to compare the elements in that order. So set compare_whole
            to true.
          */
          if (js->value_type == value->value_type)
            found_value= check_overlaps(js, value, true);
          if (found_value)
          {
            /*
             We have found at least one common key-value pair now.
             No need to check for more key-value pairs. So skip remaining
             jsons and return TRUE.
            */
            json_skip_current_level(js, value);
            return TRUE;
          }
          else
          {
            /*
              Key is found but value is not found. We have already
              exhausted both values for current key. Hence "reset"
              only js (first argument i.e json document) and
              continue.
            */
            if (json_resume_scan(js, &loc_js))
              return FALSE;
            continue;
          }
        }
        else
        {
          /*
            key is not found. So no need to check for value for that key.
            Read the value anyway so we get the "type" of json value.
            If is is non-scalar then skip the entire value
            (scalar values get exhausted while reading so no need to skip them).
            Then reset the json doc again.
          */
          if (json_read_value(value))
            return FALSE;
          if (!json_value_scalar(value))
            json_skip_level(value);
          if (json_resume_scan(js, &loc_js))
            return FALSE;
        }
      }
      /*
        At this point we have already returned true if any intersection exists.
        So skip jsons if not exhausted and return false.
      */
      json_skip_current_level(js, value);
      return FALSE;
    }
  }
  else if (value->value_type == JSON_VALUE_ARRAY)
  {
    if (compare_whole)
    {
      json_skip_current_level(js, value);
      return FALSE;
    }
    return json_compare_arr_and_obj(value, js);
  }
  return FALSE;
}


/*
  Find if two json documents overlap

  SYNOPSIS
    check_overlaps()
    js     - json document
    value  - value
    compare_whole - If true then find full overlap with the document in case of
                    object and comparing in-order in case of array.
                    Else find at least one match between two objects or array.

  IMPLEMENTATION
  We can compare two json datatypes if they are of same type to check if
  they are equal. When comparing between a json document and json value,
  there can be following cases:
  1) When at least one of the two json documents is of scalar type:
     1.a) If value and json document both are scalar, then return true
          if they have same type and value.
     1.b) If json document is scalar but other is array (or vice versa),
          then return true if array has at least one element of same type
          and value as scalar.
     1.c) If one is scalar and other is object, then return false because
          it can't be compared.

  2) When both arguments are of non-scalar type:
      2.a) If both arguments are arrays:
           Iterate over the value and json document. If there exists at least
           one element in other array of same type and value as that of
           element in value, then return true else return false.
      2.b) If both arguments are objects:
           Iterate over value and json document and if there exists at least
           one key-value pair common between two objects, then return true,
           else return false.
      2.c) If either of json document or value is array and other is object:
           Iterate over the array, if an element of type object is found,
           then compare it with the object (which is the other arguemnt).
           If the entire object matches i.e all they key value pairs match,
           then return true else return false.

  When we are comparing an object which is nested in other object or nested
  in an array, we need to compare all the key-value pairs, irrespective of
  what order they are in as opposed to non-nested where we return true if
  at least one match is found. However, if we have an array nested in another
  array, then we compare two arrays in that order i.e we compare
  i-th element of array 1 with i-th element of array 2.

  RETURN
    FALSE - If two json documents do not overlap
    TRUE  - if two json documents overlap
*/
bool check_overlaps(json_engine_t *js, json_engine_t *value, bool compare_whole)
{
  DBUG_EXECUTE_IF("json_check_min_stack_requirement",
                  return dbug_json_check_min_stack_requirement(););
  if (check_stack_overrun(current_thd, STACK_MIN_SIZE , NULL))
    return 1;

  switch (js->value_type)
  {
  case JSON_VALUE_OBJECT:
    return json_find_overlap_with_object(js, value, compare_whole);
  case JSON_VALUE_ARRAY:
    return json_find_overlap_with_array(js, value, compare_whole);
  default:
    return json_find_overlap_with_scalar(js, value);
  }
}

bool Item_func_json_overlaps::val_bool()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, ve;
  int result;
  THD *thd;
  Json_source_watch watch;

  if ((null_value= (js == nullptr) || args[0]->null_value))
    return 0;

  thd= current_thd;
  JSON_DO_PAUSE_EXECUTION(thd, 0.0002);

  watch.take(js);
  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  DBUG_ASSERT(watch.unchanged(js));
  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  je.killed_ptr= (uint32_t *) &thd->killed;

  json_scan_start(&ve, val->charset(), (const uchar *) val->ptr(),
                  (const uchar *) val->end());
  ve.killed_ptr= (uint32_t *) &thd->killed;

  if (json_read_value(&je) || json_read_value(&ve))
    goto error;

  result= check_overlaps(&je, &ve, false);
  if (unlikely(je.s.error || ve.s.error))
    goto error;

  return result;

error:
  if (je.s.error)
    report_json_error(js, &je, 0);
  if (ve.s.error)
    report_json_error(val, &ve, 1);
  return 0;
}

bool Item_func_json_overlaps::fix_length_and_dec(THD *thd)
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  set_maybe_null();

  return Item_bool_func::fix_length_and_dec(thd);
}
