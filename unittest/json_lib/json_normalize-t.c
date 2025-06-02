/* Copyright (c) 2021 Eric Herman and MariaDB Foundation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#include "my_config.h"
#include "config.h"
#include <tap.h>
#include <my_global.h>
#include <json_lib.h>


static void
check_json_normalize(const char *in, const char *expected, json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  int err;
  DYNAMIC_STRING result;
  MEM_ROOT current_mem_root;

  CHARSET_INFO *cs= &my_charset_utf8mb4_general_ci;

  init_dynamic_string(&result, NULL, 0, 0);

  err= json_normalize(&result, in, strlen(in), cs, &current_mem_root, je, stack);

  ok(err == 0, "normalize err: %d", err);

  ok(strcmp(expected, result.str) == 0,
    "from '%s'\n expect: '%s'\n actual: '%s'",
    in, expected, result.str);

  dynstr_free(&result);
}


static void
test_json_normalize_invalid(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  DYNAMIC_STRING result;
  MEM_ROOT current_mem_root;

  CHARSET_INFO *cs= &my_charset_utf8mb4_general_ci;

  init_alloc_root(PSI_INSTRUMENT_MEM, &current_mem_root,
                  BLOCK_SIZE_JSON_DYN_ARRAY, 0, MYF(0));

  init_dynamic_string(&result, NULL, 0, 0);
  ok(json_normalize(&result, STRING_WITH_LEN(""), cs, &current_mem_root, je, stack) != 0,
     "expected normalized error");
  dynstr_free(&result);

  init_dynamic_string(&result, NULL, 0, 0);
  ok(json_normalize(&result, STRING_WITH_LEN("["), cs, &current_mem_root, je, stack) != 0,
     "expected normalized error");
  dynstr_free(&result);

  init_dynamic_string(&result, NULL, 0, 0);
  ok(json_normalize(&result, STRING_WITH_LEN("}"), cs, &current_mem_root, je, stack) != 0,
     "expected normalized error");
  dynstr_free(&result);

  init_dynamic_string(&result, NULL, 0, 0);
  ok(json_normalize(&result, NULL, 0, cs, &current_mem_root, je, stack) != 0,
     "expected normalized error");
  dynstr_free(&result);
}


static void
test_json_normalize_single_kv(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  const char *in= ""
  "{\n"
  "  \"foo\": \"value\"\n"
  "}\n";

  const char *expected= "{\"foo\":\"value\"}";
  check_json_normalize(in, expected, je, stack);
}


static void
test_json_normalize_multi_kv(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  const char *in= ""
  "{\n"
  "  \"bar\": \"baz\",\n"
  "  \"foo\": \"value\"\n"
  "}\n";

  const char *expected= "{\"bar\":\"baz\",\"foo\":\"value\"}";
  check_json_normalize(in, expected, je, stack);
}


static void
test_json_normalize_array(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  const char *in= "[ \"a\", \"b\", true, false, null ]";
  const char *expected= "[\"a\",\"b\",true,false,null]";
  check_json_normalize(in, expected, je, stack);
}


static void
test_json_normalize_values(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  check_json_normalize("\"foo\"", "\"foo\"", je, stack);
  check_json_normalize("true", "true", je, stack);
  check_json_normalize("false", "false", je, stack);
  check_json_normalize("null", "null", je, stack);
  check_json_normalize("\"\"", "\"\"", je, stack);
  check_json_normalize("{}", "{}", je, stack);
  check_json_normalize("[]", "[]", je, stack);
  check_json_normalize("5", "5.0E0", je, stack);
  check_json_normalize("5.1", "5.1E0", je, stack);
  check_json_normalize("-5.1", "-5.1E0", je, stack);
  check_json_normalize("12345.67890", "1.23456789E4", je, stack);
  check_json_normalize("2.99792458e8", "2.99792458E8", je, stack);
  check_json_normalize("6.02214076e23", "6.02214076E23", je, stack);
  check_json_normalize("6.62607015e-34", "6.62607015E-34", je, stack);
  check_json_normalize("-6.62607015e-34", "-6.62607015E-34", je, stack);
}


static void
test_json_normalize_nested_objects(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  const char *in = ""
  "{\n"
  "  \"wiz\": {\n"
  "\t\t\"bang\": \"a\",\n\t\t\"alpha\": false\n\t},\n"
  "  \"foo\": {\"value\":true}\n"
  "}";

  const char *expected= "{\"foo\":{\"value\":true},"
                        "\"wiz\":{\"alpha\":false,\"bang\":\"a\"}}";
  check_json_normalize(in, expected, je, stack);
}


static void
test_json_normalize_nested_arrays(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  const char *in = ""
  "[\n"
  "  \"wiz\",\n"
  " [\"bang\", \t\t\"alpha\"\t]\n"
  "]";

  const char *expected= "[\"wiz\",[\"bang\",\"alpha\"]]";
  check_json_normalize(in, expected, je, stack);
}


static void
test_json_normalize_nested_deep(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  const char *in = ""
  "{\n"
  "  \"foo\": \"value\",\n"
  "  \"wiz\": [true, false, {\n"
  "\t\t\"bang\": \"a\",\n\t\t\"alpha\": 12345.67890\n\t},\n  \"string\",\n"
  "\t{ \"b\": \"one\", \"a\": \"two\", \"c\": \"three\"}, false,\n"
  "\t\t[-1.20, \"w\", \"x\"]],\n"
  "  \"bar\": \"value2\"\n"
  "}\n";

  const char *expected= ""
  "{"
    "\"bar\":\"value2\","
    "\"foo\":\"value\","
    "\"wiz\":["
               "true,false,"
               "{\"alpha\":1.23456789E4,\"bang\":\"a\"},"
               "\"string\","
               "{\"a\":\"two\",\"b\":\"one\",\"c\":\"three\"},"
               "false,"
               "[-1.2E0,\"w\",\"x\"]"
            "]"
  "}";
  check_json_normalize(in, expected, je, stack);
}


/* a "friend" function */
int
json_normalize_number(DYNAMIC_STRING *out, const char *str, size_t str_len);


