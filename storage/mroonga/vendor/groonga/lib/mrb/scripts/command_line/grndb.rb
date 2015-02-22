module Groonga
  module CommandLine
    class Grndb
      def initialize(argv)
        @command, *@arguments = argv
        @succeeded = true
        @executed = false
        @database_path = nil
      end

      def run
        slop = create_slop
        rest = nil
        begin
          rest = slop.parse(@arguments)
        rescue Slop::Error
          $stderr.puts($!.message)
          return false
        end

        if slop.help?
          $stdout.puts(slop.help)
          return true
        end

        unless @executed
          if rest.empty?
            $stderr.puts("No command is specified.")
          else
            $stderr.puts("Unknown command: <#{rest.first}>")
          end
          return false
        end

        @succeeded
      end

      private
      def create_slop
        slop = Slop.new
        command_name = File.basename(@command)
        slop.banner = "Usage: #{command_name} COMMAND [OPTIONS] DB_PATH"
        slop_enable_help(slop)

        slop.command "check" do |command|
          command.description "Check database"
          slop_enable_help(command)

          command.run do |options, arguments|
            run_command(options, arguments) do |database, new_arguments|
              check(database, options, new_arguments)
            end
          end
        end

        slop.command "recover" do |command|
          command.description "Recover database"
          slop_enable_help(command)

          command.run do |options, arguments|
            run_command(options, arguments) do |database, new_arguments|
              recover(database, options, new_arguments)
            end
          end
        end

        slop
      end

      def slop_enable_help(slop)
        slop.on("-h", "--help", "Display this help message.", :tail => true)
      end

      def open_database(arguments)
        if arguments.empty?
          $stderr.puts("Database path is missing")
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

      def run_command(options, arguments)
        @executed = true

        if options.help?
          $stdout.puts(options.help)
          return
        end

        open_database(arguments) do |database|
          yield(database)
        end
      end

      def recover(database, options, arguments)
        begin
          database.recover
        rescue Error => error
          $stderr.puts("Failed to recover database: <#{@database_path}>")
          $stderr.puts(error.message)
          @succeeded = false
        end
      end

      def check(database, options, arguments)
        if database.locked?
          message =
            "Database is locked. " +
            "It may be broken. " +
            "Re-create the database."
          $stdout.puts(message)
          @succeeded = false
        end

        database.each do |object|
          case object
          when IndexColumn
            next unless object.locked?
            message =
              "[#{object.name}] Index column is locked. " +
              "It may be broken. " +
              "Re-create index by '#{@command} recover #{@database_path}'."
            $stdout.puts(message)
            @succeeded = false
          when Column
            next unless object.locked?
            name = object.name
            message =
              "[#{name}] Data column is locked. " +
              "It may be broken. " +
              "(1) Truncate the column (truncate #{name}) or " +
              "clear lock of the column (lock_clear #{name}) " +
              "and (2) load data again."
            $stdout.puts(message)
            @succeeded = false
          when Table
            next unless object.locked?
            name = object.name
            message =
              "[#{name}] Table is locked. " +
              "It may be broken. " +
              "(1) Truncate the table (truncate #{name}) or " +
              "clear lock of the table (lock_clear #{name}) " +
              "and (2) load data again."
            $stdout.puts(message)
            @succeeded = false
          end
        end
      end
    end
  end
end
