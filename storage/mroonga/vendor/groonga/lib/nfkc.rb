#!/usr/bin/env ruby
# -*- coding: utf-8 -*-
#
# Copyright(C) 2010-2016 Brazil
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

CUSTOM_RULE_PATH = 'nfkc-custom-rules.txt'

class SwitchGenerator
  def initialize(unicode_version, output)
    @unicode_version = unicode_version
    @output = output
  end

  def generate(bc, decompose_map, compose_map)
    STDERR.puts('generating char type code..')
    generate_blockcode_char_type(bc)
    STDERR.puts('generating decompose code..')
    generate_decompose(decompose_map)
    STDERR.puts('generating compose code..')
    generate_compose(compose_map)
  end

  private
  def generate_blockcode_char_type(bc)
    @output.puts(<<-HEADER)

grn_char_type
grn_nfkc#{@unicode_version}_char_type(const unsigned char *str)
{
    HEADER

    @lv = 0
    gen_bc(bc, 0)

    @output.puts(<<-FOOTER)
  return -1;
}
    FOOTER
  end

  def gen_bc(hash, level)
    bl = ' ' * (level * 2)
    h2 = {}
    hash.each{|key,val|
      key = key.dup
      key.force_encoding("ASCII-8BIT")
      head = key.bytes[0]
      rest = key[1..-1]
      if h2[head]
        h2[head][rest] = val
      else
        h2[head] = {rest => val}
      end
    }
    if h2.size < 3
      h2.keys.sort.each{|k|
        if (0x80 < k)
          @output.printf("#{bl}if (str[#{level}] < 0x%02X) { return #{@lv}; }\n", k)
        end
        h = h2[k]
        if h.keys.join =~ /^\x80*$/n
          @lv, = h.values
        else
          @output.printf("#{bl}if (str[#{level}] == 0x%02X) {\n", k)
          gen_bc(h, level + 1)
          @output.puts bl + '}'
        end
      }
      @output.puts bl + "return #{@lv};"
    else
      @output.puts bl + "switch (str[#{level}]) {"
      lk = 0x80
      br = true
      h2.keys.sort.each{|k|
        if (lk < k)
          for j in lk..k-1
            @output.printf("#{bl}case 0x%02X :\n", j)
          end
          br = false
        end
        unless br
          @output.puts bl + "  return #{@lv};"
          @output.puts bl + '  break;'
        end
        h = h2[k]
        @output.printf("#{bl}case 0x%02X :\n", k)
        if h.keys.join =~ /^\x80*$/n
          @lv, = h.values
          br = false
        else
          gen_bc(h, level + 1)
          @output.puts bl + '  break;'
          br = true
        end
        lk = k + 1
      }
      @output.puts bl + 'default :'
      @output.puts bl + "  return #{@lv};"
      @output.puts bl + '  break;'
      @output.puts bl + '}'
    end
  end

  def generate_decompose(hash)
    @output.puts(<<-HEADER)

const char *
grn_nfkc#{@unicode_version}_decompose(const unsigned char *str)
{
    HEADER

    gen_decompose(hash, 0)

    @output.puts(<<-FOOTER)
  return 0;
}
    FOOTER
  end

  def gen_decompose(hash, level)
    bl = ' ' * ((level + 0) * 2)
    if hash['']
      dst = ''
      hash[''].each_byte{|b| dst << format('\x%02X', b)}
      @output.puts "#{bl}return \"#{dst}\";"
      hash.delete('')
    end
    return if hash.empty?
    h2 = {}
    hash.each{|key,val|
      key = key.dup
      key.force_encoding("ASCII-8BIT")
      head = key.bytes[0]
      rest = key[1..-1]
      if h2[head]
        h2[head][rest] = val
      else
        h2[head] = {rest => val}
      end
    }
    if h2.size == 1
      h2.each{|key,val|
        @output.printf("#{bl}if (str[#{level}] == 0x%02X) {\n", key)
        gen_decompose(val, level + 1)
        @output.puts bl + '}'
      }
    else
      @output.puts "#{bl}switch (str[#{level}]) {"
      h2.keys.sort.each{|k|
        @output.printf("#{bl}case 0x%02X :\n", k)
        gen_decompose(h2[k], level + 1)
        @output.puts("#{bl}  break;")
      }
      @output.puts bl + '}'
    end
  end

  def generate_compose(compose_map)
    @output.puts(<<-HEADER)

