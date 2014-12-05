/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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

#include "../ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>

#include "../mrb.h"
#include "mrb_error.h"

void
grn_mrb_error_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *error_class;

  error_class = mrb_define_class_under(mrb, module, "Error",
                                       mrb->eStandardError_class);

  mrb_define_class_under(mrb, module, "EndOfData",
                         error_class);
  mrb_define_class_under(mrb, module, "UnknownError",
                         error_class);
  mrb_define_class_under(mrb, module, "OperationNotPermitted",
                         error_class);
  mrb_define_class_under(mrb, module, "NoSuchFileOrDirectory",
                         error_class);
  mrb_define_class_under(mrb, module, "NoSuchProcess",
                         error_class);
  mrb_define_class_under(mrb, module, "InterruptedFunctionCall",
                         error_class);
  mrb_define_class_under(mrb, module, "InputOutputError",
                         error_class);
  mrb_define_class_under(mrb, module, "NoSuchDeviceOrAddress",
                         error_class);
  mrb_define_class_under(mrb, module, "ArgListTooLong",
                         error_class);
  mrb_define_class_under(mrb, module, "ExecFormatError",
                         error_class);
  mrb_define_class_under(mrb, module, "BadFileDescriptor",
                         error_class);
  mrb_define_class_under(mrb, module, "NoChildProcesses",
                         error_class);
  mrb_define_class_under(mrb, module, "ResourceTemporarilyUnavailable",
                         error_class);
  mrb_define_class_under(mrb, module, "NotEnoughSpace",
                         error_class);
  mrb_define_class_under(mrb, module, "PermissionDenied",
                         error_class);
  mrb_define_class_under(mrb, module, "BadAddress",
                         error_class);
  mrb_define_class_under(mrb, module, "ResourceBusy",
                         error_class);
  mrb_define_class_under(mrb, module, "FileExists",
                         error_class);
  mrb_define_class_under(mrb, module, "ImproperLink",
                         error_class);
  mrb_define_class_under(mrb, module, "NoSuchDevice",
                         error_class);
  mrb_define_class_under(mrb, module, "NotDirectory",
                         error_class);
  mrb_define_class_under(mrb, module, "IsDirectory",
                         error_class);
  mrb_define_class_under(mrb, module, "InvalidArgument",
                         error_class);
  mrb_define_class_under(mrb, module, "TooManyOpenFilesInSystem",
                         error_class);
  mrb_define_class_under(mrb, module, "TooManyOpenFiles",
                         error_class);
  mrb_define_class_under(mrb, module, "InappropriateIOControlOperation",
                         error_class);
  mrb_define_class_under(mrb, module, "FileTooLarge",
                         error_class);
  mrb_define_class_under(mrb, module, "NoSpaceLeftOnDevice",
                         error_class);
  mrb_define_class_under(mrb, module, "InvalidSeek",
                         error_class);
  mrb_define_class_under(mrb, module, "ReadOnlyFileSystem",
                         error_class);
  mrb_define_class_under(mrb, module, "TooManyLinks",
                         error_class);
  mrb_define_class_under(mrb, module, "BrokenPipe",
                         error_class);
  mrb_define_class_under(mrb, module, "DomainError",
                         error_class);
  mrb_define_class_under(mrb, module, "ResultTooLarge",
                         error_class);
  mrb_define_class_under(mrb, module, "ResourceDeadlockAvoided",
                         error_class);
  mrb_define_class_under(mrb, module, "NoMemoryAvailable",
                         error_class);
  mrb_define_class_under(mrb, module, "FilenameTooLong",
                         error_class);
  mrb_define_class_under(mrb, module, "NoLocksAvailable",
                         error_class);
  mrb_define_class_under(mrb, module, "FunctionNotImplemented",
                         error_class);
  mrb_define_class_under(mrb, module, "DirectoryNotEmpty",
                         error_class);
  mrb_define_class_under(mrb, module, "IllegalByteSequence",
                         error_class);
  mrb_define_class_under(mrb, module, "SocketNotInitialized",
                         error_class);
  mrb_define_class_under(mrb, module, "OperationWouldBlock",
                         error_class);
  mrb_define_class_under(mrb, module, "AddressIsNotAvailable",
                         error_class);
  mrb_define_class_under(mrb, module, "NetworkIsDown",
                         error_class);
  mrb_define_class_under(mrb, module, "NoBuffer",
                         error_class);
  mrb_define_class_under(mrb, module, "SocketIsAlreadyConnected",
                         error_class);
  mrb_define_class_under(mrb, module, "SocketIsNotConnected",
                         error_class);
  mrb_define_class_under(mrb, module, "SocketIsAlreadyShutdowned",
                         error_class);
  mrb_define_class_under(mrb, module, "OperationTimeout",
                         error_class);
  mrb_define_class_under(mrb, module, "ConnectionRefused",
                         error_class);
  mrb_define_class_under(mrb, module, "RangeError",
                         error_class);
  mrb_define_class_under(mrb, module, "TokenizerError",
                         error_class);
  mrb_define_class_under(mrb, module, "FileCorrupt",
                         error_class);
  mrb_define_class_under(mrb, module, "InvalidFormat",
                         error_class);
  mrb_define_class_under(mrb, module, "ObjectCorrupt",
                         error_class);
  mrb_define_class_under(mrb, module, "TooManySymbolicLinks",
                         error_class);
  mrb_define_class_under(mrb, module, "NotSocket",
                         error_class);
  mrb_define_class_under(mrb, module, "OperationNotSupported",
                         error_class);
  mrb_define_class_under(mrb, module, "AddressIsInUse",
                         error_class);
  mrb_define_class_under(mrb, module, "ZlibError",
                         error_class);
  mrb_define_class_under(mrb, module, "LzoError",
                         error_class);
  mrb_define_class_under(mrb, module, "StackOverFlow",
                         error_class);
  mrb_define_class_under(mrb, module, "SyntaxError",
                         error_class);
  mrb_define_class_under(mrb, module, "RetryMax",
                         error_class);
  mrb_define_class_under(mrb, module, "IncompatibleFileFormat",
                         error_class);
  mrb_define_class_under(mrb, module, "UpdateNotAllowed",
                         error_class);
  mrb_define_class_under(mrb, module, "TooSmallOffset",
                         error_class);
  mrb_define_class_under(mrb, module, "TooLargeOffset",
                         error_class);
  mrb_define_class_under(mrb, module, "TooSmallLimit",
                         error_class);
  mrb_define_class_under(mrb, module, "CASError",
                         error_class);
  mrb_define_class_under(mrb, module, "UnsupportedCommandVersion",
                         error_class);
  mrb_define_class_under(mrb, module, "NormalizerError",
                         error_class);
}
#endif
