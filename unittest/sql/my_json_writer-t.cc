/*
   Copyright (c) 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include <my_global.h>
#include <my_pthread.h>
#include <my_sys.h>
#include <stdio.h>
#include <tap.h>

/*
  Unit tests for class Json_writer.
*/

struct TABLE;
class Json_writer;


/* Several fake objects */
class Opt_trace
{
public:
  void enable_tracing_if_required() {}
  void disable_tracing_if_required() {}
  Json_writer *get_current_json() { return nullptr; }
};

class THD
{
public:
  Opt_trace opt_trace;
};

constexpr uint FAKE_SELECT_LEX_ID= UINT_MAX;

#define sql_print_error printf

#define JSON_WRITER_UNIT_TEST
#include "../sql/my_json_writer.h"
#include "../sql/my_json_writer.cc"

static bool json_output_eq(Json_writer &w, const char *expected)
{
  const String *s= w.output.get_string();
  size_t exp_len= strlen(expected);
  if (s->length() == exp_len && memcmp(s->ptr(), expected, exp_len) == 0)
    return true;
  diag("  Expected (%d bytes): [%s]", (int) exp_len, expected);
  diag("  Actual   (%d bytes): [%.*s]",
       (int) s->length(), (int) s->length(), s->ptr());
  return false;
}

static bool json_output_eq_len(Json_writer &w,
                               const char *expected,
                               size_t expected_len)
{
  const String *s= w.output.get_string();
  if (s->length() == expected_len &&
      memcmp(s->ptr(), expected, expected_len) == 0)
    return true;

  diag("  Expected length: %d bytes", (int) expected_len);
  diag("  Actual length  : %d bytes", (int) s->length());

  size_t common_len= s->length() < expected_len ? s->length() : expected_len;
  for (size_t i= 0; i < common_len; i++)
  {
    uchar actual= (uchar) s->ptr()[i];
    uchar exp= (uchar) expected[i];
    if (actual != exp)
    {
      diag("  First mismatch at byte %d: expected 0x%02x, actual 0x%02x",
           (int) i, (uint) exp, (uint) actual);
      break;
    }
  }
  return false;
}