static void
test_json_normalize_non_utf8(json_engine_t *je, MEM_ROOT_DYNAMIC_ARRAY *stack)
{
  int err;
  const char utf8[]= { 0x22, 0xC3, 0x8A, 0x22, 0x00 };
  const char latin[] = { 0x22, 0xCA, 0x22, 0x00 };
  DYNAMIC_STRING result;
  MEM_ROOT current_mem_root;
  CHARSET_INFO *cs_utf8= &my_charset_utf8mb4_bin;
  CHARSET_INFO *cs_latin= &my_charset_latin1;

  init_dynamic_string(&result, NULL, 0, 0);
  init_alloc_root(PSI_INSTRUMENT_MEM, &current_mem_root,
                  BLOCK_SIZE_JSON_DYN_ARRAY, 0, MYF(0));
  err= json_normalize(&result, utf8, strlen(utf8), cs_utf8, &current_mem_root, je, stack);
  ok(err == 0, "normalize err?");
  ok((strcmp(utf8, result.str) == 0), "utf8 round trip");
  dynstr_free(&result);

  init_dynamic_string(&result, NULL, 0, 0);
  err= json_normalize(&result, latin, strlen(latin), cs_latin, &current_mem_root, je, stack);
  ok(err == 0, "normalize err?");
  ok((strcmp(utf8, result.str) == 0), "latin to utf8 round trip");
  dynstr_free(&result);
}


void
check_number_normalize(const char *in, const char *expected)
{
  int err;
  DYNAMIC_STRING buf;

  init_dynamic_string(&buf, NULL, 0, 0);

  err= json_normalize_number(&buf, in, strlen(in));
  ok(err == 0, "normalize number err: %d", err);

  ok(strcmp(buf.str, expected) == 0,
    "    from: %s\n"
    "expected: %s\n"
    "  actual: %s\n",
    in,
    expected,
    buf.str);

  dynstr_free(&buf);
}


int
main(int argc, char** argv)
{
  MEM_ROOT current_mem_root;
  MEM_ROOT_DYNAMIC_ARRAY stack;
  json_engine_t je;

  init_alloc_root(PSI_INSTRUMENT_MEM, &current_mem_root,
                    BLOCK_SIZE_JSON_DYN_ARRAY, 0, MYF(0));
  mem_root_dynamic_array_init(&current_mem_root, PSI_INSTRUMENT_MEM,
                              &je.stack,
                              sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  mem_root_dynamic_array_init(&current_mem_root, PSI_INSTRUMENT_MEM,
                              &stack,
                              sizeof(struct json_norm_value*), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  plan(88);
  diag("Testing json_normalization.");

  check_number_normalize("0", "0.0E0");
  check_number_normalize("-0.0", "0.0E0");
  check_number_normalize("0E100", "0.0E0");
  check_number_normalize("0.000000E100", "0.0E0");
  check_number_normalize("-0E100", "0.0E0");
  check_number_normalize("-0.000E100", "0.0E0");
  check_number_normalize("1", "1.0E0");
  check_number_normalize("-1", "-1.0E0");
  check_number_normalize("36", "3.6E1");
  check_number_normalize("37.000", "3.7E1");
  check_number_normalize("3.000", "3.0E0");
  check_number_normalize("0.00012345", "1.2345E-4");
  check_number_normalize("32.14e234", "3.214E235");
  check_number_normalize("0.00357e-23", "3.57E-26");
  check_number_normalize("0.00357e23", "3.57E20");
  check_number_normalize("123.456e10", "1.23456E12");
  check_number_normalize("123.456e-9", "1.23456E-7");
  check_number_normalize("0000123.456000000e-9", "1.23456E-7");
  check_number_normalize("0000123.456000000e+9", "1.23456E11");

  test_json_normalize_invalid(&je, &stack);
  test_json_normalize_values(&je, &stack);
  test_json_normalize_single_kv(&je, &stack);
  test_json_normalize_multi_kv(&je, &stack);
  test_json_normalize_array(&je, &stack);
  test_json_normalize_nested_objects(&je, &stack);
  test_json_normalize_nested_arrays(&je, &stack);
  test_json_normalize_nested_deep(&je, &stack);
  test_json_normalize_non_utf8(&je, &stack);

    free_root(&current_mem_root, MYF(0));

  my_end(MY_CHECK_ERROR);
  return exit_status();
}
