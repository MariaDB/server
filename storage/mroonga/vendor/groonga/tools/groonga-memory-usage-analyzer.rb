#!/usr/bin/env ruby

class Memory < Struct.new(:size, :file, :line, :function)
  def location
    "#{file}:#{line}"
  end
end

class LocationGroup
  attr_reader :location
  attr_reader :memories
  def initialize(location)
    @location = location
    @memories = []
  end

  def add(memory)
    @memories << memory
  end

  def total_size
    @memories.inject(0) do |sum, memory|
      sum + memory.size
    end
  end

  def average_size
    total_size / @memories.size.to_f
  end

  def max_size
    @memories.collect(&:size).max
  end

  def min_size
    @memories.collect(&:size).min
  end
end

class Statistics
  def initialize
    @location_groups = {}
  end

  def add(memory)
    group = location_group(memory.location)
    group.add(memory)
  end

  def sort_by_size
    @location_groups.values.sort_by do |group|
      group.total_size
    end
  end

  private
  def location_group(location)
    @location_groups[location] ||= LocationGroup.new(location)
  end
end

statistics = Statistics.new

ARGF.each_line do |line|
  case line
  when /\Aaddress\[\d+\]\[not-freed\]:\s
          (?:0x)?[\da-fA-F]+\((\d+)\):\s
          (.+?):(\d+):\s(\S+)/x
    size = $1.to_i
    file = $2
    line = $3.to_i
    function = $4.strip
    memory = Memory.new(size, file, line, function)
    statistics.add(memory)
  end
end

def format_size(size)
  if size < 1024
    "#{size}B"
  elsif size < (1024 * 1024)
    "%.3fKiB" % (size / 1024.0)
  elsif size < (1024 * 1024 * 1024)
    "%.3fMiB" % (size / 1024.0 / 1024.0)
  elsif size < (1024 * 1024 * 1024 * 1024)
    "%.3fGiB" % (size / 1024.0 / 1024.0 / 1024.0)
  else
    "#{size}B"
  end
end

puts("%10s(%10s:%10s:%10s): %s(%s)" % [
       "Total",
       "Average",
       "Max",
       "Min",
       "Location",
       "N allocations",
     ])
top_allocated_groups = statistics.sort_by_size.reverse_each.take(10)
top_allocated_groups.each do |group|
  puts("%10s(%10s:%10s:%10s): %s(%d)" % [
         format_size(group.total_size),
         format_size(group.average_size),
         format_size(group.max_size),
         format_size(group.min_size),
         group.location,
         group.memories.size,
       ])
end

puts
puts("Top allocated location's details")
top_allocated_group = top_allocated_groups.first
target_memories = top_allocated_group.memories
size_width = Math.log10(target_memories.size).floor + 1
target_memories.group_by(&:size).sort_by do |size, memories|
  size * memories.size
end.reverse_each do |size, memories|
  total_size = memories.inject(0) {|sum, memory| sum + memory.size}
  puts("%10s(%10s * %#{size_width}d): %s" % [
         format_size(total_size),
         format_size(size),
         memories.size,
         memories.first.location,
       ])
end
