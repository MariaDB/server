#ifndef JSON_LIB_INCLUDED
#define JSON_LIB_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_DEPTH_LIMIT 32

/*
  When error happens, the c_next of the JSON engine contains the
  character that caused the error, and the c_str is the position
  in string where the error occurs.
*/
enum json_errors {
  JE_BAD_CHR= -1,      /* Invalid character, charset handler cannot read it. */

  JE_NOT_JSON_CHR= -2, /* Character met not used in JSON. */
                       /* ASCII 00-08 for instance.       */

  JE_EOS= -3,          /* Unexpected end of string. */

  JE_SYN= -4,          /* The next character breaks the JSON syntax. */

  JE_STRING_CONST= -5, /* Character disallowed in string constant. */

  JE_ESCAPING= -6,     /* Error in the escaping. */

  JE_DEPTH= -7,        /* The limit on the JSON depth was overrun. */
};


typedef struct st_json_string_t
{
  const uchar *c_str;    /* Current position in JSON string */
  const uchar *str_end;  /* The end on the string. */
  my_wc_t c_next;        /* UNICODE of the last read character */
  int error;             /* error code. */

  CHARSET_INFO *cs;      /* Character set of the JSON string. */

  my_charset_conv_mb_wc wc; /* UNICODE conversion function. */
                            /* It's taken out of the cs just to speed calls. */
} json_string_t;


void json_string_set_cs(json_string_t *s, CHARSET_INFO *i_cs);
void json_string_set_str(json_string_t *s,
                         const uchar *str, const uchar *end);
#define json_next_char(j) \
  (j)->wc((j)->cs, &(j)->c_next, (j)->c_str, (j)->str_end)
#define json_eos(j) ((j)->c_str >= (j)->str_end)
/*
  read_string_const_chr() reads the next character of the string constant
  and saves it to the js->c_next.
  It takes into account possible escapings, so if for instance
  the string is '\b', the read_string_const_chr() sets 8.
*/
int json_read_string_const_chr(json_string_t *js);


/*
  Various JSON-related operations expect JSON path as a parameter.
  The path is a string like this "$.keyA[2].*"
  The path itself is a number of steps specifying either a key or a position
  in an array. Some of them can be wildcards.
  So the representation of the JSON path is the json_path_t class
  containing an array of json_path_step_t objects.
*/


/* Path step types - actually bitmasks to let '&' or '|' operations. */
enum json_path_step_types
{
  JSON_PATH_KEY_NULL=0,
  JSON_PATH_KEY=1,   /* Must be equal to JSON_VALUE_OBJECT. */
  JSON_PATH_ARRAY=2, /* Must be equal to JSON_VALUE_ARRAY. */
  JSON_PATH_KEY_OR_ARRAY=3,
  JSON_PATH_WILD=4, /* Step like .* or [*] */
  JSON_PATH_DOUBLE_WILD=8, /* Step like **.k or **[1] */
  JSON_PATH_KEY_WILD= 1+4,
  JSON_PATH_KEY_DOUBLEWILD= 1+8,
  JSON_PATH_ARRAY_WILD= 2+4,
  JSON_PATH_ARRAY_DOUBLEWILD= 2+8
};


typedef struct st_json_path_step_t
{
  enum json_path_step_types type;  /* The type of the step -   */
                                   /* see json_path_step_types */
  const uchar *key; /* Pointer to the beginning of the key. */
  const uchar *key_end;  /* Pointer to the end of the key. */
  uint n_item;      /* Item number in an array. No meaning for the key step. */
} json_path_step_t;


typedef struct st_json_path_t
{
  json_string_t s;  /* The string to be parsed. */
  json_path_step_t steps[JSON_DEPTH_LIMIT]; /* Steps of the path. */
  json_path_step_t *last_step; /* Points to the last step. */

  int mode_strict; /* TRUE if the path specified as 'strict' */
  enum json_path_step_types types_used; /* The '|' of all step's 'type'-s */
} json_path_t;


int json_path_setup(json_path_t *p,
                    CHARSET_INFO *i_cs, const uchar *str, const uchar *end);


