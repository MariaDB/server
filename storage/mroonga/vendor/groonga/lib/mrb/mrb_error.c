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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>

#include "../grn_mrb.h"
#include "mrb_error.h"

void
grn_mrb_error_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *error_class;
  struct RClass *groonga_error_class;

  error_class = mrb_define_class_under(mrb, module, "Error",
                                       mrb->eStandardError_class);
  groonga_error_class = mrb_define_class_under(mrb, module, "GroongaError",
                                               error_class);

  mrb_define_class_under(mrb, module, "EndOfData",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "UnknownError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "OperationNotPermitted",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoSuchFileOrDirectory",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoSuchProcess",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "InterruptedFunctionCall",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "InputOutputError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoSuchDeviceOrAddress",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ArgListTooLong",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ExecFormatError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "BadFileDescriptor",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoChildProcesses",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ResourceTemporarilyUnavailable",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NotEnoughSpace",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "PermissionDenied",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "BadAddress",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ResourceBusy",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "FileExists",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ImproperLink",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoSuchDevice",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NotDirectory",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "IsDirectory",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "InvalidArgument",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooManyOpenFilesInSystem",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooManyOpenFiles",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "InappropriateIOControlOperation",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "FileTooLarge",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoSpaceLeftOnDevice",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "InvalidSeek",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ReadOnlyFileSystem",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooManyLinks",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "BrokenPipe",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "DomainError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ResultTooLarge",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ResourceDeadlockAvoided",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoMemoryAvailable",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "FilenameTooLong",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoLocksAvailable",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "FunctionNotImplemented",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "DirectoryNotEmpty",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "IllegalByteSequence",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "SocketNotInitialized",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "OperationWouldBlock",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "AddressIsNotAvailable",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NetworkIsDown",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NoBuffer",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "SocketIsAlreadyConnected",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "SocketIsNotConnected",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "SocketIsAlreadyShutdowned",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "OperationTimeout",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ConnectionRefused",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "RangeError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TokenizerError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "FileCorrupt",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "InvalidFormat",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ObjectCorrupt",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooManySymbolicLinks",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NotSocket",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "OperationNotSupported",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "AddressIsInUse",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ZlibError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "LZ4Error",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "StackOverFlow",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "SyntaxError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "RetryMax",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "IncompatibleFileFormat",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "UpdateNotAllowed",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooSmallOffset",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooLargeOffset",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TooSmallLimit",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "CASError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "UnsupportedCommandVersion",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "NormalizerError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "TokenFilterError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "CommandError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "PluginError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ScorerError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "Cancel",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "WindowFunctionError",
                         groonga_error_class);
  mrb_define_class_under(mrb, module, "ZstdError",
                         groonga_error_class);
}
#endif