int main(int args, char **argv)
{
  MY_INIT(argv[0]);

  plan(137);
  diag("Testing Json_writer checks");

  {
    Json_writer w;
    w.start_object();
    w.add_member("foo");
    w.end_object();
    ok(w.invalid_json, "Started a name but didn't add a value");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_ll(123678456);
    ok(w.invalid_json, "Unnamed LL value in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_ull(123);
    ok(w.invalid_json, "Unnamed ULL value in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_size(567890);
    ok(w.invalid_json, "Unnamed size value in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_double(123.567867);
    ok(w.invalid_json, "Unnamed double value in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_bool(true);
    ok(w.invalid_json, "Unnamed bool value in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_null();
    ok(w.invalid_json, "Unnamed null value in an object");
  }

  {
    Json_writer w;
    w.start_array();
    w.add_member("bebebe").add_ull(345);
    ok(w.invalid_json, "Named member in array");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_str("some string");
    ok(w.invalid_json, "Unnamed string (const char*) in an object");
  }

  {
    Json_writer w;
    w.start_object();
    CHARSET_INFO cs_info{};
    String str("another string", sizeof("another string"), &cs_info);
    w.add_str(str);
    ok(w.invalid_json, "Unnamed string (class String) in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.start_array();
    ok(!w.invalid_json, "Implicitly added member for the unnamed array"
                        " in an object");
  }

  {
    Json_writer w;
    w.start_object();
    w.start_object();
    ok(!w.invalid_json, "Implicitly added member for the unnamed object"
                        " in an object");
  }

  {
    Json_writer w;
    w.start_array();
    w.add_member("zzz");
    w.start_object();
    ok(w.invalid_json, "Named object in an array");
  }
  {
    Json_writer w;
    w.start_array();
    w.add_member("zzz");
    w.start_array();
    ok(w.invalid_json, "Named array in an array");
  }

  {
    Json_writer w;
    w.start_array();
    w.end_object();
    ok(w.invalid_json, "JSON object end of array");
  }

  {
    Json_writer w;
    w.start_object();
    w.end_array();
    ok(w.invalid_json, "JSON array end of object");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("name").start_object();
    w.add_member("name").add_ll(2);
    w.end_object();
    w.end_object();
    ok(!w.invalid_json, "Valid JSON: nested object member name is the same");
    ok(json_output_eq(w,
       "{\n"
       "  \"name\": {\n"
       "    \"name\": 2\n"
       "  }\n"
       "}"),
       "Nested same-name key output");
  }

  diag("Testing Json_writer output");

  {
    Json_writer w;
    w.start_object();
    w.end_object();
    ok(json_output_eq(w, "{}"), "Empty object");
    ok(!w.invalid_json, "Empty object is valid");
  }

  {
    Json_writer w;
    w.start_array();
    w.end_array();
    ok(json_output_eq(w, "[]"), "Empty array");
    ok(!w.invalid_json, "Empty array is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("key").add_str("hello");
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"key\": \"hello\"\n"
       "}"),
       "String value");
    ok(!w.invalid_json, "String value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("s").add_str("");
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"s\": \"\"\n"
       "}"),
       "Empty string value");
    ok(!w.invalid_json, "Empty string value is valid");
  }

  /* add_str does not escape: special chars are passed through verbatim */
  {
    Json_writer w;
    w.start_object();
    w.add_member("s").add_str("with\"quote");
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"s\": \"with\"quote\"\n"
       "}"),
       "String with embedded quote (no escaping)");
    ok(!w.invalid_json,
       "String with embedded quote remains valid for this writer");
  }

  {
    Json_writer w;
    String latin1_str("\xe9\xf6\xfc", 3, &my_charset_latin1);
    w.start_object();
    w.add_member("s").add_str(latin1_str);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"s\": \"\xe9\xf6\xfc\"\n"
       "}"),
       "Latin-1 encoded string bytes pass through unchanged");
    ok(!w.invalid_json,
       "Latin-1 encoded string bytes pass through unchanged and stay valid");
  }

  {
    Json_writer w;
    /* 4-byte UTF-8 sequence (U+1F600) */
    String utf8mb4_str("\xf0\x9f\x98\x80", 4, &my_charset_utf8mb4_general_ci);
    w.start_object();
    w.add_member("s").add_str(utf8mb4_str);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"s\": \"\xf0\x9f\x98\x80\"\n"
       "}"),
       "UTF-8 mb4 encoded string bytes pass through unchanged");
    ok(!w.invalid_json,
       "UTF-8 mb4 encoded string bytes pass through unchanged and stay valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("num").add_ll(42);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"num\": 42\n"
       "}"),
       "Positive longlong value");
    ok(!w.invalid_json, "Positive longlong value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("num").add_ll(-100);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"num\": -100\n"
       "}"),
       "Negative longlong value");
    ok(!w.invalid_json, "Negative longlong value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("num").add_ll(LLONG_MAX);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"num\": 9223372036854775807\n"
       "}"),
       "LLONG_MAX");
    ok(!w.invalid_json, "LLONG_MAX is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("num").add_ll(LLONG_MIN);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"num\": -9223372036854775808\n"
       "}"),
       "LLONG_MIN");
    ok(!w.invalid_json, "LLONG_MIN is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("num").add_ull(ULLONG_MAX);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"num\": 18446744073709551615\n"
       "}"),
       "ULLONG_MAX");
    ok(!w.invalid_json, "ULLONG_MAX is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("flag").add_bool(true);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"flag\": true\n"
       "}"),
       "Bool true");
    ok(!w.invalid_json, "Bool true is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("val").add_null();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"val\": null\n"
       "}"),
       "Null value");
    ok(!w.invalid_json, "Null value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("pi").add_double(3.14);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"pi\": 3.14\n"
       "}"),
       "Double value");
    ok(!w.invalid_json, "Double value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("d").add_double(0.0);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"d\": 0\n"
       "}"),
       "Double zero");
    ok(!w.invalid_json, "Double zero is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("d").add_double(-0.0);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"d\": -0\n"
       "}"),
       "Double negative zero");
    ok(!w.invalid_json, "Double negative zero is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("d").add_double(1e-10);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"d\": 1e-10\n"
       "}"),
       "Double very small value");
    ok(!w.invalid_json, "Double very small value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("d").add_double(1e+15);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"d\": 1e15\n"
       "}"),
       "Double very large value");
    ok(!w.invalid_json, "Double very large value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("a").add_str("hello");
    w.add_member("b").add_ll(42);
    w.add_member("c").add_bool(true);
    w.add_member("d").add_null();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"a\": \"hello\",\n"
       "  \"b\": 42,\n"
       "  \"c\": true,\n"
       "  \"d\": null\n"
       "}"),
       "Object with multiple typed members");
    ok(!w.invalid_json, "Multiple members is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("key with spaces").add_ll(1);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"key with spaces\": 1\n"
       "}"),
       "Key with spaces");
    ok(!w.invalid_json, "Key with spaces is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("cl\xe9").add_ll(1);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"cl\xe9\": 1\n"
       "}"),
       "Key with Latin-1 byte");
    ok(!w.invalid_json, "Key with Latin-1 byte is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("has\"quote").add_ll(1);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"has\"quote\": 1\n"
       "}"),
       "Key with embedded quote (no escaping)");
    ok(!w.invalid_json,
       "Key with embedded quote remains valid for this writer");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("").add_ll(1);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"\": 1\n"
       "}"),
       "Empty key name");
    ok(!w.invalid_json, "Empty key name is valid");
  }

  {
    Json_writer w;
    const char key[]= "prefix";
    w.start_object();
    w.add_member(key, 3).add_ll(7);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"pre\": 7\n"
       "}"),
       "add_member(name, len) uses explicit key length");
    ok(!w.invalid_json, "add_member(name, len) remains valid");
  }

  {
    Json_writer w;
    const char key_with_nul[]= { 'k', '\0', 'y' };
    const char expected[]=
       "{\n"
       "  \"k\0y\": 1\n"
       "}";
    w.start_object();
    w.add_member(key_with_nul, sizeof(key_with_nul)).add_ll(1);
    w.end_object();
    ok(json_output_eq_len(w, expected, sizeof(expected) - 1),
       "add_member(name, len) preserves embedded NUL in key");
    ok(!w.invalid_json, "Embedded-NUL key does not mark writer invalid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("name").add_ll(1);
    w.add_member("name").add_ll(2);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"name\": 1,\n"
       "  \"name\": 2\n"
       "}"),
       "Duplicate key output");
    ok(w.invalid_json, "Duplicate key is invalid JSON");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("arr");
    w.start_array();
    w.add_str("a");
    w.add_str("b");
    w.add_str("c");
    w.end_array();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"arr\": [\"a\", \"b\", \"c\"]\n"
       "}"),
       "Named string array: single-line format");
    ok(!w.invalid_json, "Named string array is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("one");
    w.start_array();
    w.add_str("only");
    w.end_array();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"one\": [\"only\"]\n"
       "}"),
       "Named array with single element");
    ok(!w.invalid_json, "Named array with single element is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("empty");
    w.start_array();
    w.end_array();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"empty\": []\n"
       "}"),
       "Empty named array");
    ok(!w.invalid_json, "Empty named array is valid");
  }

  {
    Json_writer w;
    const char value_with_nul[]= { 'a', '\0', 'b' };
    const char expected[]=
       "{\n"
       "  \"arr\": [\n"
       "    \"a\0b\"\n"
       "  ]\n"
       "}";
    w.start_object();
    w.add_member("arr");
    w.start_array();
    w.add_str(value_with_nul, sizeof(value_with_nul));
    w.end_array();
    w.end_object();
    ok(json_output_eq_len(w, expected, sizeof(expected) - 1),
       "Named array element with embedded NUL is preserved");
    ok(!w.invalid_json, "Embedded-NUL array element keeps writer valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("arr");
    w.start_array();
    w.add_str("first");
    w.start_object();
    w.add_member("k").add_ll(1);
    w.end_object();
    w.end_array();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"arr\": [\n"
       "    \"first\",\n"
       "    {\n"
       "      \"k\": 1\n"
       "    }\n"
       "  ]\n"
       "}"),
       "Single-line helper flushes buffered values before nested object");
    ok(!w.invalid_json, "Nested object in named array stays valid");
  }

  {
    Json_writer w;
    char long_val[91];
    memset(long_val, 'b', 90);
    long_val[90]= 0;
    w.start_object();
    w.add_member("arr");
    w.start_array();
    w.add_str("a");
    w.add_str(long_val);
    w.end_array();
    w.end_object();

    String expected;
    expected.append(STRING_WITH_LEN("{\n"
                                    "  \"arr\": [\n"
                                    "    \"a\",\n"
                                    "    \""));
    expected.append(long_val, 90);
    expected.append(STRING_WITH_LEN("\"\n"
                                    "  ]\n"
                                    "}"));
    ok(json_output_eq_len(w, expected.ptr(), expected.length()),
       "Single-line helper falls back when line length exceeds threshold");
    ok(!w.invalid_json, "Long element fallback output stays valid");
  }

  {
    Json_writer w;
    w.start_array();
    w.add_str("x");
    w.add_str("y");
    w.end_array();
    ok(json_output_eq(w,
       "[\n"
       "  \"x\",\n"
       "  \"y\"\n"
       "]"),
       "Root array: multi-line format");
    ok(!w.invalid_json, "Root array is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("outer").start_object();
    w.add_member("inner").add_str("val");
    w.end_object();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"outer\": {\n"
       "    \"inner\": \"val\"\n"
       "  }\n"
       "}"),
       "Nested objects");
    ok(!w.invalid_json, "Nested objects valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("items");
    w.start_array();
    w.start_object();
    w.add_member("x").add_ll(10);
    w.end_object();
    w.start_object();
    w.add_member("x").add_ll(20);
    w.end_object();
    w.end_array();
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"items\": [\n"
       "    {\n"
       "      \"x\": 10\n"
       "    },\n"
       "    {\n"
       "      \"x\": 20\n"
       "    }\n"
       "  ]\n"
       "}"),
       "Object with array of objects");
    ok(!w.invalid_json, "Object with array of objects valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("name").add_str("test");
    w.add_member("enabled").add_bool(true);
    w.add_member("config").start_object();
    w.add_member("timeout").add_ll(30);
    w.add_member("tags");
    w.start_array();
    w.add_str("fast");
    w.add_str("reliable");
    w.end_array();
    w.end_object();
    w.add_member("count").add_ll(0);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"name\": \"test\",\n"
       "  \"enabled\": true,\n"
       "  \"config\": {\n"
       "    \"timeout\": 30,\n"
       "    \"tags\": [\"fast\", \"reliable\"]\n"
       "  },\n"
       "  \"count\": 0\n"
       "}"),
       "Complex mixed structure");
    ok(!w.invalid_json, "Complex structure valid");
  }

  diag("Testing RAII wrappers");

  {
    Json_writer w;
    {
      Json_writer_object obj(&w);
      w.add_member("key").add_str("val");
    }
    ok(json_output_eq(w,
       "{\n"
       "  \"key\": \"val\"\n"
       "}"),
       "Json_writer_object auto-closes on destruction");
    ok(!w.invalid_json, "RAII object valid");
  }

  {
    Json_writer w;
    {
      Json_writer_object obj(&w);
      obj.add("name", "test");
      obj.add("count", (longlong) 7);
      obj.add("flag", true);
    }
    ok(json_output_eq(w,
       "{\n"
       "  \"name\": \"test\",\n"
       "  \"count\": 7,\n"
       "  \"flag\": true\n"
       "}"),
       "Json_writer_object fluent add()");
    ok(!w.invalid_json, "Fluent add valid");
  }

  {
    Json_writer w;
    {
      Json_writer_object obj(&w);
      obj.add("u", (ulonglong) ULLONG_MAX);
    }
    ok(json_output_eq(w,
       "{\n"
       "  \"u\": 18446744073709551615\n"
       "}"),
       "Json_writer_object add(ulonglong) preserves unsigned range");
    ok(!w.invalid_json, "Json_writer_object add(ulonglong) stays valid");
  }

  {
    Json_writer w;
    Json_writer_object obj(&w);
    obj.add("a", (longlong) 1);
    obj.end();
    ok(json_output_eq(w,
       "{\n"
       "  \"a\": 1\n"
       "}"),
       "Json_writer_object explicit end()");
    ok(!w.invalid_json, "Explicit end valid");
  }

  {
    Json_writer w;
    {
      Json_writer_array arr(&w);
      arr.add("x");
      arr.end();
    }
    ok(json_output_eq(w,
       "[\n"
       "  \"x\"\n"
       "]"),
       "Json_writer_array explicit end()");
    ok(!w.invalid_json, "Json_writer_array explicit end stays valid");
  }

  {
    Json_writer w;
    {
      Json_writer_array arr(&w);
      arr.add("foo");
      arr.add("bar");
    }
    ok(json_output_eq(w,
       "[\n"
       "  \"foo\",\n"
       "  \"bar\"\n"
       "]"),
       "Json_writer_array auto-closes on destruction");
    ok(!w.invalid_json, "RAII array valid");
  }

  {
    Json_writer w;
    {
      Json_writer_array arr(&w);
      arr.add((ulonglong) 42);
    }
    ok(json_output_eq(w,
       "[\n"
       "  42\n"
       "]"),
       "RAII array add(ulonglong) output");
    ok(!w.invalid_json, "RAII array add(ulonglong) valid");
  }

  {
    Json_writer w;
    {
      Json_writer_object outer(&w);
      outer.add("level", "outer");
      {
        Json_writer_object inner(&w, "nested");
        inner.add("level", "inner");
      }
    }
    ok(json_output_eq(w,
       "{\n"
       "  \"level\": \"outer\",\n"
       "  \"nested\": {\n"
       "    \"level\": \"inner\"\n"
       "  }\n"
       "}"),
       "Nested RAII Json_writer_objects");
    ok(!w.invalid_json, "Nested RAII valid");
  }

  {
    Json_writer w;
    {
      Json_writer_object obj(&w);
      obj.add("name", "test");
      {
        Json_writer_array arr(&w, "values");
        arr.add("a");
        arr.add("b");
      }
    }
    ok(json_output_eq(w,
       "{\n"
       "  \"name\": \"test\",\n"
       "  \"values\": [\"a\", \"b\"]\n"
       "}"),
       "RAII array inside RAII object");
    ok(!w.invalid_json, "RAII array in object valid");
  }

  {
    Json_writer w;
    {
      Json_writer_object obj(&w);
      obj.add("partial", "hello world", 5);
    }
    ok(json_output_eq(w,
       "{\n"
       "  \"partial\": \"hello\"\n"
       "}"),
       "add(name, value, num_bytes) honors length");
    ok(!w.invalid_json,
       "add(name, value, num_bytes) produces valid JSON");
  }

  diag("Testing String_with_limit");

  {
    String_with_limit sl;
    sl.append("hello");
    sl.append(" world");
    ok(sl.length() == 11, "Normal append: length == 11");
    ok(sl.get_truncated_bytes() == 0, "Normal append: no truncation");
    const String *s= sl.get_string();
    ok(s->length() == 11 && memcmp(s->ptr(), "hello world", 11) == 0,
       "Normal append: content correct");
  }

  {
    String_with_limit sl;
    sl.set_size_limit(5);
    sl.append("hello world");
    ok(sl.length() == 5, "Truncation: length capped at 5");
    ok(sl.get_truncated_bytes() == 6, "Truncation: 6 bytes truncated");
    const String *s= sl.get_string();
    ok(s->length() == 5 && memcmp(s->ptr(), "hello", 5) == 0,
       "Truncation: content is prefix");
  }

  {
    String_with_limit sl;
    sl.set_size_limit(5);
    sl.append("hi");
    sl.append(" there buddy");
    ok(sl.length() == 5, "Multi-append truncation: length == 5");
    ok(sl.get_truncated_bytes() == 9, "Multi-append truncation: 9 truncated");
    const String *s= sl.get_string();
    ok(s->length() == 5 && memcmp(s->ptr(), "hi th", 5) == 0,
       "Multi-append truncation: correct partial content");
  }

  {
    String_with_limit sl;
    sl.set_size_limit(3);
    sl.append("abc");
    sl.append("def");
    ok(sl.length() == 3, "At-limit overflow: length == 3");
    ok(sl.get_truncated_bytes() == 3, "At-limit overflow: 3 truncated");
  }

  {
    String_with_limit sl;
    sl.set_size_limit(3);
    sl.append('a');
    sl.append('b');
    sl.append('c');
    sl.append('d');
    ok(sl.length() == 3, "Char append truncation: length == 3");
    ok(sl.get_truncated_bytes() == 1, "Char append truncation: 1 truncated");
  }

  diag("Testing Json_writer size limit");

  {
    Json_writer full;
    full.start_object();
    full.add_member("longkey").add_str("longvalue");
    full.end_object();

    Json_writer limited;
    limited.set_size_limit(10);
    limited.start_object();
    limited.add_member("longkey").add_str("longvalue");
    limited.end_object();

    ok(limited.output.length() == 10, "Size limit enforced on Json_writer");
    ok(limited.get_truncated_bytes() ==
       full.output.length() - limited.output.length(),
       "Truncated byte count is exact");
  }

  diag("Testing add_size formatting");

  {
    Json_writer w;
    w.start_object();
    w.add_member("size").add_size(512);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"size\": \"512\"\n"
       "}"),
       "add_size: bytes (< 1024)");
    ok(!w.invalid_json, "add_size: bytes (< 1024) is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("size").add_size(2048);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"size\": \"2KiB\"\n"
       "}"),
       "add_size: KiB value");
    ok(!w.invalid_json, "add_size: KiB value is valid");
  }

  {
    Json_writer w;
    w.start_object();
    w.add_member("size").add_size((longlong) 32 * 1024 * 1024);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"size\": \"32MiB\"\n"
       "}"),
       "add_size: MiB value");
    ok(!w.invalid_json, "add_size: MiB value is valid");
  }

  {
    Json_writer w;
    const longlong mb16= (longlong) 16 * 1024 * 1024;
    w.start_object();
    w.add_member("below_kb").add_size(1023);
    w.add_member("at_kb").add_size(1024);
    w.add_member("below_16mb").add_size(mb16 - 1);
    w.add_member("at_16mb").add_size(mb16);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"below_kb\": \"1023\",\n"
       "  \"at_kb\": \"1KiB\",\n"
       "  \"below_16mb\": \"16383KiB\",\n"
       "  \"at_16mb\": \"16MiB\"\n"
       "}"),
       "add_size boundary formatting");
    ok(!w.invalid_json, "add_size boundary formatting is valid");
  }

  diag("Testing add_str overloads");

  {
    Json_writer w;
    String s;
    s.append("hello", 5);
    w.start_object();
    w.add_member("msg").add_str(s);
    w.end_object();
    ok(json_output_eq(w,
       "{\n"
       "  \"msg\": \"hello\"\n"
       "}"),
       "add_str with String object");
    ok(!w.invalid_json, "add_str with String object is valid");
  }

  diag("Done");

  my_end(MY_CHECK_ERROR);
  return exit_status();
}