/*
  The set of functions and structures below provides interface
  to the JSON text parser.
  Running the parser normally goes like this:

    json_engine_t j_eng;   // structure keeps parser's data
    json_scan_start(j_eng) // begin the parsing

    do
    {
      // The parser has read next piece of JSON
      // and set fields of j_eng structure accordingly.
      // So let's see what we have:
      switch (j_eng.state)
      {
        case JST_KEY:
           // Handle key name. See the json_read_keyname_chr()
           // Probably compare it with the keyname we're looking for
        case JST_VALUE:
           // Handle value. It is either value of the key or an array item.
           // see the json_read_value()
        case JST_OBJ_START:
          // parser found an object (the '{' in JSON)
        case JST_OBJ_END:
          // parser found the end of the object (the '}' in JSON)
        case JST_ARRAY_START:
          // parser found an array (the '[' in JSON)
        case JST_ARRAY_END:
          // parser found the end of the array (the ']' in JSON)

      };
    } while (json_scan_next() == 0);  // parse next structure

    
    if (j_eng.s.error)  // we need to check why the loop ended.
                        // Did we get to the end of JSON, or came upon error.
    {
       signal_error_in_JSON()
    }


  Parts of JSON can be quickly skipped. If we are not interested
  in a particular key, we can just skip it with json_skip_key() call.
  Similarly json_skip_level() goes right to the end of an object
  or an array.
*/


/* These are JSON parser states that user can expect and handle.  */
enum json_states {
  JST_VALUE,       /* value found      */
  JST_KEY,         /* key found        */
  JST_OBJ_START,   /* object           */
  JST_OBJ_END,     /* object ended     */
  JST_ARRAY_START, /* array            */
  JST_ARRAY_END,   /* array ended      */
  NR_JSON_USER_STATES
};


enum json_value_types
{
  JSON_VALUE_UNINITALIZED=0,
  JSON_VALUE_OBJECT=1,
  JSON_VALUE_ARRAY=2,
  JSON_VALUE_STRING=3,
  JSON_VALUE_NUMBER=4,
  JSON_VALUE_TRUE=5,
  JSON_VALUE_FALSE=6,
  JSON_VALUE_NULL=7
};


enum json_num_flags
{
  JSON_NUM_NEG=1,        /* Number is negative. */
  JSON_NUM_FRAC_PART=2,  /* The fractional part is not empty. */
  JSON_NUM_EXP=4,        /* The number has the 'e' part. */
};


typedef struct st_json_engine_t
{
  json_string_t s;  /* String to parse. */
  int sav_c_len;    /* Length of the current character.
                       Can be more than 1 for multibyte charsets */

  int state; /* The state of the parser. One of 'enum json_states'.
                It tells us what construction of JSON we've just read. */

  /* These values are only set after the json_read_value() call. */
  enum json_value_types value_type; /* type of the value.*/
  const uchar *value;      /* Points to the value. */
  const uchar *value_begin;/* Points to where the value starts in the JSON. */
  int value_escaped;       /* Flag telling if the string value has escaping.*/
  uint num_flags;  /* the details of the JSON_VALUE_NUMBER, is it negative,
                      or if it has the fractional part.
                      See the enum json_num_flags. */

  /*
    In most cases the 'value' and 'value_begin' are equal.
    They only differ if the value is a string constants. Then 'value_begin'
    points to the starting quotation mark, while the 'value' - to
    the first character of the string.
  */

  const uchar *value_end; /* Points to the next character after the value. */
  int value_len; /* The length of the value. Does not count quotations for */
                 /* string constants. */

  int stack[JSON_DEPTH_LIMIT]; /* Keeps the stack of nested JSON structures. */
  int stack_p;                 /* The 'stack' pointer. */
} json_engine_t;


int json_scan_start(json_engine_t *je,
                        CHARSET_INFO *i_cs, const uchar *str, const uchar *end);
int json_scan_next(json_engine_t *j);


/*
  json_read_keyname_chr() function assists parsing the name of an JSON key.
  It only can be called when the json_engine is in JST_KEY.
  The json_read_keyname_chr() reads one character of the name of the key,
  and puts it in j_eng.s.next_c.
  Typical usage is like this:

  if (j_eng.state == JST_KEY)
  {
    while (json_read_keyname_chr(&j) == 0)
    {
      //handle next character i.e. match it against the pattern
    }
  }
*/

int json_read_keyname_chr(json_engine_t *j);


/*
  Check if the name of the current JSON key matches
  the step of the path.
*/
int json_key_matches(json_engine_t *je, json_string_t *k);


/*
  json_read_value() function parses the JSON value syntax,
  so that we can handle the value of a key or an array item.
  It only returns meaningful result when the engine is in
  the JST_VALUE state.

  Typical usage is like this:

  if (j_eng.state ==  JST_VALUE)
  {
    json_read_value(&j_eng);
    switch(j_eng.value_type)
    {
      case JSON_VALUE_STRING:
        // get the string
        str= j_eng.value;
        str_length= j_eng.value_len;
      case JSON_VALUE_NUMBER:
        // get the number
      ... etc
    }
*/
int json_read_value(json_engine_t *j);


