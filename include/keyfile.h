/* Copyright (C) 2014 eperi GmbH. All Rights Reserved.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/******************************************************************/
#ifndef KEYFILE_H
#define KEYFILE_H
#include<stdio.h>

struct keyentry {
    int id;
    char *iv;
    char *key;
};

int
parseFile(FILE * fp, struct keyentry **allKeys, const int k_len, const char *secret);

int
parseLine(const char *line, struct keyentry *entry, const int k_len);

int
isComment(char *line);

char*
trim(char *in);
#endif
