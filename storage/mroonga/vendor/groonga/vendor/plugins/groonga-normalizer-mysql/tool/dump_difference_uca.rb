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
  puts("Usage: #{$0} MYSQL_SOURCE/strings/ctype-uca.c")
  exit(false)
end

parser = CTypeUCAParser.new
parser.parse(ARGF)

n_idencials = 0
n_expanded_characters = 0
parser.weight_based_characters.each do |weight, characters|
  next if characters.size == 1
  n_idencials += 1
  representative_character = characters.first
  rest_characters = characters[1..-1]
  rest_characters.each do |character|
    if representative_character[:utf8].bytesize > character[:utf8].bytesize
      n_expanded_characters += 1
    end
  end
  formatted_weight = weight.collect {|component| '%#07x' % component}.join(', ')
  puts "weight: #{formatted_weight}"
  characters.each do |character|
    utf8 = character[:utf8]
    code_point = character[:code_point]
    p ["U+%04x" % code_point, utf8]
  end
end

puts "Number of idencial weights #{n_idencials}"
puts "Number of expanded characters: #{n_expanded_characters}"
