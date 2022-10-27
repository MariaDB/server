module Slop
  class MissingCommand < Error
  end

  class UnknownCommand < Error
    attr_reader :name

    def initialize(msg, name)
      super(msg)
      @name = name
    end
  end
end

module Groonga
  class CommandLineParser
    def initialize(program_name=nil)
      $0 ||= nil
      @program_name = program_name || $0
      @commands = []
      @action_runner = ActionRunner.new
      @options = Slop::Options.new
      setup_options
    end

    def options
      yield(@options) if block_given?
      @options
    end

    def add_command(name)
      command = Command.new(@program_name, name)
      setup_common_options(command.options)
      yield(command)
      @commands << command
    end

    def add_action(&action)
      @action_runner.add(&action)
    end

    def parse(command_line)
      if @commands.empty?
        result = @options.parse(command_line)
        apply_actions(result)
        return result
      end

      if command_line.empty?
        message = "Command is missing"
        raise Slop::MissingCommand.new(message)
      end

      first_argument = command_line.first
      if first_argument.start_with?("-")
        result = @options.parse(command_line)
        apply_actions(result)
        return result
      end

      command_name = first_argument
      command = find_command(command_name)
      if command.nil?
        message = "Unknown command: <#{command_name}>"
        raise Slop::UnknownCommand.new(message, command_name)
      end

      command.parse(command_line[1..-1])
    end

    def help_message
      message = @options.to_s
      @commands.each do |command|
        message << "\n"
        indent = " " * 4
        message << "#{command.description}:\n" if command.description
        message << "#{indent}#{command.options.banner}\n"
      end
      message
    end

    private
    def setup_options
      @options.banner = "Usage: #{@program_name} [OPTIONS]"
      setup_common_options(@options)
      @options.on("-h", "--help", "Display this help message.",
                  :tail => true) do
        $stdout.puts(help_message)
      end
    end

    def setup_common_options(options)
      options.string("--log-path",
                     "Change log path (#{Logger.default_path})",
                     default: Logger.default_path)
      default_log_level = Logger.default_level
      options.string("--log-level",
                     "Change log level (#{default_log_level.name})",
                     default: default_log_level)
    end

    def find_command(name)
      @commands.find do |command|
        command.name == name
      end
    end

    def apply_actions(result)
      @action_runner.run(result) unless result.help?
    end

    class ActionRunner
      def initialize
        @actions = []
      end

      def add(&action)
        @actions << action
      end

      def run(*args)
        @actions.each do |action|
          action.call(*args)
        end
      end
    end

    class Command
      attr_reader :name
      attr_reader :options
      attr_accessor :description
      def initialize(program_name, name)
        @program_name = program_name
        @name = name
        @options = Slop::Options.new
        setup_options
        @description = nil
        @action_runner = ActionRunner.new
      end

      def add_action(&action)
        @action_runner.add(&action)
      end

      def parse(command_line)
        result = @options.parse(command_line)
        @action_runner.run(result) unless result.help?
        result
      end

      def help_message
        message = ""
        message << "Description: #{@description}\n" if @description
        message << @options.to_s
        message
      end

      private
      def setup_options
        @options.banner = "Usage: #{@program_name} #{@name} [OPTIONS]"
        @options.on("-h", "--help", "Display this help message.",
                    :tail => true) do
          $stdout.puts(help_message)
        end
      end
    end
  end
end