const char *
grn_nfkc#{@unicode_version}_compose(const unsigned char *prefix, const unsigned char *suffix)
{
    HEADER
    suffix = {}
    compose_map.each{|src,dst|
      chars = src.chars
      if chars.size != 2
        STDERR.puts "caution: more than two chars in pattern #{chars.join('|')}"
      end
      s = chars.pop
      if suffix[s]
        suffix[s][chars.join] = dst
      else
        suffix[s] = {chars.join=>dst}
      end
    }
    gen_compose_sub(suffix, 0)
    @output.puts(<<-FOOTER)
  return 0;
}
    FOOTER
  end

  def gen_compose_sub2(hash, level, indent)
    bl = ' ' * ((level + indent + 0) * 2)
    if hash['']
      @output.print "#{bl}return \""
      hash[''].each_byte{|b| @output.printf('\x%02X', b)}
      @output.puts "\";"
      hash.delete('')
    end
    return if hash.empty?

    h2 = {}
    hash.each{|key,val|
      key = key.dup
      key.force_encoding("ASCII-8BIT")
      head = key.bytes[0]
      rest = key[1..-1]
      if h2[head]
        h2[head][rest] = val
      else
        h2[head] = {rest => val}
      end
    }

    if h2.size == 1
      h2.each{|key,val|
        @output.printf("#{bl}if (prefix[#{level}] == 0x%02X) {\n", key)
        gen_compose_sub2(val, level + 1, indent)
        @output.puts bl + '}'
      }
    else
      @output.puts "#{bl}switch (prefix[#{level}]) {"
      h2.keys.sort.each{|k|
        @output.printf("#{bl}case 0x%02X :\n", k)
        gen_compose_sub2(h2[k], level + 1, indent)
        @output.puts("#{bl}  break;")
      }
      @output.puts bl + '}'
    end
  end

  def gen_compose_sub(hash, level)
    bl = ' ' * ((level + 0) * 2)
    if hash['']
      gen_compose_sub2(hash[''], 0, level)
      hash.delete('')
    end
    return if hash.empty?
    h2 = {}
    hash.each{|key,val|
      key = key.dup
      key.force_encoding("ASCII-8BIT")
      head = key.bytes[0]
      rest = key[1..-1]
      if h2[head]
        h2[head][rest] = val
      else
        h2[head] = {rest => val}
      end
    }
    if h2.size == 1
      h2.each{|key,val|
        @output.printf("#{bl}if (suffix[#{level}] == 0x%02X) {\n", key)
        gen_compose_sub(val, level + 1)
        @output.puts bl + '}'
      }
    else
      @output.puts "#{bl}switch (suffix[#{level}]) {"
      h2.keys.sort.each{|k|
        @output.printf("#{bl}case 0x%02X :\n", k)
        gen_compose_sub(h2[k], level + 1)
        @output.puts("#{bl}  break;")
      }
      @output.puts bl + '}'
    end
  end
end

class TableGenerator < SwitchGenerator
  private
  def name_prefix
    "grn_nfkc#{@unicode_version}_"
  end

  def table_name(type, common_bytes)
    suffix = common_bytes.collect {|byte| "%02x" % byte}.join("")
    "#{name_prefix}#{type}_table_#{suffix}"
  end

  def function_name(type)
    "#{name_prefix}#{type}"
  end

  def generate_char_convert_tables(type, return_type, byte_size_groups)
    if return_type.end_with?("*")
      space = ""
    else
      space = " "
    end
    byte_size_groups.keys.sort.each do |common_bytes|
      chars = byte_size_groups[common_bytes]
      lines = []
      all_values = []
      last_bytes = chars.collect {|char| char.bytes.last}
      last_bytes.min.step(last_bytes.max).each_slice(8) do |slice|
        values = slice.collect do |last_byte|
          char = (common_bytes + [last_byte]).pack("c*")
          char.force_encoding("UTF-8")
          yield(char)
        end
        all_values.concat(values)
        lines << ("  " + values.join(", "))
      end

      next if all_values.uniq.size == 1

      @output.puts(<<-TABLE_HEADER)

