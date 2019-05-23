#!/usr/bin/env ruby
# -*- coding: utf-8 -*-
#
# Copyright (C) 2013-2015  Kouhei Sutou <kou@clear-code.com>
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

require "optparse"

$LOAD_PATH.unshift(File.dirname(__FILE__))
require "parser"

@version = nil
@suffix = ""
@split_small_kana_p = false
@split_kana_with_voiced_sound_mark_p = false
@split_kana_with_semi_voiced_sound_mark_p = false

option_parser = OptionParser.new
option_parser.banner += " MYSQL_SOURCE/strings/ctype-uca.c"

option_parser.on("--version=VERSION", "Use VERSION as UCA version") do |version|
  @version = version
end

option_parser.on("--suffix=SUFFIX", "Add SUFFIX to names") do |suffix|
  @suffix = suffix
end

option_parser.on("--[no-]split-small-kana",
                 "Split small hiragana (katakana) and " +
                   "large hiragana (katakana)",
                 "(#{@split_small_kana_p})") do |boolean|
  @split_small_kana_p = boolean
end

option_parser.on("--[no-]split-kana-with-voiced-sound-mark",
                 "Split hiragana (katakana) with voiced sound mark",
                 "(#{@split_kana_with_voiced_sound_mark})") do |boolean|
  @split_kana_with_voiced_sound_mark_p = boolean
end

option_parser.on("--[no-]split-kana-with-semi-voiced-sound-mark",
                 "Split hiragana (katakana) with semi-voiced sound mark",
                 "(#{@split_kana_with_semi_voiced_sound_mark})") do |boolean|
  @split_kana_with_semi_voiced_sound_mark_p = boolean
end

begin
  option_parser.parse!(ARGV)
rescue OptionParser::Error
  puts($!)
  exit(false)
end

if ARGV.size != 1
  puts(option_parser)
  exit(false)
end

ctype_uca_c_path = ARGV[0]

parser = CTypeUCAParser.new(@version)
File.open(ctype_uca_c_path) do |ctype_uca_c|
  parser.parse(ctype_uca_c)
end

SMALL_KANAS = [
  "ぁ", "ぃ", "ぅ", "ぇ", "ぉ",
  "っ",
  "ゃ", "ゅ", "ょ",
  "ゎ",
  "ァ", "ィ", "ゥ", "ェ", "ォ",
  "ッ",
  "ャ", "ュ", "ョ",
  "ヮ",
  "ｧ", "ｨ", "ｩ", "ｪ", "ｫ",
  "ｯ",
  "ｬ", "ｭ", "ｮ",
]
def small_kana?(character)
  SMALL_KANAS.include?(character[:utf8])
end

KANA_WITH_VOICED_SOUND_MARKS = [
  "が", "ぎ", "ぐ", "げ", "ご",
  "ざ", "じ", "ず", "ぜ", "ぞ",
  "だ", "ぢ", "づ", "で", "ど",
  "ば", "び", "ぶ", "べ", "ぼ",
  "ガ", "ギ", "グ", "ゲ", "ゴ",
  "ザ", "ジ", "ズ", "ゼ", "ゾ",
  "ダ", "ヂ", "ヅ", "デ", "ド",
  "バ", "ビ", "ブ", "ベ", "ボ",
]
def kana_with_voiced_sound_mark?(character)
  KANA_WITH_VOICED_SOUND_MARKS.include?(character[:utf8])
end

KANA_WITH_SEMI_VOICED_SOUND_MARKS = [
  "ぱ", "ぴ", "ぷ", "ぺ", "ぽ",
  "パ", "ピ", "プ", "ペ", "ポ",
]
def kana_with_semi_voiced_sound_mark?(character)
  KANA_WITH_SEMI_VOICED_SOUND_MARKS.include?(character[:utf8])
end

def split_characters(characters)
  grouped_characters = characters.group_by do |character|
    if @split_small_kana_p and small_kana?(character)
      :small_kana
    elsif @split_kana_with_voiced_sound_mark_p and
        kana_with_voiced_sound_mark?(character)
      :kana_with_voiced_sound_mark
    elsif @split_kana_with_semi_voiced_sound_mark_p and
        kana_with_semi_voiced_sound_mark?(character)
      :kana_with_semi_voiced_sound_mark
    else
      :other
    end
  end
  grouped_characters.values
end

grouped_characters = []
parser.weight_based_characters.each do |weight, characters|
  grouped_characters.concat(split_characters(characters))