/*
  json_skip_key() makes parser skip the content of the current
  JSON key quickly.
  It can be called only when the json_engine state is JST_KEY.
  Typical usage is:

  if (j_eng.state == JST_KEY)
  {
    if (key_does_not_match(j_eng))
      json_skip_key(j_eng);
  }
*/

int json_skip_key(json_engine_t *j);


typedef const int *json_level_t;

/*
  json_skip_to_level() makes parser quickly get out of nested
  loops and arrays. It is used when we're not interested in what is
  there in the rest of these structures.
  The 'level' should be remembered in advance.
        json_level_t level= json_get_level(j);
        .... // getting into the nested JSON structures
        json_skip_to_level(j, level);
*/
#define json_get_level(j) (j->stack_p)

int json_skip_to_level(json_engine_t *j, int level);

/*
  json_skip_level() works as above with just current structure.
  So it gets to the end of the current JSON array or object.
*/
#define json_skip_level(json_engine) \
  json_skip_to_level((json_engine), (json_engine)->stack_p)


/*
  works as json_skip_level() but also counts items on the current
  level skipped.
*/
int json_skip_level_and_count(json_engine_t *j, int *n_items_skipped);

#define json_skip_array_item json_skip_key

/*
  Checks if the current value is of scalar type -
  not an OBJECT nor ARRAY.
*/
#define json_value_scalar(je)  ((je)->value_type > JSON_VALUE_ARRAY)


/*
  Look for the JSON PATH in the json string.
  Function can be called several times with same JSON/PATH to
  find multiple matches.
  On the first call, the json_engine_t parameter should be
  initialized with the JSON string, and the json_path_t with the JSON path
  appropriately. The 'p_cur_step' should point at the first
  step of the path.
  The 'array_counters' is the array of JSON_DEPTH_LIMIT size.
  It stores the array counters of the parsed JSON.
  If function returns 0, it means it found the match. The position of
  the match is je->s.c_str. Then we can call the json_find_path()
  with same engine/path/p_cur_step to get the next match.
  Non-zero return means no matches found.
  Check je->s.error to see if there was an error in JSON.
*/
int json_find_path(json_engine_t *je,
                   json_path_t *p, json_path_step_t **p_cur_step,
                   uint *array_counters);


typedef struct st_json_find_paths_t
{
  uint n_paths;
  json_path_t *paths;
  uint cur_depth;
  uint *path_depths;
  uint array_counters[JSON_DEPTH_LIMIT];
} json_find_paths_t;


int json_find_paths_first(json_engine_t *je, json_find_paths_t *state,
                          uint n_paths, json_path_t *paths, uint *path_depths);
int json_find_paths_next(json_engine_t *je, json_find_paths_t *state);


/*
  Converst JSON string constant into ordinary string constant
  which can involve unpacking json escapes and changing character set.
  Returns negative integer in the case of an error,
  the length of the result otherwise.
*/
int json_unescape(CHARSET_INFO *json_cs,
                  const uchar *json_str, const uchar *json_end,
                  CHARSET_INFO *res_cs,
                  uchar *res, uchar *res_end);

/*
  Converst ordinary string constant into JSON string constant.
  which can involve appropriate escaping and changing character set.
  Returns negative integer in the case of an error,
  the length of the result otherwise.
*/
int json_escape(CHARSET_INFO *str_cs, const uchar *str, const uchar *str_end,
                CHARSET_INFO *json_cs, uchar *json, uchar *json_end);


/*
  Appends the ASCII string to the json with the charset conversion.
*/
int json_append_ascii(CHARSET_INFO *json_cs,
                      uchar *json, uchar *json_end,
                      const uchar *ascii, const uchar *ascii_end);


/*
  Scan the JSON and return paths met one-by-one.
     json_get_path_start(&p)
     while (json_get_path_next(&p))
     {
       handle_the_next_path();
     }
*/

int json_get_path_start(json_engine_t *je, CHARSET_INFO *i_cs,
                        const uchar *str, const uchar *end,
                        json_path_t *p);


int json_get_path_next(json_engine_t *je, json_path_t *p);


int json_path_parts_compare(
        const json_path_step_t *a, const json_path_step_t *a_end,
        const json_path_step_t *b, const json_path_step_t *b_end,
        enum json_value_types vt);
int json_path_compare(const json_path_t *a, const json_path_t *b,
                      enum json_value_types vt);

int json_valid(const char *js, size_t js_len, CHARSET_INFO *cs);

int json_locate_key(const char *js, const char *js_end,
                    const char *kname,
                    const char **key_start, const char **key_end,
                    int *comma_pos);

#ifdef  __cplusplus
}
#endif

#endif /* JSON_LIB_INCLUDED */
