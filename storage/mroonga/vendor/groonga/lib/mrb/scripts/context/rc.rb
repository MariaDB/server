module Groonga
  class Context
    class RC
      @@codes = {}
      @@names = {}

      class << self
        def find(name_or_code)
          if name_or_code.is_a?(Symbol)
            @@names[name_or_code] || UNKNOWN_ERROR
          else
            @@codes[name_or_code] || UNKNOWN_ERROR
          end
        end

        def register(name, code, error_class)
          rc = new(name, code, error_class)
          @@codes[code] = rc
          @@names[name] = rc
          error_class.rc = rc if error_class
          rc
        end
      end

      attr_reader :name
      attr_reader :error_class
      def initialize(name, code, error_class)
        @name = name
        @code = code
        @error_class = error_class
      end

      def to_i
        @code
      end

      SUCCESS =
        register(:success, 0, nil)
      END_OF_DATA =
        register(:end_of_data, 1, EndOfData)
      UNKNOWN_ERROR =
        register(:unknown_error, -1, UnknownError)
      OPERATION_NOT_PERMITTED =
        register(:operation_not_permitted, -2, OperationNotPermitted)
      NO_SUCH_FILE_OR_DIRECTORY =
        register(:no_such_file_or_directory, -3, NoSuchFileOrDirectory)
      NO_SUCH_PROCESS =
        register(:no_such_process, -4, NoSuchProcess)
      INTERRUPTED_FUNCTION_CALL =
        register(:interrupted_function_call, -5, InterruptedFunctionCall)
      INPUT_OUTPUT_ERROR =
        register(:input_output_error, -6, InputOutputError)
      NO_SUCH_DEVICE_OR_ADDRESS =
        register(:no_such_device_or_address, -7, NoSuchDeviceOrAddress)
      ARG_LIST_TOO_LONG =
        register(:arg_list_too_long, -8, ArgListTooLong)
      EXEC_FORMAT_ERROR =
        register(:exec_format_error, -9, ExecFormatError)
      BAD_FILE_DESCRIPTOR =
        register(:bad_file_descriptor, -10, BadFileDescriptor)
      NO_CHILD_PROCESSES =
        register(:no_child_processes, -11, NoChildProcesses)
      RESOURCE_TEMPORARILY_UNAVAILABLE =
        register(:resource_temporarily_unavailable, -12,
                 ResourceTemporarilyUnavailable)
      NOT_ENOUGH_SPACE =
        register(:not_enough_space, -13, NotEnoughSpace)
      PERMISSION_DENIED =
        register(:permission_denied, -14, PermissionDenied)
      BAD_ADDRESS =
        register(:bad_address, -15, BadAddress)
      RESOURCE_BUSY =
        register(:resource_busy, -16, ResourceBusy)
      FILE_EXISTS =
        register(:file_exists, -17, FileExists)
      IMPROPER_LINK =
        register(:improper_link, -18, ImproperLink)
      NO_SUCH_DEVICE =
        register(:no_such_device, -19, NoSuchDevice)
      NOT_DIRECTORY =
        register(:not_directory, -20, NotDirectory)
      IS_DIRECTORY =
        register(:is_directory, -21, IsDirectory)
      INVALID_ARGUMENT =
        register(:invalid_argument, -22, InvalidArgument)
      TOO_MANY_OPEN_FILES_IN_SYSTEM =
        register(:too_many_open_files_in_system, -23, TooManyOpenFilesInSystem)
      TOO_MANY_OPEN_FILES =
        register(:too_many_open_files, -24, TooManyOpenFiles)
      INAPPROPRIATE_IO_CONTROL_OPERATION =
        register(:inappropriate_io_control_operation, -25,
                 InappropriateIOControlOperation)
      FILE_TOO_LARGE =
        register(:file_too_large, -26, FileTooLarge)
      NO_SPACE_LEFT_ON_DEVICE =
        register(:no_space_left_on_device, -27, NoSpaceLeftOnDevice)
      INVALID_SEEK =
        register(:invalid_seek, -28, InvalidSeek)
      READ_ONLY_FILE_SYSTEM =
        register(:read_only_file_system, -29, ReadOnlyFileSystem)
      TOO_MANY_LINKS =
        register(:too_many_links, -30, TooManyLinks)
      BROKEN_PIPE =
        register(:broken_pipe, -31, BrokenPipe)
      DOMAIN_ERROR =
        register(:domain_error, -32, DomainError)
      RESULT_TOO_LARGE =
        register(:result_too_large, -33, ResultTooLarge)
      RESOURCE_DEADLOCK_AVOIDED =
        register(:resource_deadlock_avoided, -34, ResourceDeadlockAvoided)
      NO_MEMORY_AVAILABLE =
        register(:no_memory_available, -35, NoMemoryAvailable)
      FILENAME_TOO_LONG =
        register(:filename_too_long, -36, FilenameTooLong)
      NO_LOCKS_AVAILABLE =
        register(:no_locks_available, -37, NoLocksAvailable)
      FUNCTION_NOT_IMPLEMENTED =
        register(:function_not_implemented, -38, FunctionNotImplemented)
      DIRECTORY_NOT_EMPTY =
        register(:directory_not_empty, -39, DirectoryNotEmpty)
      ILLEGAL_BYTE_SEQUENCE =
        register(:illegal_byte_sequence, -40, IllegalByteSequence)
      SOCKET_NOT_INITIALIZED =
        register(:socket_not_initialized, -41, SocketNotInitialized)
      OPERATION_WOULD_BLOCK =
        register(:operation_would_block, -42, OperationWouldBlock)
      ADDRESS_IS_NOT_AVAILABLE =
        register(:address_is_not_available, -43, AddressIsNotAvailable)
      NETWORK_IS_DOWN =
        register(:network_is_down, -44, NetworkIsDown)
      NO_BUFFER =
        register(:no_buffer, -45, NoBuffer)
      SOCKET_IS_ALREADY_CONNECTED =
        register(:socket_is_already_connected, -46, SocketIsAlreadyConnected)
      SOCKET_IS_NOT_CONNECTED =
        register(:socket_is_not_connected, -47, SocketIsNotConnected)
      SOCKET_IS_ALREADY_SHUTDOWNED =
        register(:socket_is_already_shutdowned, -48, SocketIsAlreadyShutdowned)
      OPERATION_TIMEOUT =
        register(:operation_timeout, -49, OperationTimeout)
      CONNECTION_REFUSED =
        register(:connection_refused, -50, ConnectionRefused)
      RANGE_ERROR =
        register(:range_error, -51, RangeError)
      TOKENIZER_ERROR =
        register(:tokenizer_error, -52, TokenizerError)
      FILE_CORRUPT =
        register(:file_corrupt, -53, FileCorrupt)
      INVALID_FORMAT =
        register(:invalid_format, -54, InvalidFormat)
      OBJECT_CORRUPT =
        register(:object_corrupt, -55, ObjectCorrupt)
      TOO_MANY_SYMBOLIC_LINKS =
        register(:too_many_symbolic_links, -56, TooManySymbolicLinks)
      NOT_SOCKET =
        register(:not_socket, -57, NotSocket)
      OPERATION_NOT_SUPPORTED =
        register(:operation_not_supported, -58, OperationNotSupported)
      ADDRESS_IS_IN_USE =
        register(:address_is_in_use, -59, AddressIsInUse)
      ZLIB_ERROR =
        register(:zlib_error, -60, ZlibError)
      LZ4_ERROR =
        register(:lz4_error, -61, LZ4Error)
      STACK_OVER_FLOW =
        register(:stack_over_flow, -62, StackOverFlow)
      SYNTAX_ERROR =
        register(:syntax_error, -63, SyntaxError)
      RETRY_MAX =
        register(:retry_max, -64, RetryMax)
      INCOMPATIBLE_FILE_FORMAT =
        register(:incompatible_file_format, -65, IncompatibleFileFormat)
      UPDATE_NOT_ALLOWED =
        register(:update_not_allowed, -66, UpdateNotAllowed)
      TOO_SMALL_OFFSET =
        register(:too_small_offset, -67, TooSmallOffset)
      TOO_LARGE_OFFSET =
        register(:too_large_offset, -68, TooLargeOffset)
      TOO_SMALL_LIMIT =
        register(:too_small_limit, -69, TooSmallLimit)
      CAS_ERROR =
        register(:cas_error, -70, CASError)
      UNSUPPORTED_COMMAND_VERSION =
        register(:unsupported_command_version, -71, UnsupportedCommandVersion)
      NORMALIZER_ERROR =
        register(:normalizer_error, -72, NormalizerError)
      TOKEN_FILTER_ERROR =
        register(:token_filter_error, -73, TokenFilterError)
      COMMAND_ERROR =
        register(:command_error, -74, CommandError)
      PLUGIN_ERROR =
        register(:plugin_error, -75, PluginError)
      SCORER_ERROR =
        register(:scorer_error, -76, ScorerError)
      CANCEL =
        register(:cancel, -77, Cancel)
      WINDOW_FUNCTION_ERROR =
        register(:window_function_error, -78, WindowFunctionError)
      ZSTD_ERROR =
        register(:zstd_error, -79, ZstdError)

      GroongaError.rc = UNKNOWN_ERROR
    end
  end
end
