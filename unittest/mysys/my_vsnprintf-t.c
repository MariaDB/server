/* Copyright (c) 2003, 2011, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_global.h>
#include <m_string.h>
#include <tap.h>

char buf[1024]; /* let's hope that's enough */

static void test_w_len(const char *res, size_t buflen, const char *fmt, ...)
{
  va_list args;
  size_t len;
  va_start(args,fmt);
  len= my_vsnprintf(buf, buflen, fmt, args);
  va_end(args);
  ok(strlen(res) == len && strcmp(buf, res) == 0, "\"%s\"", buf);
}

static void test1(const char *res, const char *fmt, ...)
{
  va_list args;
  size_t len;
  va_start(args,fmt);
  len= my_vsnprintf(buf, sizeof(buf)-1, fmt, args);
  va_end(args);
  ok(strlen(res) == len && strcmp(buf, res) == 0, "\"%s\"", buf);
}

static void test_many(const char **res, const char *fmt, ...)
{
  va_list args;
  size_t len;
  va_start(args,fmt);
  len= my_vsnprintf(buf, sizeof(buf)-1, fmt, args);
  va_end(args);

  for (; *res ; res++)
  {
    if (strlen(*res) == len && strcmp(buf, *res) == 0)
    {
      ok(1, "\"%s\"", buf);
      return;
    }
  }
  ok(0, "\"%s\"", buf);
}


