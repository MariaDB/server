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

require "English"

module Unicode
  module_function
  def to_utf8(code_point)
    [code_point].pack("U")
  end

  def from_utf8(utf8)
    utf8.unpack("U")[0]
  end
end

class CTypeUTF8Parser
  def initialize
    @pages = {}
  end

  def parse(input)
    parse_ctype_utf8(input)
    normalize_pages
  end

  def sorted_pages
    @pages.sort_by do |page, characters|
      page
    end
  end

  private
  def parse_ctype_utf8(input)
    current_page = nil
    input.each_line do |line|
      case line
      when / plane([\da-fA-F]{2})\[\]=/
        current_page = $1.to_i(16)
        @pages[current_page] = []
      when /^\s*
             \{0x([\da-z]+),0x([\da-z]+),0x([\da-z]+)\},
             \s*
             \{0x([\da-z]+),0x([\da-z]+),0x([\da-z]+)\},?$/ix
        next if current_page.nil?
        parsed_characters = $LAST_MATCH_INFO.captures.collect do |value|
          Unicode.to_utf8(value.to_i(16))
        end
        upper1, lower1, sort1, upper2, lower2, sort2 = parsed_characters
        characters = @pages[current_page]
        characters << {:upper => upper1, :lower => lower1, :sort => sort1}
        characters << {:upper => upper2, :lower => lower2, :sort => sort2}
      when /^\};$/
        current_page = nil
      end
    end
  end

  def normalize_pages
    @pages.each do |page, characters|
      characters.each_with_index do |character, i|
        character[:base] = Unicode.to_utf8((page << 8) + i)
      end
    end
  end
end

class CTypeUCAParser
  attr_reader :pages
  def initialize(version=nil)
    @version = version
    @pages = {}
    @lengths = []
  end

  def parse(input)
    parse_ctype_uca(input)
    normalize_pages
  end

  def weight_based_characters
    weight_based_characters = {}
    sorted_pages.each do |page, characters|
      characters.each do |character|
        weight = character[:weight]
        weight_based_characters[weight] ||= []
        weight_based_characters[weight] << character
      end
    end
    weight_based_characters
  end

  def sorted_pages
    @pages.sort_by do |page, characters|
      page
    end
  end

  def n_pages
    @lengths.size
  end

  private
  def page_data_pattern
    if @version == "520"
      / uca520_p([\da-fA-F]{3})\[\]=/
    else
      / page([\da-fA-F]{3})data\[\]=/
    end
  end

  def length_pattern
    if @version == "520"
      / uca520_length\[4352\]=/
    else
      / uca_length\[256\]=/
    end
  end

  def parse_ctype_uca(input)
    current_page = nil
    in_length = false
    input.each_line do |line|
      case line
      when page_data_pattern
        current_page = $1.to_i(16)
        @pages[current_page] = []
      when /^\s*(0x(?:[\da-z]+)(?:,\s*0x(?:[\da-z]+))*),?(?: \/\*.+\*\/)?$/i
        weight_values = $1
        next if current_page.nil?
        weights = weight_values.split(/,\s*/).collect do |component|
          Integer(component)
        end
        @pages[current_page].concat(weights)
      when length_pattern
        in_length = true
      when /^\d+(?:,\d+)*,?$/
        next unless in_length
        current_lengths = line.chomp.split(/,/).collect do |length|
          Integer(length)
        end
        @lengths.concat(current_lengths)
      when /^\};$/
        current_page = nil
        in_length = false
      end
    end
  end

  def normalize_pages
    @pages.each do |page, flatten_weights|
      weights = flatten_weights.each_slice(@lengths[page])
      @pages[page] = weights.with_index.collect do |weight, i|
        if weight.all?(&:zero?)
          weight = [0]
        else
          while weight.last.zero?
            weight.pop
          end
        end
        code_point = (page << 8) + i
        {
          :weight     => weight,
          :code_point => code_point,
          :utf8       => Unicode.to_utf8(code_point),
        }
      end
    end
  end
end
