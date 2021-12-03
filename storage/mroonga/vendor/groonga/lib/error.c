/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2017 Brazil

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

#include "grn_error.h"
#include "grn_windows.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif /* HAVE_ERRNO_H */

#include <string.h>

#ifdef WIN32

grn_rc
grn_windows_error_code_to_rc(int error_code)
{
  grn_rc rc;

  switch (error_code) {
  case ERROR_FILE_NOT_FOUND :
  case ERROR_PATH_NOT_FOUND :
    rc = GRN_NO_SUCH_FILE_OR_DIRECTORY;
    break;
  case ERROR_TOO_MANY_OPEN_FILES :
    rc = GRN_TOO_MANY_OPEN_FILES;
    break;
  case ERROR_ACCESS_DENIED :
    rc = GRN_PERMISSION_DENIED;
    break;
  case ERROR_INVALID_HANDLE :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_ARENA_TRASHED :
    rc = GRN_ADDRESS_IS_NOT_AVAILABLE;
    break;
  case ERROR_NOT_ENOUGH_MEMORY :
    rc = GRN_NO_MEMORY_AVAILABLE;
    break;
  case ERROR_INVALID_BLOCK :
  case ERROR_BAD_ENVIRONMENT :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_BAD_FORMAT :
    rc = GRN_INVALID_FORMAT;
    break;
  case ERROR_INVALID_DATA :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_OUTOFMEMORY :
    rc = GRN_NO_MEMORY_AVAILABLE;
    break;
  case ERROR_INVALID_DRIVE :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_WRITE_PROTECT :
    rc = GRN_PERMISSION_DENIED;
    break;
  case ERROR_BAD_LENGTH :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_SEEK :
    rc = GRN_INVALID_SEEK;
    break;
  case ERROR_NOT_SUPPORTED :
    rc = GRN_OPERATION_NOT_SUPPORTED;
    break;
  case ERROR_NETWORK_ACCESS_DENIED :
    rc = GRN_OPERATION_NOT_PERMITTED;
    break;
  case ERROR_FILE_EXISTS :
    rc = GRN_FILE_EXISTS;
    break;
  case ERROR_INVALID_PARAMETER :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_BROKEN_PIPE :
    rc = GRN_BROKEN_PIPE;
    break;
  case ERROR_CALL_NOT_IMPLEMENTED :
    rc = GRN_FUNCTION_NOT_IMPLEMENTED;
    break;
  case ERROR_INVALID_NAME :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_BUSY_DRIVE :
  case ERROR_PATH_BUSY :
    rc = GRN_RESOURCE_BUSY;
    break;
  case ERROR_BAD_ARGUMENTS :
    rc = GRN_INVALID_ARGUMENT;
    break;
  case ERROR_BUSY :
    rc = GRN_RESOURCE_BUSY;
    break;
  case ERROR_ALREADY_EXISTS :
    rc = GRN_FILE_EXISTS;
    break;
  case ERROR_BAD_EXE_FORMAT :
    rc = GRN_EXEC_FORMAT_ERROR;
    break;
  case ERROR_NO_SYSTEM_RESOURCES :
    rc = GRN_RESOURCE_TEMPORARILY_UNAVAILABLE;
    break;
  default:
    rc = GRN_UNKNOWN_ERROR;
    break;
  }

  return rc;
}

# define LANG_ID_NEUTRAL()        MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL)
# define LANG_ID_USER_DEFAULT()   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT)
# define LANG_ID_SYSTEM_DEFAULT() MAKELANGID(LANG_NEUTRAL, SUBLANG_SYS_DEFAULT)

const char *
grn_current_error_message(void)
{
# define ERROR_MESSAGE_BUFFER_SIZE 4096
  int error_code = GetLastError();
  static WCHAR utf16_message[ERROR_MESSAGE_BUFFER_SIZE];
  DWORD written_utf16_chars;
  static char message[ERROR_MESSAGE_BUFFER_SIZE];

  written_utf16_chars = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM |
                                       FORMAT_MESSAGE_IGNORE_INSERTS,
                                       NULL,
                                       error_code,
                                       LANG_ID_USER_DEFAULT(),
                                       utf16_message,
                                       ERROR_MESSAGE_BUFFER_SIZE,
                                       NULL);
  if (written_utf16_chars >= 2) {
    if (utf16_message[written_utf16_chars - 1] == L'\n') {
      utf16_message[written_utf16_chars - 1] = L'\0';
      written_utf16_chars--;
    }
    if (utf16_message[written_utf16_chars - 1] == L'\r') {
      utf16_message[written_utf16_chars - 1] = L'\0';
      written_utf16_chars--;
    }
  }

  {
    UINT code_page;
    DWORD convert_flags = 0;
    int written_bytes;

    code_page = grn_windows_encoding_to_code_page(grn_get_default_encoding());
    written_bytes = WideCharToMultiByte(code_page,
                                        convert_flags,
                                        utf16_message,
                                        written_utf16_chars,
                                        message,
                                        ERROR_MESSAGE_BUFFER_SIZE,
                                        NULL,
                                        NULL);
  }

  return message;

