#!/usr/bin/env ruby
#
# Copyright(C) 2011  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License version 2.1 as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

require "optparse"
require "cool.io"
require "digest"

class Terms
  def initialize
    @sources = []
  end

  def next
    until @sources.empty?
      source = @sources.first
      term = source.next
      @sources.shift if source.empty?
      break if term
    end
    term
  end

  def empty?
    @sources.all?(&:empty?)
  end

  def add_source_file(path)
    @sources << InputSource.new(File.open(path))
  end

  def add_source_input(input)
    @sources << InputSource.new(input)
  end

  class InputSource
    def initialize(source)
      @source = source
      @lines = @source.each_line
      @look_ahead_term = nil
    end

    def next
      if @look_ahead_term
        term = @look_ahead_term
        @look_ahead_term = nil
      else
        loop do
          term = @lines.next.strip
          break unless term.empty?
        end
      end
      term
    rescue StopIteration
      @lines = nil
      @source.close
      @source = nil
      nil
    end

    def empty?
      if @source.nil?
        true
      else
        @look_ahead_term ||= self.next
        @look_ahead_term.nil?
      end
    end
  end
end

class GroongaSuggestHTTPDClient < Coolio::HttpClient
  def initialize(socket, dataset, id, term)
    super(socket)
    @dataset = dataset
    @id = id
    @term = term
    @callbacks = []
  end

  def request
    query = {}
    query["q"] = @term.dup.force_encoding("ASCII-8BIT")
    query["s"] = (Time.now.to_f * 1_000).round.to_s
    query["i"] = @id
    query["t"] = "submit" if rand(10).zero?
    query["l"] = @dataset
    super("GET", "/", :query => query)
  end

  def on_body_data(data)
  end

  def on_request_complete(&block)
    if block
      @callbacks << block
    else
      @callbacks.each do |callback|
        callback.call(self)
      end
    end
  end

  def on_error(reason)
    close
    $stderr.puts("Error: #{reason}")
  end
end

n_connections = 100
host = "localhost"
port = 8080
terms = Terms.new
parser = OptionParser.new
parser.banner += " DATASET"
parser.on("--host=HOST",
          "Use HOST as groonga suggest HTTPD host.",
          "(#{host})") do |_host|
  host = _host
end
parser.on("--port=PORT", Integer,
          "Use PORT as groonga suggest HTTPD port.",
          "(#{port})") do |_port|
  port = _port
end
parser.on("--n-connections=N", Integer,
          "Use N connections.",
          "(#{n_connections})") do |n|
  n_connections = n
end
parser.on("--terms=PATH",
          "Use terns in PATH.",
          "(none)") do |path|
  terms.add_source_file(path)
end

parser.parse!
if ARGV.size != 1
  puts(parser)
  exit(false)
end
dataset = ARGV.shift

if terms.empty? and !$stdin.tty?
  terms.add_source_input($stdin)
end

if terms.empty?
  puts("no terms")
  exit(false)
end

loop = Coolio::Loop.default
run_client = lambda do |id|
  term = terms.next
  return if term.nil?
  client = GroongaSuggestHTTPDClient.connect(host, port, dataset, id, term)
  client.on_request_complete do
    run_client.call(id)
  end
  client.attach(loop)
  client.request
end
n_connections.times do |i|
  id = Digest::SHA2.hexdigest(Time.now.to_f.to_s)
  run_client.call(id)
end
loop.run
