/* inih -- simple .INI file parser

SPDX-License-Identifier: BSD-3-Clause

Copyright (C) 2009-2025, Ben Hoyt

inih is released under the New BSD license (see LICENSE.txt). Go to the project
home page for more info:

https://github.com/benhoyt/inih

*/

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>
#include <ctype.h>
#include <string.h>

#include "ini.h"

#if !INI_USE_STACK
#if INI_CUSTOM_ALLOCATOR
#include <stddef.h>
void* ini_malloc(size_t size);
void ini_free(void* ptr);
void* ini_realloc(void* ptr, size_t size);
#else
#include <stdlib.h>
#define ini_malloc malloc
#define ini_free free
#define ini_realloc realloc
#endif
#endif

#define MAX_SECTION 50
#define MAX_NAME 50

/* Used by ini_parse_string() to keep track of string parsing state. */
typedef struct {
    const char* ptr;
    size_t num_left;
} ini_parse_string_ctx;

/* Strip whitespace chars off end of given string, in place. end must be a
   pointer to the NUL terminator at the end of the string. Return s. */
static char* ini_rstrip(char* s, char* end)
{
    while (end > s && isspace((unsigned char)(*--end)))
        *end = '\0';
    return s;
}

/* Return pointer to first non-whitespace char in given string. */
static char* ini_lskip(const char* s)
{
    while (*s && isspace((unsigned char)(*s)))
        s++;
    return (char*)s;
}

/* Return pointer to first char (of chars) or inline comment in given string,
   or pointer to NUL at end of string if neither found. Inline comment must
   be prefixed by a whitespace character to register as a comment. */
static char* ini_find_chars_or_comment(const char* s, const char* chars)
{
#if INI_ALLOW_INLINE_COMMENTS
    int was_space = 0;
    while (*s && (!chars || !strchr(chars, *s)) &&
           !(was_space && strchr(INI_INLINE_COMMENT_PREFIXES, *s))) {
        was_space = isspace((unsigned char)(*s));
        s++;
    }
#else
    while (*s && (!chars || !strchr(chars, *s))) {
        s++;
    }
#endif
    return (char*)s;
}

/* Similar to strncpy, but ensures dest (size bytes) is
   NUL-terminated, and doesn't pad with NULs. */
static char* ini_strncpy0(char* dest, const char* src, size_t size)
{
    /* Could use strncpy internally, but it causes gcc warnings (see issue #91) */
    size_t i;
    for (i = 0; i < size - 1 && src[i]; i++)
        dest[i] = src[i];
    dest[i] = '\0';
    return dest;
}

