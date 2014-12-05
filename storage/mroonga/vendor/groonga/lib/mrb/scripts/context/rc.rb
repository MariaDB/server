module Groonga
  class Context
    class RC
      @@names = {}

      class << self
        def find(name)
          @@names[name]
        end
      end

      attr_reader :name
      def initialize(name, code)
        @@names[name] = self
        @name = name
        @code = code
      end

      def to_i
        @code
      end

      SUCCESS                             = new(:success, 0)
      END_OF_DATA                         = new(:end_of_data, 1)
      UNKNOWN_ERROR                       = new(:unknown_error, -1)
      OPERATION_NOT_PERMITTED             = new(:operation_not_permitted, -2)
      NO_SUCH_FILE_OR_DIRECTORY           = new(:no_such_file_or_directory, -3)
      NO_SUCH_PROCESS                     = new(:no_such_process, -4)
      INTERRUPTED_FUNCTION_CALL           = new(:interrupted_function_call, -5)
      INPUT_OUTPUT_ERROR                  = new(:input_output_error, -6)
      NO_SUCH_DEVICE_OR_ADDRESS           = new(:no_such_device_or_address, -7)
      ARG_LIST_TOO_LONG                   = new(:arg_list_too_long, -8)
      EXEC_FORMAT_ERROR                   = new(:exec_format_error, -9)
      BAD_FILE_DESCRIPTOR                 = new(:bad_file_descriptor, -10)
      NO_CHILD_PROCESSES                  = new(:no_child_processes, -11)
      RESOURCE_TEMPORARILY_UNAVAILABLE    = new(:resource_temporarily_unavailable, -12)
      NOT_ENOUGH_SPACE                    = new(:not_enough_space, -13)
      PERMISSION_DENIED                   = new(:permission_denied, -14)
      BAD_ADDRESS                         = new(:bad_address, -15)
      RESOURCE_BUSY                       = new(:resource_busy, -16)
      FILE_EXISTS                         = new(:file_exists, -17)
      IMPROPER_LINK                       = new(:improper_link, -18)
      NO_SUCH_DEVICE                      = new(:no_such_device, -19)
      NOT_A_DIRECTORY                     = new(:not_a_directory, -20)
      IS_A_DIRECTORY                      = new(:is_a_directory, -21)
      INVALID_ARGUMENT                    = new(:invalid_argument, -22)
      TOO_MANY_OPEN_FILES_IN_SYSTEM       = new(:too_many_open_files_in_system, -23)
      TOO_MANY_OPEN_FILES                 = new(:too_many_open_files, -24)
      INAPPROPRIATE_IO_CONTROL_OPERATION  = new(:inappropriate_io_control_operation, -25)
      FILE_TOO_LARGE                      = new(:file_too_large, -26)
      NO_SPACE_LEFT_ON_DEVICE             = new(:no_space_left_on_device, -27)
      INVALID_SEEK                        = new(:invalid_seek, -28)
      READ_ONLY_FILE_SYSTEM               = new(:read_only_file_system, -29)
      TOO_MANY_LINKS                      = new(:too_many_links, -30)
      BROKEN_PIPE                         = new(:broken_pipe, -31)
      DOMAIN_ERROR                        = new(:domain_error, -32)
      RESULT_TOO_LARGE                    = new(:result_too_large, -33)
      RESOURCE_DEADLOCK_AVOIDED           = new(:resource_deadlock_avoided, -34)
      NO_MEMORY_AVAILABLE                 = new(:no_memory_available, -35)
      FILENAME_TOO_LONG                   = new(:filename_too_long, -36)
      NO_LOCKS_AVAILABLE                  = new(:no_locks_available, -37)
      FUNCTION_NOT_IMPLEMENTED            = new(:function_not_implemented, -38)
      DIRECTORY_NOT_EMPTY                 = new(:directory_not_empty, -39)
      ILLEGAL_BYTE_SEQUENCE               = new(:illegal_byte_sequence, -40)
      SOCKET_NOT_INITIALIZED              = new(:socket_not_initialized, -41)
      OPERATION_WOULD_BLOCK               = new(:operation_would_block, -42)
      ADDRESS_IS_NOT_AVAILABLE            = new(:address_is_not_available, -43)
      NETWORK_IS_DOWN                     = new(:network_is_down, -44)
      NO_BUFFER                           = new(:no_buffer, -45)
      SOCKET_IS_ALREADY_CONNECTED         = new(:socket_is_already_connected, -46)
      SOCKET_IS_NOT_CONNECTED             = new(:socket_is_not_connected, -47)
      SOCKET_IS_ALREADY_SHUTDOWNED        = new(:socket_is_already_shutdowned, -48)
      OPERATION_TIMEOUT                   = new(:operation_timeout, -49)
      CONNECTION_REFUSED                  = new(:connection_refused, -50)
      RANGE_ERROR                         = new(:range_error, -51)
      TOKENIZER_ERROR                     = new(:tokenizer_error, -52)
      FILE_CORRUPT                        = new(:file_corrupt, -53)
      INVALID_FORMAT                      = new(:invalid_format, -54)
      OBJECT_CORRUPT                      = new(:object_corrupt, -55)
      TOO_MANY_SYMBOLIC_LINKS             = new(:too_many_symbolic_links, -56)
      NOT_SOCKET                          = new(:not_socket, -57)
      OPERATION_NOT_SUPPORTED             = new(:operation_not_supported, -58)
      ADDRESS_IS_IN_USE                   = new(:address_is_in_use, -59)
      ZLIB_ERROR                          = new(:zlib_error, -60)
      LZO_ERROR                           = new(:lzo_error, -61)
      STACK_OVER_FLOW                     = new(:stack_over_flow, -62)
      SYNTAX_ERROR                        = new(:syntax_error, -63)
      RETRY_MAX                           = new(:retry_max, -64)
      INCOMPATIBLE_FILE_FORMAT            = new(:incompatible_file_format, -65)
      UPDATE_NOT_ALLOWED                  = new(:update_not_allowed, -66)
      TOO_SMALL_OFFSET                    = new(:too_small_offset, -67)
      TOO_LARGE_OFFSET                    = new(:too_large_offset, -68)
      TOO_SMALL_LIMIT                     = new(:too_small_limit, -69)
      CAS_ERROR                           = new(:cas_error, -70)
      UNSUPPORTED_COMMAND_VERSION         = new(:unsupported_command_version, -71)
      NORMALIZER_ERROR                    = new(:normalizer_error, -72)
    end
  end
end