static #{return_type}#{space}#{table_name(type, common_bytes)}[] = {
      TABLE_HEADER
      @output.puts(lines.join(",\n"))
      @output.puts(<<-TABLE_FOOTER)
};
      TABLE_FOOTER
    end
  end

  def generate_char_convert_function(type,
                                     argument_list,
                                     char_variable,
                                     default,
                                     return_type,
                                     byte_size_groups,
                                     options={})
    modifier = options[:internal] ? "static inline " : ""
    @output.puts(<<-HEADER)

#{modifier}#{return_type}
#{function_name(type)}(#{argument_list})
{
    HEADER

    prev_common_bytes = []
    prev_n_common_bytes = 0
    first_group = true
    byte_size_groups.keys.sort.each do |common_bytes|
      chars = byte_size_groups[common_bytes]
      chars_bytes = chars.collect(&:bytes).sort
      min = chars_bytes.first.last
      max = chars_bytes.last.last
      n_common_bytes = 0
      if common_bytes.empty?
        indent = "  "
        yield(:no_common_bytes, indent, chars, chars_bytes)
      else
        if first_group
          @output.puts(<<-BODY)
  {
          BODY
        end

        found_different_byte = false
        common_bytes.each_with_index do |common_byte, i|
          unless found_different_byte
            if prev_common_bytes[i] == common_byte
              n_common_bytes += 1
              next
            end
            found_different_byte = true
          end
          indent = "  " * i
          # p [i, prev_common_bytes.collect{|x| "%#04x" % x}, common_bytes.collect{|x| "%#04x" % x}, "%#04x" % common_byte, n_common_bytes, prev_n_common_bytes]
          # TODO: The following code may be able to be simplified.
          if prev_common_bytes[i].nil?
            # p nil
            @output.puts(<<-BODY)
    #{indent}switch (#{char_variable}[#{i}]) {
            BODY
          elsif i < prev_n_common_bytes
            # p :prev
            @output.puts(<<-BODY)
    #{indent}  default :
    #{indent}    break;
    #{indent}  }
    #{indent}  break;
            BODY
          elsif n_common_bytes < prev_n_common_bytes
            # p :common_prev
            @output.puts(<<-BODY)
    #{indent}switch (#{char_variable}[#{i}]) {
            BODY
          else
            # p :else
            prev_common_bytes.size.downto(common_bytes.size + 1) do |j|
              sub_indent = "  " * (j - 1)
              @output.puts(<<-BODY)
    #{indent}#{sub_indent}default :
    #{indent}#{sub_indent}  break;
    #{indent}#{sub_indent}}
    #{indent}#{sub_indent}break;
              BODY
            end
          end
          @output.puts(<<-BODY)
    #{indent}case #{"%#04x" % common_byte} :
          BODY
        end

        n = chars_bytes.first.size - 1
        indent = "    " + ("  " * common_bytes.size)
        yield(:have_common_bytes, indent, chars, chars_bytes, n, common_bytes)
      end

      prev_common_bytes = common_bytes
      prev_n_common_bytes = n_common_bytes
      first_group = false
    end

    # p [prev_common_bytes.collect{|x| "%#04x" % x}, prev_n_common_bytes]

    (prev_common_bytes.size - 1).step(0, -1) do |i|
      indent = "  " * i
      @output.puts(<<-BODY)
    #{indent}default :
    #{indent}  break;
    #{indent}}
      BODY
      if i > 0
        @output.puts(<<-BODY)
    #{indent}break;
        BODY
      end
    end

    @output.puts(<<-FOOTER)
  }

  return #{default};
}
    FOOTER
  end

  def generate_char_converter(type,
                              function_type,
                              char_map,
                              default,
                              return_type,
                              options={},
                              &converter)
    byte_size_groups = char_map.keys.group_by do |from|
      bytes = from.bytes
      bytes[0..-2]
    end

    generate_char_convert_tables(type,
                                 return_type,
                                 byte_size_groups,
                                 &converter)

    char_variable = "utf8"
    generate_char_convert_function(function_type,
                                   "const unsigned char *#{char_variable}",
                                   char_variable,
                                   default,
                                   return_type,
                                   byte_size_groups,
                                   options) do |state, *args|
      case state
      when :no_common_bytes
        indent, chars, chars_bytes = args
        if chars.size == 1
          char = chars[0]
          char_byte = chars_bytes.first.first
          value = yield(char)
          @output.puts(<<-BODY)
