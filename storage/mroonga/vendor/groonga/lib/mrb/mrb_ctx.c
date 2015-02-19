/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/class.h>
#include <mruby/data.h>
#include <mruby/variable.h>
#include <mruby/string.h>

#include "../grn_mrb.h"
#include "mrb_ctx.h"
#include "mrb_converter.h"

static mrb_value
ctx_class_instance(mrb_state *mrb, mrb_value klass)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_ctx;
  mrb_sym iv_name;

  iv_name = mrb_intern_lit(mrb, "@instance");
  mrb_ctx = mrb_iv_get(mrb, klass, iv_name);
  if (mrb_nil_p(mrb_ctx)) {
    struct RBasic *raw_mrb_ctx;
    raw_mrb_ctx = mrb_obj_alloc(mrb, MRB_TT_DATA, mrb_class_ptr(klass));
    mrb_ctx = mrb_obj_value(raw_mrb_ctx);
    DATA_PTR(mrb_ctx) = ctx;
    mrb_iv_set(mrb, klass, iv_name, mrb_ctx);
  }

  return mrb_ctx;
}

static mrb_value
ctx_array_reference(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value mrb_id_or_name;
  grn_obj *object;

  mrb_get_args(mrb, "o", &mrb_id_or_name);

  if (mrb_fixnum_p(mrb_id_or_name)) {
    grn_id id = mrb_fixnum(mrb_id_or_name);
    object = grn_ctx_at(ctx, id);
  } else {
    mrb_value mrb_name;
    mrb_name = mrb_convert_type(mrb, mrb_id_or_name,
                                MRB_TT_STRING, "String", "to_str");
    object = grn_ctx_get(ctx,
                         RSTRING_PTR(mrb_name),
                         RSTRING_LEN(mrb_name));
  }

  return grn_mrb_value_from_grn_obj(mrb, object);
}

static mrb_value
ctx_get_rc(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(ctx->rc);
}

static mrb_value
ctx_set_rc(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int rc;

  mrb_get_args(mrb, "i", &rc);
  ctx->rc = rc;

  return mrb_fixnum_value(ctx->rc);
}

static mrb_value
ctx_get_error_level(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(ctx->errlvl);
}

static mrb_value
ctx_set_error_level(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int error_level;

  mrb_get_args(mrb, "i", &error_level);
  ctx->errlvl = error_level;

  return mrb_fixnum_value(ctx->errlvl);
}

static mrb_value
ctx_get_error_file(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_str_new_cstr(mrb, ctx->errfile);
}

static mrb_value
ctx_set_error_file(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value error_file;

  mrb_get_args(mrb, "S", &error_file);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@error_file"), error_file);
  ctx->errfile = mrb_string_value_cstr(mrb, &error_file);

  return error_file;
}

static mrb_value
ctx_get_error_line(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_fixnum_value(ctx->errline);
}

static mrb_value
ctx_set_error_line(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_int error_line;

  mrb_get_args(mrb, "i", &error_line);
  ctx->errline = error_line;

  return mrb_fixnum_value(ctx->errline);
}

static mrb_value
ctx_get_error_method(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_str_new_cstr(mrb, ctx->errfunc);
}

static mrb_value
ctx_set_error_method(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value error_method;

  mrb_get_args(mrb, "S", &error_method);
  mrb_iv_set(mrb, self, mrb_intern_lit(mrb, "@error_method"), error_method);
  ctx->errfunc = mrb_string_value_cstr(mrb, &error_method);

  return error_method;
}

static mrb_value
ctx_get_error_message(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return mrb_str_new_cstr(mrb, ctx->errbuf);
}

static mrb_value
ctx_set_error_message(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  mrb_value error_message;

  mrb_get_args(mrb, "S", &error_message);
  grn_ctx_log(ctx, "%.*s",
              RSTRING_LEN(error_message),
              RSTRING_PTR(error_message));

  return error_message;
}

static mrb_value
ctx_get_database(mrb_state *mrb, mrb_value self)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;

  return grn_mrb_value_from_grn_obj(mrb, grn_ctx_db(ctx));
}

