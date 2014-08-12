/* Todo: SkySQL copyrights */

class Json_writer;

/*
  The idea is to catch arrays that can be printed on one line:

    arrayName : [ "boo", 123, 456 ] 

  and actually print them on one line. Arrrays that occupy too much space on
  the line, or have nested members cannot be printed on one line.
  
  We hook into JSON printing functions and try to detect the pattern. While
  detecting the pattern, we will accumulate "boo", 123, 456 as strings.

  Then, 
   - either the pattern is broken, and we print the elements out, 
   - or the pattern lasts till the end of the array, and we print the 
     array on one line.

  TODO: 
    fix the quoting. If we start to accumulate an array and but then it grows
    too large to be printed on one line, the elements will be printed as
    strings (even if some of them could be initially numbers).
*/

class Single_line_formatting_helper
{
  enum enum_state
  {
    INACTIVE,
    ADD_MEMBER,
    IN_ARRAY,
    DISABLED
  };

  enum enum_state state;
  enum { MAX_LINE_LEN= 80 };
  char buffer[80];
  char *buf_ptr;
  uint line_len;

  Json_writer *owner;
public:
  Single_line_formatting_helper() : state(INACTIVE), buf_ptr(buffer) {}

  void init(Json_writer *owner_arg) { owner= owner_arg; }

  bool on_add_member(const char *name);

  bool on_start_array();
  bool on_end_array();
  void on_start_object();
  // on_end_object() is not needed.
   
  bool on_add_str(const char *str);

  void flush_on_one_line();
  void disable_and_flush();
};


/*
  A class to write well-formed JSON documents. The documents are also formatted
  for human readability.
*/

class Json_writer
{
public:
  /* Add a member. We must be in an object. */
  Json_writer& add_member(const char *name);
  
  /* Add atomic values */
  void add_str(const char* val);
  void add_str(const String &str);

  void add_ll(longlong val);
  void add_double(double val);
  void add_bool(bool val);

private:
  void add_unquoted_str(const char* val);
public:
  /* Start a child object */
  void start_object();
  void start_array();

  void end_object();
  void end_array();
  
  Json_writer() : 
    indent_level(0), document_start(true), element_started(false), 
    first_child(true)
  {
    fmt_helper.init(this);
  }
private:
  // TODO: a stack of (name, bool is_object_or_array) elements.
  int indent_level;
  enum { INDENT_SIZE = 2 };
 
  friend class Single_line_formatting_helper;
  bool document_start;
  bool element_started;
  bool first_child;

  Single_line_formatting_helper fmt_helper;

  void append_indent();
  void start_element();
  void start_sub_element();

  //const char *new_member_name;
public:
  String output;
};