/* See documentation in header file. */
int ini_parse_stream(ini_reader reader, void* stream, ini_handler handler,
                     void* user)
{
    /* Uses a fair bit of stack (use heap instead if you need to) */
#if INI_USE_STACK
    char line[INI_MAX_LINE];
    size_t max_line = INI_MAX_LINE;
#else
    char* line;
    size_t max_line = INI_INITIAL_ALLOC;
#endif
#if INI_ALLOW_REALLOC && !INI_USE_STACK
    char* new_line;
#endif
    char section[MAX_SECTION] = "";
#if INI_ALLOW_MULTILINE
    char prev_name[MAX_NAME] = "";
#endif

    size_t offset;
    char* start;
    char* end;
    char* name;
    char* value;
    int lineno = 0;
    int error = 0;
    char abyss[16];  /* Used to consume input when a line is too long. */
    size_t abyss_len;

#if !INI_USE_STACK
    line = (char*)ini_malloc(INI_INITIAL_ALLOC);
    if (!line) {
        return -2;
    }
#endif

#if INI_HANDLER_LINENO
#define HANDLER(u, s, n, v) handler(u, s, n, v, lineno)
#else
#define HANDLER(u, s, n, v) handler(u, s, n, v)
#endif

    /* Scan through stream line by line */
    while (reader(line, (int)max_line, stream) != NULL) {
        offset = strlen(line);

#if INI_ALLOW_REALLOC && !INI_USE_STACK
        while (max_line < INI_MAX_LINE &&
               offset == max_line - 1 && line[offset - 1] != '\n') {
            max_line *= 2;
            if (max_line > INI_MAX_LINE)
                max_line = INI_MAX_LINE;
            new_line = ini_realloc(line, max_line);
            if (!new_line) {
                ini_free(line);
                return -2;
            }
            line = new_line;
            if (reader(line + offset, (int)(max_line - offset), stream) == NULL)
                break;
            offset += strlen(line + offset);
        }
#endif

        lineno++;

        /* If line exceeded INI_MAX_LINE bytes, discard till end of line. */
        if (offset == max_line - 1 && line[offset - 1] != '\n') {
            while (reader(abyss, sizeof(abyss), stream) != NULL) {
                if (!error)
                    error = lineno;
                abyss_len = strlen(abyss);
                if (abyss_len > 0 && abyss[abyss_len - 1] == '\n')
                    break;
            }
        }

        start = line;
#if INI_ALLOW_BOM
        if (lineno == 1 && (unsigned char)start[0] == 0xEF &&
                           (unsigned char)start[1] == 0xBB &&
                           (unsigned char)start[2] == 0xBF) {
            start += 3;
        }
#endif
        start = ini_rstrip(ini_lskip(start), line + offset);

        if (strchr(INI_START_COMMENT_PREFIXES, *start)) {
            /* Start-of-line comment */
        }
#if INI_ALLOW_MULTILINE
        else if (*prev_name && *start && start > line) {
#if INI_ALLOW_INLINE_COMMENTS
            end = ini_find_chars_or_comment(start, NULL);
            *end = '\0';
            ini_rstrip(start, end);
#endif
            /* Non-blank line with leading whitespace, treat as continuation
               of previous name's value (as per Python configparser). */
            if (!HANDLER(user, section, prev_name, start) && !error)
                error = lineno;
        }
#endif
        else if (*start == '[') {
            /* A "[section]" line */
            end = ini_find_chars_or_comment(start + 1, "]");
            if (*end == ']') {
                *end = '\0';
                ini_strncpy0(section, start + 1, sizeof(section));
#if INI_ALLOW_MULTILINE
                *prev_name = '\0';
#endif
#if INI_CALL_HANDLER_ON_NEW_SECTION
                if (!HANDLER(user, section, NULL, NULL) && !error)
                    error = lineno;
#endif
            }
            else if (!error) {
                /* No ']' found on section line */
                error = lineno;
            }
        }
        else if (*start) {
            /* Not a comment, must be a name[=:]value pair */
            end = ini_find_chars_or_comment(start, "=:");
            if (*end == '=' || *end == ':') {
                *end = '\0';
                name = ini_rstrip(start, end);
                value = end + 1;
#if INI_ALLOW_INLINE_COMMENTS
                end = ini_find_chars_or_comment(value, NULL);
                *end = '\0';
#endif
                value = ini_lskip(value);
                ini_rstrip(value, end);

#if INI_ALLOW_MULTILINE
                ini_strncpy0(prev_name, name, sizeof(prev_name));
#endif
                /* Valid name[=:]value pair found, call handler */
                if (!HANDLER(user, section, name, value) && !error)
                    error = lineno;
            }
            else {
                /* No '=' or ':' found on name[=:]value line */
#if INI_ALLOW_NO_VALUE
                *end = '\0';
                name = ini_rstrip(start, end);
                if (!HANDLER(user, section, name, NULL) && !error)
                    error = lineno;
#else
                if (!error)
                    error = lineno;
#endif
            }
        }

#if INI_STOP_ON_FIRST_ERROR
        if (error)
            break;
#endif
    }

#if !INI_USE_STACK
    ini_free(line);
#endif

    return error;
}

/* See documentation in header file. */
int ini_parse_file(FILE* file, ini_handler handler, void* user)
{
    return ini_parse_stream((ini_reader)fgets, file, handler, user);
}

/* See documentation in header file. */
int ini_parse(const char* filename, ini_handler handler, void* user)
{
    FILE* file;
    int error;

    file = fopen(filename, "r");
    if (!file)
        return -1;
    error = ini_parse_file(file, handler, user);
    fclose(file);
    return error;
}

/* An ini_reader function to read the next line from a string buffer. This
   is the fgets() equivalent used by ini_parse_string(). */
static char* ini_reader_string(char* str, int num, void* stream) {
    ini_parse_string_ctx* ctx = (ini_parse_string_ctx*)stream;
    const char* ctx_ptr = ctx->ptr;
    size_t ctx_num_left = ctx->num_left;
    char* strp = str;
    char c;

    if (ctx_num_left == 0 || num < 2)
        return NULL;

    while (num > 1 && ctx_num_left != 0) {
        c = *ctx_ptr++;
        ctx_num_left--;
        *strp++ = c;
        if (c == '\n')
            break;
        num--;
    }

    *strp = '\0';
    ctx->ptr = ctx_ptr;
    ctx->num_left = ctx_num_left;
    return str;
}

/* See documentation in header file. */
int ini_parse_string(const char* string, ini_handler handler, void* user) {
    return ini_parse_string_length(string, strlen(string), handler, user);
}

/* See documentation in header file. */
int ini_parse_string_length(const char* string, size_t length,
                            ini_handler handler, void* user) {
    ini_parse_string_ctx ctx;

    ctx.ptr = string;
    ctx.num_left = length;
    return ini_parse_stream((ini_reader)ini_reader_string, &ctx, handler,
                            user);
}