#{indent}if (#{char_variable}[0] < 0x80) {
#{indent}  if (#{char_variable}[0] == #{"%#04x" % char_byte}) {
#{indent}    return #{value};
#{indent}  } else {
#{indent}    return #{default};
#{indent}  }
#{indent}} else {
          BODY
        else
          min = chars_bytes.first.first
          max = chars_bytes.last.first
          @output.puts(<<-BODY)
#{indent}if (#{char_variable}[0] < 0x80) {
#{indent}  if (#{char_variable}[0] >= #{"%#04x" % min} &&
#{indent}      #{char_variable}[0] <= #{"%#04x" % max}) {
#{indent}    return #{table_name(type, [])}[#{char_variable}[0] - #{"%#04x" % min}];
#{indent}  } else {
#{indent}    return #{default};
#{indent}  }
#{indent}} else {
          BODY
        end
      when :have_common_bytes
        indent, chars, chars_bytes, n, common_bytes = args
        if chars.size == 1
          char = chars[0]
          char_byte = chars_bytes.first.last
          value = yield(char)
          @output.puts(<<-BODY)
#{indent}if (#{char_variable}[#{n}] == #{"%#04x" % char_byte}) {
#{indent}  return #{value};
#{indent}}
#{indent}break;
          BODY
        else
          sorted_chars = chars.sort
          min = chars_bytes.first.last
          max = chars_bytes.last.last
          all_values = (min..max).collect do |last_byte|
            char = (common_bytes + [last_byte]).pack("c*")
            char.force_encoding("UTF-8")
            yield(char)
          end
          if all_values.uniq.size == 1
            value = all_values.first
          else
            value = "#{table_name(type, common_bytes)}[#{char_variable}[#{n}] - #{"%#04x" % min}]"
          end
          last_n_bits_for_char_in_utf8 = 6
          max_n_chars_in_byte = 2 ** last_n_bits_for_char_in_utf8
          if all_values.size == max_n_chars_in_byte
            @output.puts(<<-BODY)
#{indent}return #{value};
            BODY
          else
            @output.puts(<<-BODY)
