/*
*/

#ifndef _mysqlbinlog_python_h
#define _mysqlbinlog_python_h

#define PY_SSIZE_T_CLEAN
#include "Python.h"

#include "log_event.h"
#include "mysqlbinlog.h"

class MaPyBinlog
{
private:
  const char *program_name;
  const char *module_name;

  const wchar_t *user_script_dir;

  /* PyObjects for caching modules */
  PyObject *user_module;
  PyObject *binlog_types_module;

  /* PyObjects for caching types */
  PyObject *gtid_type;
  PyObject *event_type;

  /* PyObjects for caching API calls */
  PyObject *process_ev_func;


public:
  MaPyBinlog(const char *program_name, const char *module_name,
                         const wchar_t *user_script_dir)
      : program_name(program_name), module_name(module_name),
        user_script_dir(user_script_dir), user_module(NULL)
  {
  }

  ~MaPyBinlog()
  {
    Py_XDECREF(process_ev_func);
    Py_XDECREF(user_module);
    Py_XDECREF(gtid_type);
    Py_XDECREF(event_type);
    Py_XDECREF(binlog_types_module);
  }

  PyStatus init_config();
  int load_user_module();
  int load_binlog_types();
  int process_event(Log_event *ev);
};

/*
  Things to consider:
    1. Pass along initial program options to mysqlbinlog
    2. Create a return status for options to...
      a. Continue as normal
      b. Ignore event and continue processing
      c. Stop processing
    3. Pass to python function:
      a. Original query
      b. Log event type
      c. Log event binary/hex dump info?
      d. The current running status of mysqlbinlog
      e. 
    _. Consider what else a user would care about, look through Log_event?
      _. E.g. to make their own flashback, to make their own data initializer..
        so they would need "before" and "after" for each row 
*/

//PyStatus ma_py_init(const char *program_name, const char *user_script_dir, const char *module_name);
//Exit_status ma_py_process_event(Log_event *event);

#endif