int main(void)
{
  plan(48);

  test1("Constant string",
        "Constant string");

  test1("Format specifier s works",
        "Format specifier s %s", "works");
  test1("Format specifier b works (mysql extension)",
        "Format specifier b %.5b (mysql extension)", "works!!!");
  test1("Format specifier c !",
        "Format specifier c %c", '!');
  test1("Format specifier d 1",
        "Format specifier d %d", 1);
  test1("Format specifier i 1",
        "Format specifier i %i", 1);
  test1("Format specifier u 2",
        "Format specifier u %u", 2);
  test1("Format specifier o 375",
        "Format specifier o %o", 0375);
  test1("Format specifier x a",
        "Format specifier x %x", 10);
  test1("Format specifier X B",
        "Format specifier X %X", 11);
  test1("Format specifier p 0x5",
        "Format specifier p %p", 5);
  test1("Format specifier f 3.141593",
        "Format specifier f %f", 3.1415926);
  test1("Format specifier g 3.1416",
        "Format specifier g %g", 3.1415926);

  test1("Flag '-' is ignored <   1>",
        "Flag '-' is ignored <%-4d>", 1);
  test1("Flag '0' works <0006>",
        "Flag '0' works <%04d>", 6);

  test1("Width for strings <   x> <    y>",
        "Width for strings <%04s> <%5s>", "x", "y");

  test1("Negative width is ignored for strings <   x> <    y>",
        "Negative width is ignored for strings <%-4s> <%-5s>", "x", "y");

  test1("Precision works for strings <abcde>",
        "Precision works for strings <%.5s>", "abcdef!");
  test1("Precision works for strings <ab...>",
        "Precision works for strings <%.5T>", "abcdef!");

  test1("Flag '`' (backtick) works: `abcd` `op``q` (mysql extension)",
        "Flag '`' (backtick) works: %`s %`.4s (mysql extension)",
        "abcd", "op`qrst");

  test1("Flag '`' (backtick) works: `abcd` `op``q...` (mysql extension)",
        "Flag '`' (backtick) works: %`T %`.7T (mysql extension)",
        "abcd", "op`qrstuuuuuuuuu");

  test1("Flag '`' (backtick) works: `abcd` `.` (mysql extension)",
        "Flag '`' (backtick) works: %`T %`.1T (mysql extension)",
        "abcd", "op`qrstuuuuuuuuu");

  test1("Flag '`' (backtick) works: `abcd` `...` (mysql extension)",
        "Flag '`' (backtick) works: %`T %`.3T (mysql extension)",
        "abcd", "op`qrstuuuuuuuuu");

  test1("Flag '`' (backtick) works: `abcd` `op...` (mysql extension)",
        "Flag '`' (backtick) works: %`T %`.5T (mysql extension)",
        "abcd", "op`qrstuuuuuuuuu");

  test1("Flag '`' (backtick) works: `abcd` `op``...` (mysql extension)",
        "Flag '`' (backtick) works: %`T %`.6T (mysql extension)",
        "abcd", "op`qrstuuuuuuuuu");

  test1("Length modifiers work: 1 * -1 * 2 * 3",
        "Length modifiers work: %d * %ld * %lld * %zd", 1, -1L, 2LL, (size_t)3);

  test1("Length modifiers work: 1 * -1 * 2 * 3",
        "Length modifiers work: %i * %li * %lli * %zd", 1, -1L, 2LL, (size_t)3);

  test1("long long X: 123456789abcdef0",
        "long long X: %llx", 0x123456789abcdef0LL);

  test1("(null) pointer is fine",
        "%s pointer is fine", NULL);

  test1("Positional arguments work: on the dark side they are",
        "Positional arguments work: %3$s %1$s %2$s",
        "they", "are", "on the dark side");

  test1("Asterisk '*' as a width works: <    4>",
        "Asterisk '*' as a width works: <%*d>", 5, 4);

  test1("Asterisk '*' as a precision works: <qwerty>",
        "Asterisk '*' as a precision works: <%.*s>", 6, "qwertyuiop");

  test1("Asterisk '*' as a precision works: <qwe...>",
        "Asterisk '*' as a precision works: <%.*T>", 6, "qwertyuiop");

  test1("Positional arguments for a width: <    4>",
        "Positional arguments for a width: <%1$*2$d>", 4, 5);

  test1("Positional arguments for a precision: <qwerty>",
        "Positional arguments for a precision: <%1$.*2$s>", "qwertyuiop", 6);

  test1("Positional arguments for a precision: <qwe...>",
        "Positional arguments for a precision: <%1$.*2$T>", "qwertyuiop", 6);

  test1("Positional arguments and a width: <0000ab>",
        "Positional arguments and a width: <%1$06x>", 0xab);

  test1("Positional arguments octal: <7777>",
        "Positional arguments octal: <%1$o>", 07777);

  /* Can't use int arguments, as they may be different size from pointers */

  test1("Padding and %p <0x12> <0x034> <0x0000ab> <    0xcd>",
        "Padding and %%p <%04p> <%05p> <%08p> <%8p>",
        (void*) 0x12, (void*) 0x34, (void*) 0xab, (void*) 0xcd);

  test1("F with a width (ignored) and precision: <12.34568>",
        "F with a width (ignored) and precision: <%10.5f>", 12.3456789);
  test1("G with a width (ignored) and precision: <12.35>",
        "G with a width (ignored) and precision: <%10.5g>", 12.3456789);

  {
    /* Test that %M works */
    const char *results[]=
    {
      "Error 1 \"Operation not permitted\"",    /* Linux */
      "Error 1 \"Not owner\"",                  /* Solaris */
      NullS
    };
    test_many(results, "Error %M", 1);
  }

  test1("M with 0 error code: 0 \"Internal error/check (Not system error)\"",
        "M with 0 error code: %M", 0);

  test1("M with positional: 0 \"Internal error/check (Not system error)\"",
        "M with positional: %1$M", 0);

  test1("M with width: 0 \"Internal error...",
        "M with width: %.20M", 0);
  test1("M with width positional: 0 \"Internal error...",
        "M with width positional: %2$.*1$M", 20, 0);

  test_w_len("M small buf: 0 \"..",
         19, "M small buf: %M", 0);
  test_w_len("M small buf positional: 0 \"..",
         30, "M small buf positional: %1$M", 0);

  return exit_status();
}