# undef ERROR_MESSAGE_BUFFER_SIZE
}
#else
const char *
grn_current_error_message(void)
{
  return strerror(errno);
}
#endif

const char *
grn_strerror(int error_code)
{
#ifdef WIN32
# define MESSAGE_BUFFER_SIZE 1024
  static char message[MESSAGE_BUFFER_SIZE];
  strerror_s(message, MESSAGE_BUFFER_SIZE, error_code);
  return message;
# undef MESSAGE_BUFFER_SIZE
#else /* WIN32 */
  return strerror(error_code);
#endif /* WIN32 */
}

const char *
grn_rc_to_string(grn_rc rc)
{
  const char *message = "invalid grn_rc";

  switch (rc) {
  case GRN_SUCCESS :
    message = "success";
    break;
  case GRN_END_OF_DATA :
    message = "end of data";
    break;
  case GRN_UNKNOWN_ERROR :
    message = "unknown error";
    break;
  case GRN_OPERATION_NOT_PERMITTED :
    message = "operation not permitted";
    break;
  case GRN_NO_SUCH_FILE_OR_DIRECTORY :
    message = "no such file or directory";
    break;
  case GRN_NO_SUCH_PROCESS :
    message = "no such process";
    break;
  case GRN_INTERRUPTED_FUNCTION_CALL :
    message = "interrupted function call";
    break;
  case GRN_INPUT_OUTPUT_ERROR :
    message = "input output error";
    break;
  case GRN_NO_SUCH_DEVICE_OR_ADDRESS :
    message = "no such device or address";
    break;
  case GRN_ARG_LIST_TOO_LONG :
    message = "argument list is too long";
    break;
  case GRN_EXEC_FORMAT_ERROR :
    message = "exec format error";
    break;
  case GRN_BAD_FILE_DESCRIPTOR :
    message = "bad file descriptor";
    break;
  case GRN_NO_CHILD_PROCESSES :
    message = "no child processes";
    break;
  case GRN_RESOURCE_TEMPORARILY_UNAVAILABLE :
    message = "resource temporarily unavailable";
    break;
  case GRN_NOT_ENOUGH_SPACE :
    message = "not enough space";
    break;
  case GRN_PERMISSION_DENIED :
    message = "permission denied";
    break;
  case GRN_BAD_ADDRESS :
    message = "bad address";
    break;
  case GRN_RESOURCE_BUSY :
    message = "resource busy";
    break;
  case GRN_FILE_EXISTS :
    message = "file exists";
    break;
  case GRN_IMPROPER_LINK :
    message = "improper link";
    break;
  case GRN_NO_SUCH_DEVICE :
    message = "no such device";
    break;
  case GRN_NOT_A_DIRECTORY :
    message = "not a directory";
    break;
  case GRN_IS_A_DIRECTORY :
    message = "is a directory";
    break;
  case GRN_INVALID_ARGUMENT :
    message = "invalid argument";
    break;
  case GRN_TOO_MANY_OPEN_FILES_IN_SYSTEM :
    message = "too many open files in system";
    break;
  case GRN_TOO_MANY_OPEN_FILES :
    message = "too many open files";
    break;
  case GRN_INAPPROPRIATE_I_O_CONTROL_OPERATION :
    message = "inappropriate I/O control operation";
    break;
  case GRN_FILE_TOO_LARGE :
    message = "file too large";
    break;
  case GRN_NO_SPACE_LEFT_ON_DEVICE :
    message = "no space left on device";
    break;
  case GRN_INVALID_SEEK :
    message = "invalid seek";
    break;
  case GRN_READ_ONLY_FILE_SYSTEM :
    message = "read only file system";
    break;
  case GRN_TOO_MANY_LINKS :
    message = "too many links";
    break;
  case GRN_BROKEN_PIPE :
    message = "broken pipe";
    break;
  case GRN_DOMAIN_ERROR :
    message = "domain error";
    break;
  case GRN_RESULT_TOO_LARGE :
    message = "result too large";
    break;
  case GRN_RESOURCE_DEADLOCK_AVOIDED :
    message = "resource deadlock avoided";
    break;
  case GRN_NO_MEMORY_AVAILABLE :
    message = "no memory available";
    break;
  case GRN_FILENAME_TOO_LONG :
    message = "filename too long";
    break;
  case GRN_NO_LOCKS_AVAILABLE :
    message = "no locks available";
    break;
  case GRN_FUNCTION_NOT_IMPLEMENTED :
    message = "function not implemented";
    break;
  case GRN_DIRECTORY_NOT_EMPTY :
    message = "directory not empty";
    break;
  case GRN_ILLEGAL_BYTE_SEQUENCE :
    message = "illegal byte sequence";
    break;
  case GRN_SOCKET_NOT_INITIALIZED :
    message = "socket not initialized";
    break;
  case GRN_OPERATION_WOULD_BLOCK :
    message = "operation would block";
    break;
  case GRN_ADDRESS_IS_NOT_AVAILABLE :
    message = "address is not available";
    break;
  case GRN_NETWORK_IS_DOWN :
    message = "network is down";
    break;
  case GRN_NO_BUFFER :
    message = "no buffer";
    break;
  case GRN_SOCKET_IS_ALREADY_CONNECTED :
    message = "socket is already connected";
    break;
  case GRN_SOCKET_IS_NOT_CONNECTED :
    message = "socket is not connected";
    break;
  case GRN_SOCKET_IS_ALREADY_SHUTDOWNED :
    message = "socket is already shutdowned";
    break;
  case GRN_OPERATION_TIMEOUT :
    message = "operation timeout";
    break;
  case GRN_CONNECTION_REFUSED :
    message = "connection refused";
    break;
  case GRN_RANGE_ERROR :
    message = "range error";
    break;
  case GRN_TOKENIZER_ERROR :
    message = "tokenizer error";
    break;
  case GRN_FILE_CORRUPT :
    message = "file corrupt";
    break;
  case GRN_INVALID_FORMAT :
    message = "invalid format";
    break;
  case GRN_OBJECT_CORRUPT :
    message = "object corrupt";
    break;
  case GRN_TOO_MANY_SYMBOLIC_LINKS :
    message = "too many symbolic links";
    break;
  case GRN_NOT_SOCKET :
    message = "not socket";
    break;
  case GRN_OPERATION_NOT_SUPPORTED :
    message = "operation not supported";
    break;
  case GRN_ADDRESS_IS_IN_USE :
    message = "address is in use";
    break;
  case GRN_ZLIB_ERROR :
    message = "zlib error";
    break;
  case GRN_LZ4_ERROR :
    message = "LZ4 error";
    break;
  case GRN_STACK_OVER_FLOW :
    message = "stack over flow";
    break;
  case GRN_SYNTAX_ERROR :
    message = "syntax error";
    break;
  case GRN_RETRY_MAX :
    message = "retry max";
    break;
  case GRN_INCOMPATIBLE_FILE_FORMAT :
    message = "incompatible file format";
    break;
  case GRN_UPDATE_NOT_ALLOWED :
    message = "update not allowed";
    break;
  case GRN_TOO_SMALL_OFFSET :
    message = "too small offset";
    break;
  case GRN_TOO_LARGE_OFFSET :
    message = "too large offset";
    break;
  case GRN_TOO_SMALL_LIMIT :
    message = "too small limit";
    break;
  case GRN_CAS_ERROR :
    message = "cas error";
    break;
  case GRN_UNSUPPORTED_COMMAND_VERSION :
    message = "unsupported command version";
    break;
  case GRN_NORMALIZER_ERROR :
    message = "normalizer error";
    break;
  case GRN_TOKEN_FILTER_ERROR :
    message = "token filter error";
    break;
  case GRN_COMMAND_ERROR :
    message = "command error";
    break;
  case GRN_PLUGIN_ERROR :
    message = "plugin error";
    break;
  case GRN_SCORER_ERROR :
    message = "scorer error";
    break;
  case GRN_CANCEL :
    message = "cancel";
    break;
  case GRN_WINDOW_FUNCTION_ERROR :
    message = "window function error";
    break;
  case GRN_ZSTD_ERROR :
    message = "Zstandard error";
    break;
  }

  return message;
}
