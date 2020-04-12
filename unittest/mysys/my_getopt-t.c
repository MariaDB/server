/* Copyright (c) 2015, MariaDB Corporation

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

   1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   2. Redistributions in binary form must the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.

   THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND ANY
   EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
   IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
   OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
   OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
   SUCH DAMAGE.
*/

#include <my_global.h>
#include <my_getopt.h>
#include <mysys_err.h>
#include <stdarg.h>
#include <tap.h>

ulonglong opt_ull;
ulong opt_ul;
int arg_c, res;
char **arg_v, *arg_s[100];

ulong mopts_num;
char *mopts_str;
my_bool mopts_bool;
static struct my_option mopts_options[]=
{
  {"str", 0,
    "Something numeric.",
    &mopts_str, &mopts_str, 0, GET_STR,
    REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"bool", 0,
    "Something true or false",
    &mopts_bool, &mopts_bool, 0, GET_BOOL,
    OPT_ARG, FALSE, 0, 0, 0, 0, 0},
  {"num", 0,
   "Something numeric.",
   &mopts_num, &mopts_num, 0, GET_ULONG,
   REQUIRED_ARG, 1000000L, 1, ULONG_MAX, 0, 2, 0},
  {"ull", 0, "ull", &opt_ull, &opt_ull,
   0, GET_ULL, REQUIRED_ARG, 1, 0, ~0ULL, 0, 0, 0},
  {"ul", 0, "ul", &opt_ul, &opt_ul,
   0, GET_ULONG, REQUIRED_ARG, 1, 0, 0xFFFFFFFF, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

my_bool dummy_get_one_option(const struct my_option *opt __attribute__((unused)),
                             const char *argument __attribute__((unused)),
                             const char *filename __attribute__((unused)))
{
  return FALSE;
}

void run(const char *arg, ...)
{
  va_list ap;
  va_start(ap, arg);
  arg_v= arg_s;
  *arg_v++= (char*)"<skipped>";
  while (arg)
  {
    *arg_v++= (char*)arg;
    arg= va_arg(ap, char*);
  }
  va_end(ap);
  arg_c= (int)(arg_v - arg_s);
  arg_v= arg_s;
  res= handle_options(&arg_c, &arg_v, mopts_options, &dummy_get_one_option);
}

int mopts1_argc= 4;
const char *mopts1_argv[]= {"mopts1", "--num=123", "--str=str", "--bool"};
void test_mopts1()
{
  int rc;
  char **av= (char **)mopts1_argv;

  rc= handle_options(&mopts1_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts1 call");
  ok( (mopts_num == 122), "%s", "test_mopts1 num");
  ok( (strncmp(mopts_str, "str", 4) == 0), "%s", "test_mopts1 str");
  ok( (mopts_bool == 1), "%s", "test_mopts1 bool");
}

int mopts2_argc= 4;
const char *mopts2_argv[]= {"mopts2", "--num=123", "--num=124", "--bool=0"};
void test_mopts2()
{
  int rc;
  char **av= (char **)mopts2_argv;

  rc= handle_options(&mopts2_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts2 call");
  ok( (mopts_num == 124), "%s", "test_mopts2 num");
  ok( (strncmp(mopts_str, "ddd", 4) == 0), "%s", "test_mopts2 str");
  ok( (mopts_bool == 0), "%s", "test_mopts2 bool");
}

int mopts3_argc= 4;
const char *mopts3_argv[]= {"mopts3", "--loose-foo", "--loose-loose-foo", "--enable-bool"};
void test_mopts3()
{
  int rc;
  char **av= (char **)mopts3_argv;

  rc= handle_options(&mopts3_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts3 call");
  ok( (mopts_num == 1000000L), "%s", "test_mopts3 num");
  ok( (strncmp(mopts_str, "ddd", 4) == 0), "%s", "test_mopts3 str");
  ok( (mopts_bool == 1), "%s", "test_mopts3 bool");
}

int mopts4_argc= 3;
const char *mopts4_argv[]= {"mopts4", "--loose-str=aa", "--skip-bool"};
void test_mopts4()
{
  int rc;
  char **av= (char **)mopts4_argv;

  rc= handle_options(&mopts4_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts4 call");
  ok( (mopts_num == 1000000L), "%s", "test_mopts4 num");
  ok( (strncmp(mopts_str, "aa", 3) == 0), "%s", "test_mopts4 str");
  ok( (mopts_bool == 0), "%s", "test_mopts4 bool");
}

int mopts5_argc= 2;
const char *mopts5_argv[]= {"mopts5", "--loose-skip-bool"};
void test_mopts5()
{
  int rc;
  char **av= (char **)mopts5_argv;

  rc= handle_options(&mopts5_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts5 call");
  ok( (mopts_num == 1000000L), "%s", "test_mopts5 num");
  ok( (strncmp(mopts_str, "ddd", 4) == 0), "%s", "test_mopts5 str");
  ok( (mopts_bool == 0), "%s", "test_mopts5 bool");
}

int mopts6_argc= 2;
const char *mopts6_argv[]= {"mopts6", "--loose-skip-skip-bool"};
void test_mopts6()
{
  int rc;
  char **av= (char **)mopts6_argv;

  rc= handle_options(&mopts6_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts6 call");
  ok( (mopts_num == 1000000L), "%s", "test_mopts6 num");
  ok( (strncmp(mopts_str, "ddd", 4) == 0), "%s", "test_mopts6 str");
  ok( (mopts_bool == 0), "%s", "test_mopts6 bool");
}

int mopts7_argc= 2;
const char *mopts7_argv[]= {"mopts7", "--loose-disable-skip-bool"};
void test_mopts7()
{
  int rc;
  char **av= (char **)mopts7_argv;

  rc= handle_options(&mopts7_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts7 call");
  ok( (mopts_num == 1000000L), "%s", "test_mopts7 num");
  ok( (strncmp(mopts_str, "ddd", 4) == 0), "%s", "test_mopts7 str");
  ok( (mopts_bool == 0), "%s", "test_mopts7 bool");
}

int mopts8_argc= 2;
const char *mopts8_argv[]= {"mopts8", "--loose-disable-enable-bool"};
void test_mopts8()
{
  int rc;
  char **av= (char **)mopts8_argv;

  rc= handle_options(&mopts8_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_mopts8 call");
  ok( (mopts_num == 1000000L), "%s", "test_mopts7 num");
  ok( (strncmp(mopts_str, "ddd", 4) == 0), "%s", "test_mopts7 str");
  ok( (mopts_bool == 1), "%s", "test_mopts7 bool");
}

int mopts9_argc= 2;
const char *mopts9_argv[]= {"mopts9", "--foo"};
void test_mopts9()
{
  int rc;
  char **av= (char **)mopts9_argv;

  rc= handle_options(&mopts9_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc != 0), "%s", "test_mopts9 call");
}

int mopts10_argc= 2;
const char *mopts10_argv[]= {"mopts10", "--skip-foo"};
void test_mopts10()
{
  int rc;
  char **av= (char **)mopts10_argv;

  rc= handle_options(&mopts10_argc, &av, mopts_options, &dummy_get_one_option);
  ok( (rc != 0), "%s", "test_mopts10 call");
}

ulong auto_num;
static struct my_option auto_options[]=
{
  {"anum", 0,
   "Something numeric.",
   &auto_num, &auto_num, 0, GET_ULONG | GET_AUTO,
   REQUIRED_ARG, 1000000L, 1, ULONG_MAX, 0, 1, 0},
  {"num", 0,
   "Something numeric.",
   &mopts_num, &mopts_num, 0, GET_ULONG,
   REQUIRED_ARG, 1000000L, 1, ULONG_MAX, 0, 1, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};



my_bool auto_get_one_option(const struct my_option *opt,
                            const char *argument,
                            const char *filename __attribute__((unused)))
{
  if (argument == autoset_my_option)
  {
    *((ulong*)opt->value)= 111;
  }
  return FALSE;
}

int auto2_argc= 3;
const char *auto2_argv[]= {"auto2", "--num=123", "--autoset-num"};
void test_auto2()
{
  int rc;
  char **av= (char **)auto2_argv;

  rc= handle_options(&auto2_argc, &av, auto_options, &auto_get_one_option);
  ok( (rc == EXIT_ARGUMENT_INVALID), "%s", "test_auto2 call");
}

int auto3_argc= 3;
const char *auto3_argv[]= {"auto3", "--anum=123", "--autoset-anum"};
void test_auto3()
{
  int rc;
  char **av= (char **)auto3_argv;

  rc= handle_options(&auto3_argc, &av, auto_options, &auto_get_one_option);
  ok( (rc == 0), "%s", "test_auto3 call");
  ok( (mopts_num == 1000000L), "%s", "test_auto3 num");
  ok( (auto_num == 111), "%s", "test_auto3 anum");
}

int auto4_argc= 3;
const char *auto4_argv[]= {"auto4", "--loose-autoset-num", "--loose-autoset-anum"};
void test_auto4()
{
  int rc;
  char **av= (char **)auto4_argv;

  rc= handle_options(&auto4_argc, &av, auto_options, &auto_get_one_option);
  ok( (rc == 0), "%s", "test_auto4 call");
  ok( (mopts_num == 1000000L), "%s", "test_auto4 num");
  ok( (auto_num == 111), "%s", "test_auto4 anum");
}

int auto5_argc= 3;
const char *auto5_argv[]= {"auto5", "--autoset-loose-num", "--autoset-loose-anum"};
void test_auto5()
{
  int rc;
  char **av= (char **)auto5_argv;

  rc= handle_options(&auto5_argc, &av, auto_options, &auto_get_one_option);
  ok( (rc == 0), "%s", "test_auto5 call");
  ok( (mopts_num == 1000000L), "%s", "test_auto5 num");
  ok( (auto_num == 111), "%s", "test_auto5 anum");
}

int auto6_argc= 3;
const char *auto6_argv[]= {"auto6", "--autoset-anum", "--anum=123"};
void test_auto6()
{
  int rc;
  char **av= (char **)auto6_argv;

  rc= handle_options(&auto6_argc, &av, auto_options, &auto_get_one_option);
  ok( (rc == 0), "%s", "test_auto6 call");
  ok( (mopts_num == 1000000L), "%s", "test_auto6 num");
  ok( (auto_num == 123), "%s", "test_auto6 anum");
}


ulong max_num= ULONG_MAX;
static struct my_option max_options[]=
{
  {"num", 0,
   "Something numeric.",
   &mopts_num, &max_num, 0, GET_ULONG,
   REQUIRED_ARG, 1000000L, 1, 1000001L, 0, 1, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

int max1_argc= 3;
const char *max1_argv[]= {"max1", "--num=100", "--num=200"};
void test_max1()
{
  int rc;
  char **av= (char **)max1_argv;

  rc= handle_options(&max1_argc, &av, max_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_max1 call");
  ok( (mopts_num == 200), "%s", "test_max1 num");
  ok( (max_num == 1000001L), "%s", "test_max1 max_num");
}
int max2_argc= 3;
const char *max2_argv[]= {"max2", "--maximum-num=100", "--num=200"};
void test_max2()
{
  int rc;
  char **av= (char **)max2_argv;

  rc= handle_options(&max2_argc, &av, max_options, &dummy_get_one_option);
  ok( (rc == 0), "%s", "test_max2 call");
  ok( (mopts_num == 200), "%s", "test_max2 num");
  ok( (max_num == 100), "%s", "test_max2 max_num");
}

int main(int argc __attribute__((unused)), char **argv)
{
  MY_INIT(argv[0]);
  plan(4*8 + 1*4 + 3*4 + 3*2 + 2);

  /* gcc 4.1.2 doesn't want it in the initializer, we have to do it run-time */
  mopts_options[0].def_value= (intptr)"ddd";

  test_mopts1();
  test_mopts2();
  test_mopts3();
  test_mopts4();
  test_mopts5();
  test_mopts6();
  test_mopts7();
  test_mopts8();

  test_mopts9();
  test_mopts10();
  test_auto2();

  test_auto3();
  test_auto4();
  test_auto5();
  test_auto6();

  test_max1();
  test_max2();

  run("--ull=100", NULL);
  ok(res==0 && arg_c==0 && opt_ull==100,
     "res:%d, argc:%d, opt_ull:%llu", res, arg_c, opt_ull);

  run("--ull=-100", NULL);
  ok(res==9 && arg_c==1 && opt_ull==0ULL,
     "res:%d, argc:%d, opt_ull:%llu", res, arg_c, opt_ull);
  run("--ul=-100", NULL);
  ok(res==9 && arg_c==1 && opt_ul==0UL,
     "res:%d, argc:%d, opt_ul:%lu", res, arg_c, opt_ul);

  my_end(0);
  return exit_status();
}