#{indent}if (#{char_variable}[#{n}] >= #{"%#04x" % min} &&
#{indent}    #{char_variable}[#{n}] <= #{"%#04x" % max}) {
#{indent}  return #{value};
#{indent}}
#{indent}break;
            BODY
          end
        end
      end
    end
  end

  def generate_blockcode_char_type(block_codes)
    default = "GRN_CHAR_OTHERS"

    char_types = {}
    current_type = default
    prev_char = nil
    block_codes.keys.sort.each do |char|
      type = block_codes[char]
      if current_type != default
        prev_code_point = prev_char.codepoints[0]
        code_point = char.codepoints[0]
        (prev_code_point...code_point).each do |target_code_point|
          target_char = [target_code_point].pack("U*")
          char_types[target_char] = current_type
        end
      end
      current_type = type
      prev_char = char
    end
    unless current_type == default
      raise "TODO: Consider the max unicode character"
      max_unicode_char = "\u{10ffff}"
      (prev_char..max_unicode_char).each do |target_char|
        char_types[target_char] = current_type
      end
    end

    generate_char_converter("char_type",
                            "char_type",
                            char_types,
                            default,
                            "grn_char_type") do |char|
      char_types[char] || default
    end
  end

  def generate_decompose(decompose_map)
    default = "NULL"
    generate_char_converter("decompose",
                            "decompose",
                            decompose_map,
                            default,
                            "const char *") do |from|
      to = decompose_map[from]
      if to
        escaped_value = to.bytes.collect {|char| "\\x%02x" % char}.join("")
        "\"#{escaped_value}\""
      else
        default
      end
    end
  end

  def generate_compose(compose_map)
    # require "pp"
    # p compose_map.size
    # pp compose_map.keys.group_by {|x| x.chars[1]}.size
    # pp compose_map.keys.group_by {|x| x.chars[1]}.collect {|k, vs| [k, k.codepoints, vs.size, vs.group_by {|x| x.chars[0].bytesize}.collect {|k2, vs2| [k2, vs2.size]}]}
    # pp compose_map.keys.group_by {|x| x.chars[0].bytesize}.collect {|k, vs| [k, vs.size]}
    # pp compose_map

    suffix_char_map = {}
    compose_map.each do |source, destination|
      chars = source.chars
      if chars.size != 2
        STDERR.puts "caution: more than two chars in pattern #{chars.join('|')}"
        return
      end
      prefix, suffix = chars
      suffix_char_map[suffix] ||= {}
      suffix_char_map[suffix][prefix] = destination
    end

    suffix_char_map.each do |suffix, prefix_char_map|
      suffix_bytes = suffix.bytes.collect {|byte| "%02x" % byte}.join("")
      default = "NULL"
      generate_char_converter("compose_prefix_#{suffix_bytes}",
                              "compose_prefix_#{suffix_bytes}",
                              prefix_char_map,
                              default,
                              "const char *",
                              :internal => true) do |prefix|
        to = prefix_char_map[prefix]
        if to
          escaped_value = to.bytes.collect {|char| "\\x%02x" % char}.join("")
          "\"#{escaped_value}\""
        else
          default
        end
      end
    end


    char_variable = "suffix_utf8"
    argument_list =
      "const unsigned char *prefix_utf8, " +
      "const unsigned char *#{char_variable}"
    default = "NULL"
    byte_size_groups = suffix_char_map.keys.group_by do |from|
      bytes = from.bytes
      bytes[0..-2]
    end
    generate_char_convert_function("compose",
                                   argument_list,
                                   char_variable,
                                   default,
                                   "const char *",
                                   byte_size_groups) do |type, *args|
      case type
      when :no_common_bytes
        indent, chars, chars_bytes = args
        @output.puts(<<-BODY)
#{indent}switch (#{char_variable}[0]) {
        BODY
        chars.each do |char|
          suffix_bytes = char.bytes.collect {|byte| "%02x" % byte}.join("")
          type = "compose_prefix_#{suffix_bytes}"
          @output.puts(<<-BODY)
#{indent}case #{"%#04x" % char.bytes.last} :
#{indent}  return #{function_name(type)}(prefix_utf8);
          BODY
        end
        @output.puts(<<-BODY)
#{indent}default :
#{indent}  return #{default};
#{indent}}
#{indent}break;
        BODY
      when :have_common_bytes
        indent, chars, chars_bytes, n, common_bytes = args
        @output.puts(<<-BODY)
#{indent}switch (#{char_variable}[#{n}]) {
        BODY
        chars.each do |char|
          suffix_bytes = char.bytes.collect {|byte| "%02x" % byte}.join("")
          type = "compose_prefix_#{suffix_bytes}"
          @output.puts(<<-BODY)
#{indent}case #{"%#04x" % char.bytes.last} :
#{indent}  return #{function_name(type)}(prefix_utf8);
          BODY
        end
        @output.puts(<<-BODY)
#{indent}default :
#{indent}  return #{default};
#{indent}}
#{indent}break;
        BODY
      end
    end
  end

  def to_bytes_map(char_map)
    bytes_map = {}
    char_map.each_key do |from|
      parent = bytes_map
      from.bytes[0..-2].each do |byte|
        parent[byte] ||= {}
        parent = parent[byte]
      end
      parent[from.bytes.last] = char_map[from]
    end
    bytes_map
  end
end

def create_bc(option)
  bc = {}
  open("|./icudump --#{option}").each{|l|
    src,_,code = l.chomp.split("\t")
    str = src.split(':').collect(&:hex).pack("c*")
    str.force_encoding("UTF-8")
    bc[str] = code
  }
  bc
end

def ccpush(hash, src, dst)
  head = src.shift
  hash[head] = {} unless hash[head]
  if head
    ccpush(hash[head], src, dst)
  else
    hash[head] = dst
  end
end

