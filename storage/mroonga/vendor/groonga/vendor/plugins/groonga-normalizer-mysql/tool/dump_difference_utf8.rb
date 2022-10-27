# Copyright (C) 2013  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

$LOAD_PATH.unshift(File.dirname(__FILE__))
require "parser"

if ARGV.size != 1
  puts("Usage: #{$0} MYSQL_SOURCE/strings/ctype-utf8.c")
  exit(false)
end

parser = CTypeUTF8Parser.new
parser.parse(ARGF)

n_differences = 0
n_expanded_sort_characters = 0
parser.sorted_pages.each do |page, characters|
  characters.each do |character|
    base = character[:base]
    upper = character[:upper]
    lower = character[:lower]
    sort = character[:sort]
    next if base == sort
    n_differences += 1
    utf8s = [base, upper, lower, sort]
    formatted_code_points = utf8s.collect do |utf8|
      "%#07x" % Unicode.from_utf8(utf8)
    end
    if sort.bytesize > base.bytesize
      n_expanded_sort_characters += 1
    end
    p [utf8s, formatted_code_points]
  end
end

puts "Number of differences: #{n_differences}"
puts "Number of expanded sort characters: #{n_expanded_sort_characters}"
