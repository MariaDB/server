#!/usr/bin/env ruby
#
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

ctype_utf8_c_path = ARGV[0]

parser = CTypeUTF8Parser.new
File.open(ctype_utf8_c_path) do |ctype_utf8_c|
  parser.parse(ctype_utf8_c)
end

target_pages = {}
parser.sorted_pages.each do |page, characters|
  characters.each do |character|
    base = character[:base]
    upper = character[:upper]
    lower = character[:lower]
    sort = character[:sort]
    next if base == sort
    target_pages[page] ||= [nil] * 256
    low_code = Unicode.from_utf8(base) & 0xff
    target_pages[page][low_code] = Unicode.from_utf8(sort)
  end
end

normalized_ctype_utf8_c_path =
  ctype_utf8_c_path.sub(/\A.*\/([^\/]+\/strings\/ctype-utf8\.c)\z/, "\\1")
puts(<<-HEADER)
/*
  Copyright(C) 2013  Kouhei Sutou <kou@clear-code.com>

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
  #{normalized_ctype_utf8_c_path}.
  The following is the header of the file:

    Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.

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

    UTF8 according RFC 2279
    Written by Alexander Barkov <bar@udm.net>
*/

#ifndef MYSQL_UTF8_H
#define MYSQL_UTF8_H

#include <stdint.h>
HEADER

def page_name(page)
  "general_ci_page_%02x" % page
end

target_pages.each do |page, characters|
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

static uint32_t *general_ci_table[256] = {
PAGES_HEADER

pages = ["NULL"] * 256
target_pages.each do |page, characters|
  pages[page] = page_name(page)
end
lines = pages.each_slice(2).collect do |pages_group|
  formatted_pages = pages_group.collect do |page|
    "%18s" % page
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
