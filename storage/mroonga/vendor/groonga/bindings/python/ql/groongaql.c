/* Copyright(C) 2007 Brazil

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
#include <Python.h>
#include <groonga.h>

/* TODO: use exception */

typedef struct {
  PyObject_HEAD
  grn_ctx ctx;
  int closed;
} groongaql_ContextObject;

static PyTypeObject groongaql_ContextType;

/* Object methods */

static PyObject *
groongaql_ContextObject_new(PyTypeObject *type, PyObject *args, PyObject *keywds)
{
  grn_rc rc;
  int flags;
  grn_encoding encoding;
  groongaql_ContextObject *self;

  static char *kwlist[] = {"flags", "encoding", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, keywds, "ii", kwlist,
                                   &flags, &encoding)) {
    return NULL;
  }
  if (!(self = (groongaql_ContextObject *)type->tp_alloc(type, 0))) {
    return NULL;
  }
  Py_BEGIN_ALLOW_THREADS
  rc = grn_ctx_init(&self->ctx, flags);
  GRN_CTX_SET_ENCODING(&self->ctx, encoding);
  Py_END_ALLOW_THREADS
  if (rc) {
    self->ob_type->tp_free(self);
    return NULL;
  }
  self->closed = 0;
  return (PyObject *)self;
}

static void
groongaql_ContextObject_dealloc(groongaql_ContextObject *self)
{
  if (!self->closed) {
    Py_BEGIN_ALLOW_THREADS
    grn_ctx_fin(&self->ctx);
    Py_END_ALLOW_THREADS
  }
  self->ob_type->tp_free(self);
}

/* Class methods */

/* instance methods */

static PyObject *
groongaql_ContextClass_ql_connect(groongaql_ContextObject *self, PyObject *args, PyObject *keywds)
{
  grn_rc rc;
  int port, flags;
  const char *host;
  static char *kwlist[] = {"host", "port", "flags", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, keywds, "sii", kwlist,
                                   &host, &port, &flags)) {
    return NULL;
  }
  if (self->closed) { return NULL; }
  Py_BEGIN_ALLOW_THREADS
  rc = grn_ctx_connect(&self->ctx, host, port, flags);
  Py_END_ALLOW_THREADS
  return Py_BuildValue("i", rc);
}

static PyObject *
groongaql_Context_ql_send(groongaql_ContextObject *self, PyObject *args, PyObject *keywds)
{
  grn_rc rc;
  char *str;
  int str_len, flags;
  static char *kwlist[] = {"str", "flags", NULL};

  if (!PyArg_ParseTupleAndKeywords(args, keywds, "s#i", kwlist,
                                   &str,
                                   &str_len,
                                   &flags)) {
    return NULL;
  }
  if (self->closed) { return NULL; }
  Py_BEGIN_ALLOW_THREADS
  rc = grn_ctx_send(&self->ctx, str, str_len, flags);
  Py_END_ALLOW_THREADS
  return Py_BuildValue("i", rc);
}

static PyObject *
groongaql_Context_ql_recv(groongaql_ContextObject *self)
{
  grn_rc rc;
  int flags;
  char *str;
  unsigned int str_len;

  if (self->closed) { return NULL; }
  Py_BEGIN_ALLOW_THREADS
  rc = grn_ctx_recv(&self->ctx, &str, &str_len, &flags);
  Py_END_ALLOW_THREADS
  return Py_BuildValue("(is#i)", rc, str, str_len, flags);
}

static PyObject *
groongaql_Context_fin(groongaql_ContextObject *self)
{
  grn_rc rc;

  if (self->closed) { return NULL; }
  Py_BEGIN_ALLOW_THREADS
  rc = grn_ctx_fin(&self->ctx);
  Py_END_ALLOW_THREADS
  if (!rc) {
    self->closed = 1;
  }
  return Py_BuildValue("i", rc);
}