void
grn_mrb_ctx_check(mrb_state *mrb)
{
  grn_ctx *ctx = (grn_ctx *)mrb->ud;
  grn_mrb_data *data = &(ctx->impl->mrb);
  struct RClass *module = data->module;
  struct RClass *error_class;
#define MESSAGE_SIZE 4096
  char message[MESSAGE_SIZE];

  switch (ctx->rc) {
  case GRN_SUCCESS:
    return;
  case GRN_END_OF_DATA:
    error_class = mrb_class_get_under(mrb, module, "EndOfData");
    snprintf(message, MESSAGE_SIZE,
             "end of data: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_UNKNOWN_ERROR:
    error_class = mrb_class_get_under(mrb, module, "UnknownError");
    snprintf(message, MESSAGE_SIZE,
             "unknown error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_OPERATION_NOT_PERMITTED:
    error_class = mrb_class_get_under(mrb, module, "OperationNotPermitted");
    snprintf(message, MESSAGE_SIZE,
             "operation not permitted: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_SUCH_FILE_OR_DIRECTORY:
    error_class = mrb_class_get_under(mrb, module, "NoSuchFileOrDirectory");
    snprintf(message, MESSAGE_SIZE,
             "no such file or directory: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_SUCH_PROCESS:
    error_class = mrb_class_get_under(mrb, module, "NoSuchProcess");
    snprintf(message, MESSAGE_SIZE,
             "no such process: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INTERRUPTED_FUNCTION_CALL:
    error_class = mrb_class_get_under(mrb, module, "InterruptedFunctionCall");
    snprintf(message, MESSAGE_SIZE,
             "interrupted function call: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INPUT_OUTPUT_ERROR:
    error_class = mrb_class_get_under(mrb, module, "InputOutputError");
    snprintf(message, MESSAGE_SIZE,
             "input output error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_SUCH_DEVICE_OR_ADDRESS:
    error_class = mrb_class_get_under(mrb, module, "NoSuchDeviceOrAddress");
    snprintf(message, MESSAGE_SIZE,
             "no such device or address: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_ARG_LIST_TOO_LONG:
    error_class = mrb_class_get_under(mrb, module, "ArgListTooLong");
    snprintf(message, MESSAGE_SIZE,
             "arg list too long: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_EXEC_FORMAT_ERROR:
    error_class = mrb_class_get_under(mrb, module, "ExecFormatError");
    snprintf(message, MESSAGE_SIZE,
             "exec format error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_BAD_FILE_DESCRIPTOR:
    error_class = mrb_class_get_under(mrb, module, "BadFileDescriptor");
    snprintf(message, MESSAGE_SIZE,
             "bad file descriptor: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_CHILD_PROCESSES:
    error_class = mrb_class_get_under(mrb, module, "NoChildProcesses");
    snprintf(message, MESSAGE_SIZE,
             "no child processes: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_RESOURCE_TEMPORARILY_UNAVAILABLE:
    error_class = mrb_class_get_under(mrb, module,
                                      "ResourceTemporarilyUnavailable");
    snprintf(message, MESSAGE_SIZE,
             "resource temporarily unavailable: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NOT_ENOUGH_SPACE:
    error_class = mrb_class_get_under(mrb, module, "NotEnoughSpace");
    snprintf(message, MESSAGE_SIZE,
             "not enough space: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_PERMISSION_DENIED:
    error_class = mrb_class_get_under(mrb, module, "PermissionDenied");
    snprintf(message, MESSAGE_SIZE,
             "permission denied: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_BAD_ADDRESS:
    error_class = mrb_class_get_under(mrb, module, "BadAddress");
    snprintf(message, MESSAGE_SIZE,
             "bad address: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_RESOURCE_BUSY:
    error_class = mrb_class_get_under(mrb, module, "ResourceBusy");
    snprintf(message, MESSAGE_SIZE,
             "resource busy: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_FILE_EXISTS:
    error_class = mrb_class_get_under(mrb, module, "FileExists");
    snprintf(message, MESSAGE_SIZE,
             "file exists: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_IMPROPER_LINK:
    error_class = mrb_class_get_under(mrb, module, "ImproperLink");
    snprintf(message, MESSAGE_SIZE,
             "improper link: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_SUCH_DEVICE:
    error_class = mrb_class_get_under(mrb, module, "NoSuchDevice");
    snprintf(message, MESSAGE_SIZE,
             "no such device: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NOT_A_DIRECTORY:
    error_class = mrb_class_get_under(mrb, module, "NotDirectory");
    snprintf(message, MESSAGE_SIZE,
             "not directory: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_IS_A_DIRECTORY:
    error_class = mrb_class_get_under(mrb, module, "IsDirectory");
    snprintf(message, MESSAGE_SIZE,
             "is directory: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INVALID_ARGUMENT:
    error_class = mrb_class_get_under(mrb, module, "InvalidArgument");
    snprintf(message, MESSAGE_SIZE,
             "invalid argument: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_MANY_OPEN_FILES_IN_SYSTEM:
    error_class = mrb_class_get_under(mrb, module, "TooManyOpenFilesInSystem");
    snprintf(message, MESSAGE_SIZE,
             "too many open files in system: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_MANY_OPEN_FILES:
    error_class = mrb_class_get_under(mrb, module, "TooManyOpenFiles");
    snprintf(message, MESSAGE_SIZE,
             "too many open files: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INAPPROPRIATE_I_O_CONTROL_OPERATION:
    error_class = mrb_class_get_under(mrb, module,
                                      "InappropriateIOControlOperation");
    snprintf(message, MESSAGE_SIZE,
             "inappropriate IO control operation: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_FILE_TOO_LARGE:
    error_class = mrb_class_get_under(mrb, module, "FileTooLarge");
    snprintf(message, MESSAGE_SIZE,
             "file too large: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_SPACE_LEFT_ON_DEVICE:
    error_class = mrb_class_get_under(mrb, module, "NoSpaceLeftOnDevice");
    snprintf(message, MESSAGE_SIZE,
             "no space left on device: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INVALID_SEEK:
    error_class = mrb_class_get_under(mrb, module, "InvalidSeek");
    snprintf(message, MESSAGE_SIZE,
             "invalid seek: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_READ_ONLY_FILE_SYSTEM:
    error_class = mrb_class_get_under(mrb, module, "ReadOnlyFileSystem");
    snprintf(message, MESSAGE_SIZE,
             "read only file system: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_MANY_LINKS:
    error_class = mrb_class_get_under(mrb, module, "TooManyLinks");
    snprintf(message, MESSAGE_SIZE,
             "too many links: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_BROKEN_PIPE:
    error_class = mrb_class_get_under(mrb, module, "BrokenPipe");
    snprintf(message, MESSAGE_SIZE,
             "broken pipe: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_DOMAIN_ERROR:
    error_class = mrb_class_get_under(mrb, module, "DomainError");
    snprintf(message, MESSAGE_SIZE,
             "domain error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_RESULT_TOO_LARGE:
    error_class = mrb_class_get_under(mrb, module, "ResultTooLarge");
    snprintf(message, MESSAGE_SIZE,
             "result too large: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_RESOURCE_DEADLOCK_AVOIDED:
    error_class = mrb_class_get_under(mrb, module, "ResourceDeadlockAvoided");
    snprintf(message, MESSAGE_SIZE,
             "resource deadlock avoided: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_MEMORY_AVAILABLE:
    error_class = mrb_class_get_under(mrb, module, "NoMemoryAvailable");
    snprintf(message, MESSAGE_SIZE,
             "no memory available: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_FILENAME_TOO_LONG:
    error_class = mrb_class_get_under(mrb, module, "FilenameTooLong");
    snprintf(message, MESSAGE_SIZE,
             "filename too long: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_LOCKS_AVAILABLE:
    error_class = mrb_class_get_under(mrb, module, "NoLocksAvailable");
    snprintf(message, MESSAGE_SIZE,
             "no locks available: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_FUNCTION_NOT_IMPLEMENTED:
    error_class = mrb_class_get_under(mrb, module, "FunctionNotImplemented");
    snprintf(message, MESSAGE_SIZE,
             "function not implemented: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_DIRECTORY_NOT_EMPTY:
    error_class = mrb_class_get_under(mrb, module, "DirectoryNotEmpty");
    snprintf(message, MESSAGE_SIZE,
             "directory not empty: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_ILLEGAL_BYTE_SEQUENCE:
    error_class = mrb_class_get_under(mrb, module, "IllegalByteSequence");
    snprintf(message, MESSAGE_SIZE,
             "illegal byte sequence: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_SOCKET_NOT_INITIALIZED:
    error_class = mrb_class_get_under(mrb, module, "SocketNotInitialized");
    snprintf(message, MESSAGE_SIZE,
             "socket not initialized: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_OPERATION_WOULD_BLOCK:
    error_class = mrb_class_get_under(mrb, module, "OperationWouldBlock");
    snprintf(message, MESSAGE_SIZE,
             "operation would block: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_ADDRESS_IS_NOT_AVAILABLE:
    error_class = mrb_class_get_under(mrb, module, "AddressIsNotAvailable");
    snprintf(message, MESSAGE_SIZE,
             "address is not available: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NETWORK_IS_DOWN:
    error_class = mrb_class_get_under(mrb, module, "NetworkIsDown");
    snprintf(message, MESSAGE_SIZE,
             "network is down: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NO_BUFFER:
    error_class = mrb_class_get_under(mrb, module, "NoBuffer");
    snprintf(message, MESSAGE_SIZE,
             "no buffer: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_SOCKET_IS_ALREADY_CONNECTED:
    error_class = mrb_class_get_under(mrb, module, "SocketIsAlreadyConnected");
    snprintf(message, MESSAGE_SIZE,
             "socket is already connected: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_SOCKET_IS_NOT_CONNECTED:
    error_class = mrb_class_get_under(mrb, module, "SocketIsNotConnected");
    snprintf(message, MESSAGE_SIZE,
             "socket is not connected: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_SOCKET_IS_ALREADY_SHUTDOWNED:
    error_class = mrb_class_get_under(mrb, module, "SocketIsAlreadyShutdowned");
    snprintf(message, MESSAGE_SIZE,
             "socket is already shutdowned: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_OPERATION_TIMEOUT:
    error_class = mrb_class_get_under(mrb, module, "OperationTimeout");
    snprintf(message, MESSAGE_SIZE,
             "operation timeout: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_CONNECTION_REFUSED:
    error_class = mrb_class_get_under(mrb, module, "ConnectionRefused");
    snprintf(message, MESSAGE_SIZE,
             "connection refused: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_RANGE_ERROR:
    error_class = mrb_class_get_under(mrb, module, "RangeError");
    snprintf(message, MESSAGE_SIZE,
             "range error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOKENIZER_ERROR:
    error_class = mrb_class_get_under(mrb, module, "TokenizerError");
    snprintf(message, MESSAGE_SIZE,
             "tokenizer error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_FILE_CORRUPT:
    error_class = mrb_class_get_under(mrb, module, "FileCorrupt");
    snprintf(message, MESSAGE_SIZE,
             "file corrupt: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INVALID_FORMAT:
    error_class = mrb_class_get_under(mrb, module, "InvalidFormat");
    snprintf(message, MESSAGE_SIZE,
             "invalid format: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_OBJECT_CORRUPT:
    error_class = mrb_class_get_under(mrb, module, "ObjectCorrupt");
    snprintf(message, MESSAGE_SIZE,
             "object corrupt: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_MANY_SYMBOLIC_LINKS:
    error_class = mrb_class_get_under(mrb, module, "TooManySymbolicLinks");
    snprintf(message, MESSAGE_SIZE,
             "too many symbolic links: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NOT_SOCKET:
    error_class = mrb_class_get_under(mrb, module, "NotSocket");
    snprintf(message, MESSAGE_SIZE,
             "not socket: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_OPERATION_NOT_SUPPORTED:
    error_class = mrb_class_get_under(mrb, module, "OperationNotSupported");
    snprintf(message, MESSAGE_SIZE,
             "operation not supported: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_ADDRESS_IS_IN_USE:
    error_class = mrb_class_get_under(mrb, module, "AddressIsInUse");
    snprintf(message, MESSAGE_SIZE,
             "address is in use: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_ZLIB_ERROR:
    error_class = mrb_class_get_under(mrb, module, "ZlibError");
    snprintf(message, MESSAGE_SIZE,
             "zlib error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_LZ4_ERROR:
    error_class = mrb_class_get_under(mrb, module, "LZ4Error");
    snprintf(message, MESSAGE_SIZE,
             "LZ4 error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_STACK_OVER_FLOW:
    error_class = mrb_class_get_under(mrb, module, "StackOverFlow");
    snprintf(message, MESSAGE_SIZE,
             "stack over flow: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_SYNTAX_ERROR:
    error_class = mrb_class_get_under(mrb, module, "SyntaxError");
    snprintf(message, MESSAGE_SIZE,
             "syntax error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_RETRY_MAX:
    error_class = mrb_class_get_under(mrb, module, "RetryMax");
    snprintf(message, MESSAGE_SIZE,
             "retry max: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_INCOMPATIBLE_FILE_FORMAT:
    error_class = mrb_class_get_under(mrb, module, "IncompatibleFileFormat");
    snprintf(message, MESSAGE_SIZE,
             "incompatible file format: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_UPDATE_NOT_ALLOWED:
    error_class = mrb_class_get_under(mrb, module, "UpdateNotAllowed");
    snprintf(message, MESSAGE_SIZE,
             "update not allowed: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_SMALL_OFFSET:
    error_class = mrb_class_get_under(mrb, module, "TooSmallOffset");
    snprintf(message, MESSAGE_SIZE,
             "too small offset: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_LARGE_OFFSET:
    error_class = mrb_class_get_under(mrb, module, "TooLargeOffset");
    snprintf(message, MESSAGE_SIZE,
             "too large offset: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_TOO_SMALL_LIMIT:
    error_class = mrb_class_get_under(mrb, module, "TooSmallLimit");
    snprintf(message, MESSAGE_SIZE,
             "too small limit: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_CAS_ERROR:
    error_class = mrb_class_get_under(mrb, module, "CASError");
    snprintf(message, MESSAGE_SIZE,
             "CAS error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_UNSUPPORTED_COMMAND_VERSION:
    error_class = mrb_class_get_under(mrb, module, "UnsupportedCommandVersion");
    snprintf(message, MESSAGE_SIZE,
             "unsupported command version: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  case GRN_NORMALIZER_ERROR:
    error_class = mrb_class_get_under(mrb, module, "NormalizerError");
    snprintf(message, MESSAGE_SIZE,
             "normalizer error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  default:
    error_class = mrb_class_get_under(mrb, module, "Error");
    snprintf(message, MESSAGE_SIZE,
             "unsupported error: <%s>(%d)",
             ctx->errbuf, ctx->rc);
    break;
  }
#undef MESSAGE_SIZE

  mrb_raise(mrb, error_class, message);
}

void
grn_mrb_ctx_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *klass;

  klass = mrb_define_class_under(mrb, module, "Context", mrb->object_class);
  MRB_SET_INSTANCE_TT(klass, MRB_TT_DATA);

  mrb_define_class_method(mrb, klass, "instance",
                          ctx_class_instance, MRB_ARGS_NONE());

  mrb_define_method(mrb, klass, "[]", ctx_array_reference, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "rc", ctx_get_rc, MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "rc=", ctx_set_rc, MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_level", ctx_get_error_level,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_level=", ctx_set_error_level,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_file", ctx_get_error_file,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_file=", ctx_set_error_file,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_line", ctx_get_error_line,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_line=", ctx_set_error_line,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_method", ctx_get_error_method,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_method=", ctx_set_error_method,
                    MRB_ARGS_REQ(1));
  mrb_define_method(mrb, klass, "error_message", ctx_get_error_message,
                    MRB_ARGS_NONE());
  mrb_define_method(mrb, klass, "error_message=", ctx_set_error_message,
                    MRB_ARGS_REQ(1));

  mrb_define_method(mrb, klass, "database", ctx_get_database,
                    MRB_ARGS_NONE());
}
#endif
