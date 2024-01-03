#!/usr/bin/perl

# Copyright (c) 2024, Väinö Mäkelä
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
# MA 02110-1335  USA

# This script generates a C header containing the supported mariadbd options,
# parsed from mariadbd help output.
#
# Usage:
#
# generate_option_list.pl > mariadbd_options.h

use strict;
use warnings;

my %enums;
my $help_output = `mariadbd --verbose --help`;

print "#include <my_global.h>\n";
print "static const char *mariadbd_valid_options[]= {\n";
while ($help_output =~ /^  --([^ =\[]+)(?:(?:(?<!^  -).)*One\s+of:(.*?)(?=^  -))?/gms) {
    my $enum_part = $2;
    my $option = $1 =~ s/-/_/gr;
    print '"' . $option . "\",\n";
    if ($enum_part) {
        my @cleaned = split(/[\s,\.]+/, $enum_part =~ s/\([^\)]*\)//gr);
        $enums{$option} = \@cleaned;
    }
}
print "};\n";

my @enum_options = sort (keys %enums);
for my $option (@enum_options) {
    print "\nstatic const char *valid_" . $option . "_values[] = {\n";
    for my $value (@{$enums{$option}}) {
        print '"' . $value . "\",\n" if $value;
    }
    print "0\n};\n";
    print "static TYPELIB valid_" . $option . "_values_typelib = {\n"
        . "array_elements(valid_" . $option . "_values)-1,\n"
        . "\"\", valid_" . $option . "_values, 0};\n";
}

print "\nstatic const char *mariadbd_enum_options[] = {\n";
for my $option (@enum_options) {
    print '"' . $option . "\",\n";
}
print "};\n";
print "\nstatic TYPELIB *mariadbd_enum_typelibs[] = {\n";
for my $option (@enum_options) {
    print "&valid_" . $option . "_values_typelib,\n";
}
print "};\n";