static PyObject *
groongaql_Context_ql_info_get(groongaql_ContextObject *self)
{
  grn_rc rc;
  grn_ctx_info info;

  if (self->closed) { return NULL; }
  rc = grn_ctx_info_get(&self->ctx, &info);
  /* TODO: handling unsigned int properlly */
  /* TODO: get outbuf */
  return Py_BuildValue("{s:i,s:i,s:i}",
                       "rc", rc,
                       "com_status", info.com_status,
                       /* "outbuf", info.outbuf, */
                       "stat", info.stat
                      );
}

/* methods of classes */

static PyMethodDef groongaql_Context_methods[] = {
  {"ql_connect", (PyCFunction)groongaql_ContextClass_ql_connect,
   METH_VARARGS | METH_KEYWORDS,
   "Create a remote groonga context."},
  {"ql_send", (PyCFunction)groongaql_Context_ql_send,
   METH_VARARGS | METH_KEYWORDS,
   "Send message to context."},
  {"ql_recv", (PyCFunction)groongaql_Context_ql_recv,
   METH_NOARGS,
   "Receive message from context."},
  {"fin", (PyCFunction)groongaql_Context_fin,
   METH_NOARGS,
   "Release groonga context."},
  {"ql_info_get", (PyCFunction)groongaql_Context_ql_info_get,
   METH_NOARGS,
   "Get QL context information."},
  {NULL, NULL, 0, NULL}
};

static PyMethodDef module_methods[] = {
  {NULL, NULL, 0, NULL}
};

/* type objects */

static PyTypeObject groongaql_ContextType = {
  PyObject_HEAD_INIT(NULL)
  0,                                           /* ob_size */
  "groongaql.Context",                          /* tp_name */
  sizeof(groongaql_ContextObject),              /* tp_basicsize */
  0,                                           /* tp_itemsize */
  (destructor)groongaql_ContextObject_dealloc,  /* tp_dealloc */
  0,                                           /* tp_print */
  0,                                           /* tp_getattr */
  0,                                           /* tp_setattr */
  0,                                           /* tp_compare */
  0,                                           /* tp_repr */
  0,                                           /* tp_as_number */
  0,                                           /* tp_as_sequence */
  0,                                           /* tp_as_mapping */
  0,                                           /* tp_hash  */
  0,                                           /* tp_call */
  0,                                           /* tp_str */
  0,                                           /* tp_getattro */
  0,                                           /* tp_setattro */
  0,                                           /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT,                          /* tp_flags */
  "groonga Context objects",                     /* tp_doc */
  0,                                           /* tp_traverse */
  0,                                           /* tp_clear */
  0,                                           /* tp_richcompare */
  0,                                           /* tp_weaklistoffset */
  0,                                           /* tp_iter */
  0,                                           /* tp_iternext */
  groongaql_Context_methods,                    /* tp_methods */
  0,                                           /* tp_members */
  0,                                           /* tp_getset */
  0,                                           /* tp_base */
  0,                                           /* tp_dict */
  0,                                           /* tp_descr_get */
  0,                                           /* tp_descr_set */
  0,                                           /* tp_dictoffset */
  0,                                           /* tp_init */
  0,                                           /* tp_alloc */
  groongaql_ContextObject_new,                  /* tp_new */
};

/* consts */

typedef struct _ConstPair {
  const char *name;
  int value;
} ConstPair;

