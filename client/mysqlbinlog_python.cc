
#define PY_SSIZE_T_CLEAN
#include "Python.h"
#include <stdio.h>
#include <stdlib.h>


//#include "rpl_gtid.h"
//#include "log_event.h"

/* /client */
#include "mysqlbinlog_python.h"
#include "mysqlbinlog.h"

PyStatus MaPyBinlog::init_config()
{
  PyStatus status;
  PyConfig config;
  PyConfig_InitPythonConfig(&config);
  /* Set the program name before reading the configuration
  (decode byte string from the locale encoding).
  Implicitly preinitialize Python. */
  status = PyConfig_SetBytesString(&config, &config.program_name, program_name);
  if (PyStatus_Exception(status)) {
    goto done;
  }
  /* Read all configuration at once */
  status = PyConfig_Read(&config);
  if (PyStatus_Exception(status)) {
    goto done;
  }
  /* Specify sys.path explicitly */
  /* If you want to modify the default set of paths, finish
  initialization first and then use PySys_GetObject("path") */
  config.module_search_paths_set = 1;
  status =
      //PyWideStringList_Append(&config.module_search_paths, L"/path/to/stdlib");
      PyWideStringList_Append(&config.module_search_paths, L"/usr/include");
  if (PyStatus_Exception(status)) {
    goto done;
  }
  status = PyWideStringList_Append(&config.module_search_paths,
                                   L"/home/brandon/workspace/server/client/lib");
  if (PyStatus_Exception(status)) {
    goto done;
  }

  status = PyWideStringList_Append(&config.module_search_paths,
                                   user_script_dir);
  if (PyStatus_Exception(status)) {
    goto done;
  }
  /* Override executable computed by PyConfig_Read() */
  //status = PyConfig_SetString(&config, &config.executable,
  //                            L"/path/to/my_executable");
  //if (PyStatus_Exception(status)) {
  //  goto done;
  //}
  status = Py_InitializeFromConfig(&config);

done:
  PyConfig_Clear(&config);
  return status;
}


int MaPyBinlog::load_user_module()
{
  user_module= PyImport_ImportModule(module_name);
  if (user_module == NULL)
  {
    fprintf(stdout, "ERROR: Could not load user python module\n");
    return 1;
  }

  process_ev_func = PyObject_GetAttrString(user_module, "process_event");
  if (process_ev_func == NULL)
  {
    fprintf(stdout, "ERROR: Could not find process_event() function in user module\n");
    return 1;
  }

  return 0;
}

int MaPyBinlog::load_binlog_types()
{
  binlog_types_module= PyImport_ImportModule("binlog_types");
  if (binlog_types_module == NULL)
  {
    fprintf(stdout, "ERROR: Could not load binlog_types python module\n");
    return 1;
  }

  gtid_type = PyObject_GetAttrString(binlog_types_module, "GTID");
  if (gtid_type == NULL)
  {
    fprintf(stdout, "ERROR: Could not load binlog_types GTID python type\n");
    return 1;
  }

  event_type= PyObject_GetAttrString(binlog_types_module, "TrxEvent");
  if (event_type == NULL)
  {
    fprintf(stdout, "ERROR: Could not load binlog_types Event python type\n");
    return 1;
  }

  return 0;
}

int MaPyBinlog::process_event(Log_event *event)
{
  PyObject *gtid_obj= PyObject_CallFunction(gtid_type, "lll", 0,1,1);
  if (gtid_obj == NULL)
  {
    fprintf(stdout, "ERROR: Could not instantiate GTID\n");
    return 1;
  }
  
  PyObject *event_obj= PyObject_CallFunction(event_type, "l,l,l,l,N", 0, 0, 0, 0, gtid_obj);
  if (event_obj == NULL)
  {
    fprintf(stdout, "ERROR: Could not instantiate event\n");
    return 1;
  }

  PyObject_CallOneArg(process_ev_func, event_obj);

  Py_XDECREF(event_obj);
  Py_XDECREF(gtid_obj);

  //return OK_CONTINUE;
  return 0;
}

//void MaPyBinlog::process_event(Log_event *event)
//{
//  PyObject *replay_ctx_module= PyImport_ImportModule("replay_ctx");
//  PyObject *replay_ctx_type = PyObject_GetAttrString(replay_ctx_module, "ReplayContext");
//  PyObject *replay_ctx = PyObject_CallNoArgs(replay_ctx_type);
//  PyObject *add_gtid_func = PyObject_GetAttrString(replay_ctx, "new_gtid");
//
//  // New GTID type
//  PyObject_CallOneArg(add_gtid_func, PyObject_CallFunction(gtid_type, "lll", 0,1,1));
//  PyObject_CallOneArg(add_gtid_func, PyObject_CallFunction(gtid_type, "lll", 0,1,2));
//  PyObject_CallOneArg(add_gtid_func, PyObject_CallFunction(gtid_type, "lll", 0,1,3));
//  PyObject_CallOneArg(add_gtid_func, PyObject_CallFunction(gtid_type, "lll", 1,2,1));
//  PyObject_CallOneArg(add_gtid_func, PyObject_CallFunction(gtid_type, "lll", 1,2,3));
//  PyObject_CallOneArg(add_gtid_func, PyObject_CallFunction(gtid_type, "lll", 1,2,2));
//
//  PyObject_Print(replay_ctx, stdout, Py_PRINT_RAW);
//
//  Py_XDECREF(add_gtid_func);
//  Py_XDECREF(replay_ctx);
//  Py_XDECREF(replay_ctx_type);
//
//  //return OK_CONTINUE;
//}
