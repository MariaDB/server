/* Todo: SkySQL copyrights */

class Json_writer
{
public:
  /* Add a member. We must be in an object. */
  Json_writer& add_member(const char *name);
  
  /* Add atomic values */
  void add_ll(longlong val);
  void add_str(const char* val);
  void add_str(const String &str);
  void add_double(double val);
  void add_bool(bool val);
  
  /* Start a child object */
  void start_object();
  void start_array();

  void end_object();
  void end_array();
  
  Json_writer() : 
    indent_level(0), document_start(true), element_started(false), 
    first_child(true)
  {}
private:
  // stack of (name, bool is_object_or_array) elements.
  int indent_level;
  enum { INDENT_SIZE = 2 };
  
  bool document_start;
  bool element_started;
  bool first_child;

  void append_indent();
  void start_element();

  //const char *new_member_name;
public:
  String output;
};