static ConstPair consts[] = {
  /* grn_rc */
  {"SUCCESS", GRN_SUCCESS},
  {"END_OF_DATA", GRN_END_OF_DATA},
  {"UNKNOWN_ERROR", GRN_UNKNOWN_ERROR},
  {"OPERATION_NOT_PERMITTED", GRN_OPERATION_NOT_PERMITTED},
  {"NO_SUCH_FILE_OR_DIRECTORY", GRN_NO_SUCH_FILE_OR_DIRECTORY},
  {"NO_SUCH_PROCESS", GRN_NO_SUCH_PROCESS},
  {"INTERRUPTED_FUNCTION_CALL", GRN_INTERRUPTED_FUNCTION_CALL},
  {"INPUT_OUTPUT_ERROR", GRN_INPUT_OUTPUT_ERROR},
  {"NO_SUCH_DEVICE_OR_ADDRESS", GRN_NO_SUCH_DEVICE_OR_ADDRESS},
  {"ARG_LIST_TOO_LONG", GRN_ARG_LIST_TOO_LONG},
  {"EXEC_FORMAT_ERROR", GRN_EXEC_FORMAT_ERROR},
  {"BAD_FILE_DESCRIPTOR", GRN_BAD_FILE_DESCRIPTOR},
  {"NO_CHILD_PROCESSES", GRN_NO_CHILD_PROCESSES},
  {"RESOURCE_TEMPORARILY_UNAVAILABLE", GRN_RESOURCE_TEMPORARILY_UNAVAILABLE},
  {"NOT_ENOUGH_SPACE", GRN_NOT_ENOUGH_SPACE},
  {"PERMISSION_DENIED", GRN_PERMISSION_DENIED},
  {"BAD_ADDRESS", GRN_BAD_ADDRESS},
  {"RESOURCE_BUSY", GRN_RESOURCE_BUSY},
  {"FILE_EXISTS", GRN_FILE_EXISTS},
  {"IMPROPER_LINK", GRN_IMPROPER_LINK},
  {"NO_SUCH_DEVICE", GRN_NO_SUCH_DEVICE},
  {"NOT_A_DIRECTORY", GRN_NOT_A_DIRECTORY},
  {"IS_A_DIRECTORY", GRN_IS_A_DIRECTORY},
  {"INVALID_ARGUMENT", GRN_INVALID_ARGUMENT},
  {"TOO_MANY_OPEN_FILES_IN_SYSTEM", GRN_TOO_MANY_OPEN_FILES_IN_SYSTEM},
  {"TOO_MANY_OPEN_FILES", GRN_TOO_MANY_OPEN_FILES},
  {"INAPPROPRIATE_I_O_CONTROL_OPERATION", GRN_INAPPROPRIATE_I_O_CONTROL_OPERATION},
  {"FILE_TOO_LARGE", GRN_FILE_TOO_LARGE},
  {"NO_SPACE_LEFT_ON_DEVICE", GRN_NO_SPACE_LEFT_ON_DEVICE},
  {"INVALID_SEEK", GRN_INVALID_SEEK},
  {"READ_ONLY_FILE_SYSTEM", GRN_READ_ONLY_FILE_SYSTEM},
  {"TOO_MANY_LINKS", GRN_TOO_MANY_LINKS},
  {"BROKEN_PIPE", GRN_BROKEN_PIPE},
  {"DOMAIN_ERROR", GRN_DOMAIN_ERROR},
  {"RESULT_TOO_LARGE", GRN_RESULT_TOO_LARGE},
  {"RESOURCE_DEADLOCK_AVOIDED", GRN_RESOURCE_DEADLOCK_AVOIDED},
  {"NO_MEMORY_AVAILABLE", GRN_NO_MEMORY_AVAILABLE},
  {"FILENAME_TOO_LONG", GRN_FILENAME_TOO_LONG},
  {"NO_LOCKS_AVAILABLE", GRN_NO_LOCKS_AVAILABLE},
  {"FUNCTION_NOT_IMPLEMENTED", GRN_FUNCTION_NOT_IMPLEMENTED},
  {"DIRECTORY_NOT_EMPTY", GRN_DIRECTORY_NOT_EMPTY},
  {"ILLEGAL_BYTE_SEQUENCE", GRN_ILLEGAL_BYTE_SEQUENCE},
  {"SOCKET_NOT_INITIALIZED", GRN_SOCKET_NOT_INITIALIZED},
  {"OPERATION_WOULD_BLOCK", GRN_OPERATION_WOULD_BLOCK},
  {"ADDRESS_IS_NOT_AVAILABLE", GRN_ADDRESS_IS_NOT_AVAILABLE},
  {"NETWORK_IS_DOWN", GRN_NETWORK_IS_DOWN},
  {"NO_BUFFER", GRN_NO_BUFFER},
  {"SOCKET_IS_ALREADY_CONNECTED", GRN_SOCKET_IS_ALREADY_CONNECTED},
  {"SOCKET_IS_NOT_CONNECTED", GRN_SOCKET_IS_NOT_CONNECTED},
  {"SOCKET_IS_ALREADY_SHUTDOWNED", GRN_SOCKET_IS_ALREADY_SHUTDOWNED},
  {"OPERATION_TIMEOUT", GRN_OPERATION_TIMEOUT},
  {"CONNECTION_REFUSED", GRN_CONNECTION_REFUSED},
  {"RANGE_ERROR", GRN_RANGE_ERROR},
  {"TOKENIZER_ERROR", GRN_TOKENIZER_ERROR},
  {"FILE_CORRUPT", GRN_FILE_CORRUPT},
  {"INVALID_FORMAT", GRN_INVALID_FORMAT},
  {"OBJECT_CORRUPT", GRN_OBJECT_CORRUPT},
  {"TOO_MANY_SYMBOLIC_LINKS", GRN_TOO_MANY_SYMBOLIC_LINKS},
  {"NOT_SOCKET", GRN_NOT_SOCKET},
  {"OPERATION_NOT_SUPPORTED", GRN_OPERATION_NOT_SUPPORTED},
  {"ADDRESS_IS_IN_USE", GRN_ADDRESS_IS_IN_USE},
  {"ZLIB_ERROR", GRN_ZLIB_ERROR},
  {"LZO_ERROR", GRN_LZO_ERROR},
  /* grn_encoding */
  {"ENC_DEFAULT", GRN_ENC_DEFAULT},
  {"ENC_NONE", GRN_ENC_NONE},
  {"ENC_EUC_JP", GRN_ENC_EUC_JP},
  {"ENC_UTF8", GRN_ENC_UTF8},
  {"ENC_SJIS", GRN_ENC_SJIS},
  {"ENC_LATIN1", GRN_ENC_LATIN1},
  {"ENC_KOI8R", GRN_ENC_KOI8R},
  /* grn_ctx flags */
  {"CTX_USE_QL", GRN_CTX_USE_QL},
  {"CTX_BATCH_MODE", GRN_CTX_BATCH_MODE},
  {"CTX_MORE", GRN_CTX_MORE},
  {"CTX_TAIL", GRN_CTX_TAIL},
  {"CTX_HEAD", GRN_CTX_HEAD},
  {"CTX_QUIET", GRN_CTX_QUIET},
  {"CTX_QUIT", GRN_CTX_QUIT},
  {"CTX_FIN", GRN_CTX_FIN},
  /* end */
  {NULL, 0}
};

#ifndef PyMODINIT_FUNC
#define PyMODINIT_FUNC void
#endif
PyMODINIT_FUNC
initgroongaql(void)
{
  unsigned int i;
  PyObject *m, *dict;

  if (!(m = Py_InitModule3("groongaql", module_methods,
                           "groonga ql module."))) {
    goto exit;
  }
  grn_init();

  /* register classes */

  if (PyType_Ready(&groongaql_ContextType) < 0) { goto exit; }
  Py_INCREF(&groongaql_ContextType);
  PyModule_AddObject(m, "Context", (PyObject *)&groongaql_ContextType);

  /* register consts */

  if (!(dict = PyModule_GetDict(m))) { goto exit; }

  for (i = 0; consts[i].name; i++) {
    PyObject *v;
    if (!(v = PyInt_FromLong(consts[i].value))) {
      goto exit;
    }
    PyDict_SetItemString(dict, consts[i].name, v);
    Py_DECREF(v);
  }
  if (Py_AtExit((void (*)(void))grn_fin)) { goto exit; }
exit:
  if (PyErr_Occurred()) {
    PyErr_SetString(PyExc_ImportError, "groongaql: init failed");
  }
}
