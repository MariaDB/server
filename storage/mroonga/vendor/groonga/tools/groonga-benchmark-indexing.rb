#!/usr/bin/env ruby

require "fileutils"
require "json"
require "optparse"

class IndexingBenchmarker
  def initialize
    @groonga = "groonga"
    @database_path = nil
    @benchmark_database_dir = detect_benchmark_database_dir
  end

  def run
    catch(:run) do
      parse_options!
    end

    dump_no_indexes = dump("dump-no-indexes.grn",
                           "--dump_indexes", "no")
    dump_only_indexes = dump("dump-only-indexes.grn",
                             "--dump_plugins", "no",
                             "--dump_schema", "no",
                             "--dump_records", "no",
                             "--dump_configs", "no")
    dump_no_records = dump("dump-no-records.grn",
                           "--dump_records", "no")
    dump_only_records = dump("dump-only-records.grn",
                             "--dump_plugins", "no",
                             "--dump_schema", "no",
                             "--dump_indexes", "no",
                             "--dump_configs", "no")

    create_benchmark_database do
      p [:load_record, measure(dump_no_indexes)]
      p [:static_index_creation, measure(dump_only_indexes)]
    end

    create_benchmark_database do
      p [:create_schema, measure(dump_no_records)]
      p [:load_record_and_create_index, measure(dump_only_records)]
    end

    true
  end

  private
  def detect_benchmark_database_dir
    candiates = [
      "/dev/shm",
      "tmp",
    ]
    candiates.find do |candidate|
      File.exist?(candidate)
    end
  end

  def benchmark_database_path
    "#{@benchmark_database_dir}/bench-db/db"
  end

  def parse_options!
    option_parser = OptionParser.new do |parser|
      parser.banner += " SOURCE_DATABASE"

      parser.on("--groonga=PATH",
                "Use PATH as groonga command path") do |path|
        @groonga = path
      end

      parser.on("--benchmark-database-dir=DIR",
                "Use DIR to put benchmark database") do |dir|
        @benchmark_database_dir = dir
      end
    end

    @database_path, = option_parser.parse!(ARGV)
    if @database_path.nil?
      puts(option_parser)
      throw(:run)
    end
  end

  def dump(path, *dump_options)
    return path if File.exist?(path)
    unless system(@groonga,
                  @database_path,
                  "dump",
                  *dump_options,
                  :out => path)
      raise "failed to dump: #{dump_options.inspect}"
    end
    path
  end

  def create_benchmark_database
    dir = File.dirname(benchmark_database_path)
    FileUtils.rm_rf(dir)
    FileUtils.mkdir_p(dir)
    system(@groonga,
           "-n", benchmark_database_path,
           "shutdown",
           :out => IO::NULL)
    begin
      yield
    ensure
      FileUtils.rm_rf(dir)
    end
  end

  def measure(dump_path)
    result = "result"
    begin
      system(@groonga,
             "--file", dump_path,
             benchmark_database_path,
             :out => result)
      File.open(result) do |output|
        output.each_line.inject(0) do |result, line|
          result + JSON.parse(line)[0][2]
        end
      end
    ensure
      FileUtils.rm_f(result)
    end
  end
end

exit(IndexingBenchmarker.new.run)