end

GREEK_CAPITAL_UNICODE_RANGE = Unicode.from_utf8("Α")..Unicode.from_utf8("Ω")
def find_greek_capital_character(characters)
  characters.find do |character|
    GREEK_CAPITAL_UNICODE_RANGE.cover?(character[:code_point])
  end
end

def find_representative_character(characters)
  representative_character = nil
  case characters.first[:utf8]
  when "⺄", "⺇", "⺈", "⺊", "⺌", "⺗"
    representative_character = characters.last
  when "⺜", "⺝", "⺧", "⺫", "⺬", "⺮", "⺶", "⺻", "⺼", "⺽"
    representative_character = characters[1]
  when "⻆", "⻊", "⻏", "⻑", "⻕", "⻗", "⻝", "⻡", "⻣", "⻤"
    representative_character = characters.last
  when "⻱", "⼀", "⼆", "⼈"
    representative_character = characters[1]
  when "ぁ", "ぃ", "ぅ", "ぇ", "ぉ", "っ", "ゃ", "ゅ", "ょ", "ゎ"
    representative_character = characters[1] unless @split_small_kana_p
  else
    representative_character ||= find_greek_capital_character(characters)
  end
  representative_character ||= characters.first
  representative_character
end

target_pages = {}
grouped_characters.each do |characters|
  next if characters.size == 1
  representative_character = find_representative_character(characters)
  representative_code_point = representative_character[:code_point]
  rest_characters = characters.reject do |character|
    character == representative_character
  end
  rest_characters.each do |character|
    code_point = character[:code_point]
    page = code_point >> 8
    low_code = code_point & 0xff
    target_pages[page] ||= [nil] * 256
    target_pages[page][low_code] = representative_code_point
  end
end

sorted_target_pages = target_pages.sort_by do |page, code_points|
  page
end


normalized_ctype_uca_c_path =
  ctype_uca_c_path.sub(/\A.*\/([^\/]+\/strings\/ctype-uca\.c)\z/, "\\1")

header_guard_id = "MYSQL_UCA"
if @version
  header_guard_id << "_#{@version}"
end
header_guard_id << "#{@suffix.upcase}_H"

puts(<<-HEADER)
/*
  Copyright(C) 2013-2015  Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License as published by the Free Software Foundation; version 2
  of the License.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public
  License along with this library; if not, write to the Free
  Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA 02110-1335  USA

  This file uses normalization table defined in
  #{normalized_ctype_uca_c_path}.
  The following is the header of the file:

    Copyright (c) 2004, 2014, Oracle and/or its affiliates. All rights reserved.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; version 2
    of the License.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
    MA 02110-1335  USA

    UCA (Unicode Collation Algorithm) support.
    Written by Alexander Barkov <bar@mysql.com>
*/

#ifndef #{header_guard_id}
#define #{header_guard_id}

#include <stdint.h>
HEADER

def variable_name_prefix
  prefix = "unicode"
  if @version
    prefix << "_#{@version}"
  end
  prefix << "_ci#{@suffix}"
  prefix
end

def page_name(page)
  "#{variable_name_prefix}_page_%02x" % page
end

sorted_target_pages.each do |page, characters|
  puts(<<-PAGE_HEADER)

static uint32_t #{page_name(page)}[] = {
PAGE_HEADER
  lines = characters.each_with_index.each_slice(8).collect do |characters_group|
    formatted_code_points = characters_group.collect do |normalized, low_code|
      normalized ||= (page << 8) + low_code
      "0x%05x" % normalized
    end
    "  " + formatted_code_points.join(", ")
  end
  puts(lines.join(",\n"))
  puts(<<-PAGE_FOOTER)
};
PAGE_FOOTER
end

puts(<<-PAGES_HEADER)

static uint32_t *#{variable_name_prefix}_table[#{parser.n_pages}] = {
PAGES_HEADER

pages = ["NULL"] * parser.n_pages
sorted_target_pages.each do |page, characters|
  pages[page] = page_name(page)
end
lines = pages.each_slice(2).collect do |pages_group|
  formatted_pages = pages_group.collect do |page|
    "%19s" % page
  end
  "  " + formatted_pages.join(", ")
end
puts(lines.join(",\n"))

puts(<<-PAGES_FOOTER)
};
PAGES_FOOTER

puts(<<-FOOTER)

#endif
FOOTER
