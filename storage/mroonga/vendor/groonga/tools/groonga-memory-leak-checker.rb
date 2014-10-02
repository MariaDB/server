#!/usr/bin/env ruby

unless respond_to?(:spawn, true)
  puts("Ruby 1.9 is required.")
  exit(false)
end

require 'ostruct'
require 'optparse'
require 'tempfile'
require 'time'

options = OpenStruct.new
options.groonga = "groonga"
options.show_result = false
options.report_progress = true

option_parser = OptionParser.new do |parser|
  parser.banner += " DATABASE COMMAND_FILE1 ..."

  parser.on("--groonga=PATH",
            "Use PATH as groonga command path") do |path|
    options.groonga = path
  end

  parser.on("--[no-]show-result",
            "Show result of command") do |boolean|
    options.show_result = boolean
  end

  parser.on("--[no-]report-progress",
            "Report progress") do |boolean|
    options.report_progress = boolean
  end
end

database, *command_files = option_parser.parse!(ARGV)
if database.nil?
  puts(option_parser)
  exit(false)
end

i = 0
command_files.each do |path|
  File.open(path) do |file|
    file.each_line do |command|
      if options.report_progress
        i += 1
        puts("#{Time.now.iso8601}: #{i} commands done.") if (i % 1000).zero?
      end
      command = command.chomp
      base_name = File.basename($0, ".*")
      log = Tempfile.new("#{base_name}-log")
      command_file = Tempfile.new("#{base_name}-command")
      command_file.puts(command)
      command_file.close
      command_line = [options.groonga,
                      "--log-path", log.path,
                      "--file", command_file.path]
      command_file << "-n" unless File.exist?(database)
      command_line << database
      result = Tempfile.new("#{base_name}-result")
      pid = spawn(*command_line, :out => result.fileno)
      pid, status = Process.waitpid2(pid)
      unless status.success?
        puts("failed to run: (#{status.exitstatus}): " +
             "[#{command_line.join(' ')}]")
        puts("command:")
        puts(command)
        puts("result:")
        result.open
        puts(result.read)
        next
        # exit(false)
      end
      if options.show_result
        result.open
        puts(result.read)
      end
      log.open
      log.each_line do |log_line|
        case log_line
        when /grn_fin \((\d+)\)/
          n_unfreed_allocations = $1.to_i
          unless n_unfreed_allocations.zero?
            puts("maybe memory leak: #{n_unfreed_allocations}: <#{command}>")
            exit(false)
          end
        end
      end
    end
  end
end
