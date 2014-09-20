#!/usr/bin/env ruby
# -*- coding: utf-8 -*-

require 'fileutils'
require 'optparse'

class BenchmarkSummary
  attr_accessor :options

  def initialize(options)
    @options = options
    @options[:count] ||= 10
  end

  def calc_total(data)
    total = {}
    @options[:count].times do |i|
      data[i + 1].each do |key, value|
        if total[key]
          total[key] = total[key] + value
        else
          total[key] = value
        end
      end
    end
    total
  end

  def print_average(data)
    total = calc_total(data)
    text = "|項目|"
    @options[:count].times do |i|
      text << "#{i + 1}|"
      text << "平均|\n" if i == @options[:count] - 1
    end
    total.each do |key, value|
      line = [key]
      @options[:count].times do |i|
        data[i + 1].each do |data_key, data_value|
          if key == data_key
            line << data_value
          end
        end
      end
      line << [value / @options[:count].to_f]
      text << sprintf("|%s|\n", line.join("|"))
    end
    puts text
  end

  def print_performance(before, after)
    before_total = calc_total(before)
    after_total = calc_total(after)
    ratio = {}
    before_total.each do |key, value|
      ratio[key] = after_total[key] / value
    end
    text = "|項目|変更前|変更後|比率|備考|\n"
    ratio.each do |key, value|
      text << sprintf("|%s|%f|%f|%f||\n",
              key,
              before_total[key] / @options[:count].to_f,
              after_total[key] / @options[:count].to_f,
              ratio[key])
    end
    puts text
  end

  def parse_log(logs)
    parse_result = {}
    logs.each do |index, log|
      File.open(log, "r") do |file|
        data = file.read
        entry = {}
        data.split("\n").each do |line|
          if line =~ /\s*(.+?):\s+\((.+)\)/
            entry[$1] = $2.to_f
          end
        end
        parse_result[index] = entry
      end
    end
    parse_result
  end
end

=begin

Usage: geo-distance-summary.rb \
-b run-bench-geo-distance-orig-N1000 \
-a run-bench-geo-distance-work-N1000

NOTE: expected that there are run-bench-geo-distance-orig-N1000-1.log \
... \
run-bench-geo-distance-orig-N1000-[N].log.

=end

if __FILE__ == $0

  options = {}
  parser = OptionParser.new
  parser.on("-b", "--before PREFIX",
            "log file name must be PREFIX-[N].log") do |prefix|
    options[:before] = prefix
  end
  parser.on("-a", "--after PREFIX",
            "log file name must be PREFIX-[N].log") do |prefix|
    options[:after] = prefix
  end
  parser.on("-n", "data count") do |count|
    options[:count] = count
  end

  parser.parse!(ARGV)

  if not options.has_key?(:before) or not options.has_key?(:after)
    puts(parser.to_s)
    exit
  end

  bench_before_log = {}
  bench_after_log = {}
  Dir.glob("#{options[:before]}*.log") do |log|
    log =~ /(.+)-(\d+)\.log$/
    bench_before_log[$2.to_i] = log
  end
  Dir.glob("#{options[:after]}*.log") do |log|
    log =~ /(.+)-(\d+)\.log$/
    bench_after_log[$2.to_i] = log
  end

  bench = BenchmarkSummary.new(options)
  bench_before = bench.parse_log(bench_before_log)
  bench_after = bench.parse_log(bench_after_log)

  bench.print_average(bench_before)
  bench.print_average(bench_after)
  bench.print_performance(bench_before, bench_after)
end