def subst(hash, str)
  cand = nil
  src = str.chars
  for i in 0..src.size-1
    h = hash
    for j in i..src.size-1
      head = src[j]
      h = h[head]
      break unless h
      if h[nil]
        cand = src[0,i].join("") + h[nil] + src[j + 1..-1].join("")
      end
    end
    return cand if cand
  end
  return str
end

def map_entry(decompose, cc, src, dst)
  dst.downcase! unless $case_sensitive
  loop {
    dst2 = subst(cc, dst)
    break if dst2 == dst
    dst = dst2
  }
  unless $keep_space
    dst = $1 if dst =~ /^ +([^ ].*)$/
  end
  decompose[src] = dst if src != dst
end

def create_decompose_map()
  cc = {}
  open('|./icudump --cc').each{|l|
    _,src,dst = l.chomp.split("\t")
    if cc[src]
      STDERR.puts "caution: ambiguous mapping #{src}|#{cc[src]}|#{dst}" if cc[src] != dst
    end
    ccpush(cc, src.chars, dst)
  }
  decompose_map = {}
  open('|./icudump --nfkd').each{|l|
    n,src,dst = l.chomp.split("\t")
    map_entry(decompose_map, cc, src, dst)
  }
  if File.readable?(CUSTOM_RULE_PATH)
    open(CUSTOM_RULE_PATH).each{|l|
      src,dst = l.chomp.split("\t")
      map_entry(decompose_map, cc, src, dst)
    }
  end
  unless $case_sensitive
    for c in 'A'..'Z'
      decompose_map[c] = c.downcase
    end
  end
  return decompose_map
end

def create_compose_map(decompose_map)
  cc = {}
  open('|./icudump --cc').each{|l|
    _,src,dst = l.chomp.split("\t")
    src = src.chars.collect{|c| decompose_map[c] || c}.join
    dst = decompose_map[dst] || dst
    if cc[src] && cc[src] != dst
      STDERR.puts("caution: inconsitent mapping '#{src}' => '#{cc[src]}'|'#{dst}'")
    end
    cc[src] = dst if src != dst
  }
  loop {
    noccur = 0
    cc2 = {}
    cc.each {|src,dst|
      src2 = src
      chars = src.chars
      l = chars.size - 1
      for i in 0..l
        for j in i..l
          next if i == 0 && j == l
          str = chars[i..j].join
          if decompose_map[str]
            STDERR.printf("caution: recursive mapping '%s'=>'%s'\n",
                          str, decompose_map[str])
          end
          if cc[str]
            src2 = (i > 0 ? chars[0..i-1].join : '') + cc[str] + (j < l ? chars[j+1..l].join : '')
            noccur += 1
          end
        end
      end
      cc2[src2] = dst if src2 != dst
    }
    cc = cc2
    STDERR.puts("substituted #{noccur} patterns.")
    break if noccur == 0
    STDERR.puts('try again..')
  }
  return cc
end

######## main #######

generator_class = SwitchGenerator
ARGV.each{|arg|
  case arg
  when /-*c/i
    $case_sensitive = true
  when /-*s/i
    $keep_space = true
  when "--impl=switch"
    generator_class = SwitchGenerator
  when "--impl=table"
    generator_class = TableGenerator
  end
}

STDERR.puts('compiling icudump')
system('cc -Wall -O3 -o icudump -I/tmp/local/include -L/tmp/local/lib icudump.c -licuuc -licui18n')

STDERR.puts('getting Unicode version')
unicode_version = `./icudump --version`.strip.gsub(".", "")

STDERR.puts('creating bc..')
bc = create_bc("gc")

STDERR.puts('creating decompose map..')
decompose_map = create_decompose_map()

STDERR.puts('creating compose map..')
compose_map = create_compose_map(decompose_map)

File.open("nfkc#{unicode_version}.c", "w") do |output|
  output.puts(<<-HEADER)
/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

/*
  Don't edit this file by hand. it generated automatically by nfkc.rb.
*/

#include "grn.h"
#include "grn_nfkc.h"
#include <groonga/nfkc.h>

#ifdef GRN_WITH_NFKC
  HEADER

  generator = generator_class.new(unicode_version, output)
  generator.generate(bc, decompose_map, compose_map)

  output.puts(<<-FOOTER)

#endif /* GRN_WITH_NFKC */

  FOOTER
end
