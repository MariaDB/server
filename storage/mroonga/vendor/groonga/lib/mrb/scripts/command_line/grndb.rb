module Groonga
  module CommandLine
    class Grndb
      def initialize(argv)
        @program_path, *@arguments = argv
        @succeeded = true
        @database_path = nil
      end

      def run
        command_line_parser = create_command_line_parser
        options = nil
        begin
          options = command_line_parser.parse(@arguments)
        rescue Slop::Error => error
          $stderr.puts(error.message)
          $stderr.puts
          $stderr.puts(command_line_parser.help_message)
          return false
        end
        @succeeded
      end

      private
      def create_command_line_parser
        program_name = File.basename(@program_path)
        parser = CommandLineParser.new(program_name)

        parser.add_command("check") do |command|
          command.description = "Check database"

          options = command.options
          options.banner += " DB_PATH"
          options.string("--target", "Check only the target object.")

          command.add_action do |options|
            open_database(command, options) do |database, rest_arguments|
              check(database, options, rest_arguments)
            end
          end
        end

        parser.add_command("recover") do |command|
          command.description = "Recover database"

          options = command.options
          options.banner += " DB_PATH"
          options.boolean("--force-truncate", "Force to truncate corrupted objects.")

          command.add_action do |options|
            open_database(command, options) do |database, rest_arguments|
              recover(database, options, rest_arguments)
            end
          end
        end

        parser
      end

      def open_database(command, options)
        arguments = options.arguments
        if arguments.empty?
          $stderr.puts("Database path is missing")
          $stderr.puts
          $stderr.puts(command.help_message)
          @succeesed = false
          return
        end

        database = nil
        @database_path, *rest_arguments = arguments
        begin
          database = Database.open(@database_path)
        rescue Error => error
          $stderr.puts("Failed to open database: <#{@database_path}>")
          $stderr.puts(error.message)
          @succeeded = false
          return
        end

        begin
          yield(database, rest_arguments)
        ensure
          database.close
        end
      end

      def failed(*messages)
        messages.each do |message|
          $stderr.puts(message)
        end
        @succeeded = false
      end

      def recover(database, options, arguments)
        recoverer = Recoverer.new
        recoverer.database = database
        recoverer.force_truncate = options[:force_truncate]
        begin
          recoverer.recover
        rescue Error => error
          failed("Failed to recover database: <#{@database_path}>",
                 error.message)
        end
      end

      def check(database, options, arguments)
        checker = Checker.new
        checker.program_path = @program_path
        checker.database_path = @database_path
        checker.database = database
        checker.on_failure = lambda do |message|
          failed(message)
        end

        checker.check_database

        target_name = options[:target]
        if target_name
          checker.check_one(target_name)
        else
          checker.check_all
        end
      end

      class Checker
        attr_writer :program_path
        attr_writer :database_path
        attr_writer :database
        attr_writer :on_failure

        def initialize
          @context = Context.instance
          @checked = {}
        end

        def check_database
          check_database_orphan_inspect
          check_database_locked
          check_database_corrupt
          check_database_dirty
        end

        def check_one(target_name)
          target = @context[target_name]
          if target.nil?
            exist_p = open_database_cursor do |cursor|
              cursor.any? do
                cursor.key == target_name
              end
            end
            if exist_p
              failed_to_open(target_name)
            else
              message = "[#{target_name}] Not exist."
              failed(message)
            end
            return
          end

          check_object_recursive(target)
        end

        def check_all
          open_database_cursor do |cursor|
            cursor.each do |id|
              next if ID.builtin?(id)
              next if builtin_object_name?(cursor.key)
              next if @context[id]
              failed_to_open(cursor.key)
            end
          end

          @database.each do |object|
            check_object(object)
          end
        end

        private
        def check_database_orphan_inspect
          open_database_cursor do |cursor|
            cursor.each do |id|
              if cursor.key == "inspect" and @context[id].nil?
                message =
                  "Database has orphan 'inspect' object. " +
                  "Remove it by '#{@program_path} recover #{@database_path}'."
                failed(message)
                break
              end
            end
          end
        end

        def check_database_locked
          return unless @database.locked?

          message =
            "Database is locked. " +
            "It may be broken. " +
            "Re-create the database."
          failed(message)
        end

        def check_database_corrupt
          return unless @database.corrupt?

          message =
            "Database is corrupt. " +
            "Re-create the database."
          failed(message)
        end

        def check_database_dirty
          return unless @database.dirty?

          last_modified = @database.last_modified
          if File.stat(@database.path).mtime > last_modified
            return
          end

          open_database_cursor do |cursor|
            cursor.each do |id|
              next if ID.builtin?(id)
              path = "%s.%07x" % [@database.path, id]
              next unless File.exist?(path)
              return if File.stat(path).mtime > last_modified
            end
          end

          message =
            "Database wasn't closed successfully. " +
            "It may be broken. " +
            "Re-create the database."
          failed(message)
        end

        def check_object(object)
          return if @checked.key?(object.id)
          @checked[object.id] = true

          check_object_locked(object)
          check_object_corrupt(object)
        end

        def check_object_locked(object)
          case object
          when IndexColumn
            return unless object.locked?
            message =
              "[#{object.name}] Index column is locked. " +
              "It may be broken. " +
              "Re-create index by '#{@program_path} recover #{@database_path}'."
            failed(message)
          when Column
            return unless object.locked?
            name = object.name
            message =
              "[#{name}] Data column is locked. " +
              "It may be broken. " +
              "(1) Truncate the column (truncate #{name}) or " +
              "clear lock of the column (lock_clear #{name}) " +
              "and (2) load data again."
            failed(message)
          when Table
            return unless object.locked?
            name = object.name
            message =
              "[#{name}] Table is locked. " +
              "It may be broken. " +
              "(1) Truncate the table (truncate #{name}) or " +
              "clear lock of the table (lock_clear #{name}) " +
              "and (2) load data again."
            failed(message)
          end
        end

        def check_object_corrupt(object)
          case object
          when IndexColumn
            return unless object.corrupt?
            message =
              "[#{object.name}] Index column is corrupt. " +
              "Re-create index by '#{@program_path} recover #{@database_path}'."
            failed(message)
          when Column
            return unless object.corrupt?
            name = object.name
            message =
              "[#{name}] Data column is corrupt. " +
              "(1) Truncate the column (truncate #{name} or " +
              "'#{@program_path} recover --force-truncate #{@database_path}') " +
              "and (2) load data again."
            failed(message)
          when Table
            return unless object.corrupt?
            name = object.name
            message =
              "[#{name}] Table is corrupt. " +
              "(1) Truncate the table (truncate #{name} or " +
              "'#{@program_path} recover --force-truncate #{@database_path}') " +
              "and (2) load data again."
            failed(message)
          end
        end

        def check_object_recursive(target)
          return if @checked.key?(target.id)

          check_object(target)
          case target
          when Table
            unless target.is_a?(Groonga::Array)
              domain_id = target.domain_id
              domain = @context[domain_id]
              if domain.nil?
                record = Record.new(@database, domain_id)
                failed_to_open(record.key)
              elsif domain.is_a?(Table)
                check_object_recursive(domain)
              end
            end

            target.column_ids.each do |column_id|
              column = @context[column_id]
              if column.nil?
                record = Record.new(@database, column_id)
                failed_to_open(record.key)
              else
                check_object_recursive(column)
              end
            end
          when FixedSizeColumn, VariableSizeColumn
            range_id = target.range_id
            range = @context[range_id]
            if range.nil?
              record = Record.new(@database, range_id)
              failed_to_open(record.key)
            elsif range.is_a?(Table)
              check_object_recursive(range)
            end

            lexicon_ids = []
            target.indexes.each do |index_info|
              index = index_info.index
              lexicon_ids << index.domain_id
              check_object(index)
            end
            lexicon_ids.uniq.each do |lexicon_id|
              lexicon = @context[lexicon_id]
              if lexicon.nil?
                record = Record.new(@database, lexicon_id)
                failed_to_open(record.key)
              else
                check_object(lexicon)
              end
            end
          when IndexColumn
            range_id = target.range_id
            range = @context[range_id]
            if range.nil?
              record = Record.new(@database, range_id)
              failed_to_open(record.key)
              return
            end
            check_object(range)

            target.source_ids.each do |source_id|
              source = @context[source_id]
              if source.nil?
                record = Record.new(database, source_id)
                failed_to_open(record.key)
              elsif source.is_a?(Column)
                check_object_recursive(source)
              end
            end
          end
        end

        def open_database_cursor(&block)
          flags =
            TableCursorFlags::ASCENDING |
            TableCursorFlags::BY_ID
          TableCursor.open(@database, :flags => flags, &block)
        end

        def builtin_object_name?(name)
          case name
          when "inspect"
            # Just for compatibility. It's needed for users who used
            # Groonga master at between 2016-02-03 and 2016-02-26.
            true
          else
            false
          end
        end

        def failed(message)
          @on_failure.call(message)
        end

        def failed_to_open(name)
          message =
            "[#{name}] Can't open object. " +
            "It's broken. " +
            "Re-create the object or the database."
          failed(message)
        end
      end

      class Recoverer
        attr_writer :database
        attr_writer :force_truncate

        def initialize
          @context = Context.instance
        end

        def recover
          if @force_truncate
            truncate_corrupt_objects
          end
          @database.recover
        end

        def truncate_corrupt_objects
          @database.each do |object|
            next unless object.corrupt?
            logger = @context.logger
            object_path = object.path
            object_dirname = File.dirname(object_path)
            object_basename = File.basename(object_path)
            object.truncate
            Dir.foreach(object_dirname) do |path|
              if path.start_with?("#{object_basename}.")
                begin
                  File.unlink("#{object_dirname}/#{path}")
                  message = "Corrupted <#{object_path}> related file is removed: <#{path}>"
                  $stdout.puts(message)
                  logger.log(Logger::Level::INFO.to_i, __FILE__, __LINE__, "truncate_corrupt_objects", message)
                rescue Error => error
                  message = "Failed to remove file which is related to corrupted <#{object_path}>: <#{path}>"
                  $stderr.puts(message)
                  logger.log_error(message)
                end
              end
            end
          end
        end
      end
    end
  end
end
