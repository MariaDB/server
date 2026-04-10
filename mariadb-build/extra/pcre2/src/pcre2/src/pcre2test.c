/*************************************************
*             PCRE2 testing program              *
*************************************************/

/* PCRE2 is a library of functions to support regular expressions whose syntax
and semantics are as close as possible to those of the Perl 5 language. In 2014
the API was completely revised and '2' was added to the name, because the old
API, which had lasted for 16 years, could not accommodate new requirements. At
the same time, this testing program was re-designed because its original
hacked-up (non-) design had also run out of steam.

                       Written by Philip Hazel
     Original code Copyright (c) 1997-2012 University of Cambridge
    Rewritten code Copyright (c) 2016-2024 University of Cambridge

-----------------------------------------------------------------------------
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    * Neither the name of the University of Cambridge nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
-----------------------------------------------------------------------------
*/


/* This program supports testing of the 8-bit, 16-bit, and 32-bit PCRE2
libraries in a single program, though its input and output are always 8-bit.
It is different from modules such as pcre2_compile.c in the library itself,
which are compiled separately for each code unit width. If two widths are
enabled, for example, pcre2_compile.c is compiled twice. In contrast,
pcre2test.c is compiled only once, and linked with all the enabled libraries.
Therefore, it must not make use of any of the macros from pcre2.h or
pcre2_internal.h that depend on PCRE2_CODE_UNIT_WIDTH. It does, however, make
use of SUPPORT_PCRE2_8, SUPPORT_PCRE2_16, and SUPPORT_PCRE2_32, to ensure that
it references only the enabled library functions. */


#if defined HAVE_CONFIG_H && !defined PCRE2_CONFIG_H_IDEMPOTENT_GUARD
#define PCRE2_CONFIG_H_IDEMPOTENT_GUARD
#include "config.h"
#endif



#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <locale.h>
#include <errno.h>

#if defined NATIVE_ZOS
#include "pcrzoscs.h"
/* That header is not included in the main PCRE2 distribution because other
apparatus is needed to compile pcre2test for z/OS. The header can be found in
the special z/OS distribution, which is available from www.zaconsultants.net or
from www.cbttape.org. */
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

/* Debugging code enabler */

/* #define DEBUG_SHOW_MALLOC_ADDRESSES */

/* Both libreadline and libedit are optionally supported */
#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
#if defined(SUPPORT_LIBREADLINE)
#include <readline/readline.h>
#include <readline/history.h>
#else
#if defined(HAVE_EDITLINE_READLINE_H)
#include <editline/readline.h>
#elif defined(HAVE_EDIT_READLINE_READLINE_H)
#include <edit/readline/readline.h>
#else
#include <readline.h>
/* GNU readline defines this macro but libedit doesn't, if that ever changes
this needs to be updated or the build could break */
#ifdef RL_VERSION_MAJOR
#include <history.h>
#endif
#endif
#endif
#endif

/* Put the test for interactive input into a macro so that it can be changed if
required for different environments. */

#define INTERACTIVE(f) isatty(fileno(f))


/* ---------------------- System-specific definitions ---------------------- */

/* A number of things vary for Windows builds. Originally, pcretest opened its
input and output without "b"; then I was told that "b" was needed in some
environments, so it was added for release 5.0 to both the input and output. (It
makes no difference on Unix-like systems.) Later I was told that it is wrong
for the input on Windows. I've now abstracted the modes into macros that are
set here, to make it easier to fiddle with them, and removed "b" from the input
mode under Windows. The BINARY versions are used when saving/restoring compiled
patterns. */

#if defined(_WIN32) || defined(WIN32)
#include <io.h>                /* For _setmode() */
#include <fcntl.h>             /* For _O_BINARY */
#define INPUT_MODE          "r"
#define OUTPUT_MODE         "wb"
#define BINARY_INPUT_MODE   "rb"
#define BINARY_OUTPUT_MODE  "wb"

#ifndef isatty
#define isatty _isatty         /* This is what Windows calls them, I'm told, */
#endif                         /* though in some environments they seem to   */
                               /* be already defined, hence the #ifndefs.    */
#ifndef fileno
#define fileno _fileno
#endif

/* A user sent this fix for Borland Builder 5 under Windows. */

#ifdef __BORLANDC__
#define _setmode(handle, mode) setmode(handle, mode)
#endif

/* Not Windows */

#else
#include <sys/time.h>          /* These two includes are needed */
#include <sys/resource.h>      /* for setrlimit(). */
#if defined NATIVE_ZOS         /* z/OS uses non-binary I/O */
#define INPUT_MODE   "r"
#define OUTPUT_MODE  "w"
#define BINARY_INPUT_MODE   "rb"
#define BINARY_OUTPUT_MODE  "wb"
#else
#define INPUT_MODE          "rb"
#define OUTPUT_MODE         "wb"
#define BINARY_INPUT_MODE   "rb"
#define BINARY_OUTPUT_MODE  "wb"
#endif
#endif

/* VMS-specific code was included as suggested by a VMS user [1]. Another VMS
user [2] provided alternative code which worked better for him. I have
commented out the original, but kept it around just in case. */

#ifdef __VMS
#include <ssdef.h>
/* These two includes came from [2]. */
#include descrip
#include lib$routines
/* void vms_setsymbol( char *, char *, int ); Original code from [1]. */
#endif

/* old VC and older compilers don't support %td or %zu, and even some that
claim to be C99 don't support it (hence DISABLE_PERCENT_ZT). */

#if defined(DISABLE_PERCENT_ZT) || (defined(_MSC_VER) && (_MSC_VER < 1800)) || \
  (!defined(_MSC_VER) && (!defined(__STDC_VERSION__) || (__STDC_VERSION__ < 199901L)))
#ifdef _WIN64
#define PTR_FORM "lld"
#define SIZ_FORM "llu"
#else
#define PTR_FORM "ld"
#define SIZ_FORM "lu"
#endif
#else
#define PTR_FORM "td"
#define SIZ_FORM "zu"
#endif

/* ------------------End of system-specific definitions -------------------- */

/* Glueing macros that are used in several places below. */

#define glue(a,b) a##b
#define G(a,b) glue(a,b)

#define stringify(x) #x
#define STR(x) stringify(x)

/* Miscellaneous parameters and manifests */

#ifndef CLOCKS_PER_SEC
#ifdef CLK_TCK
#define CLOCKS_PER_SEC CLK_TCK
#else
#define CLOCKS_PER_SEC 100
#endif
#endif

#define CFORE_UNSET UINT32_MAX    /* Unset value for startend/cfail/cerror fields */
#define CONVERT_UNSET UINT32_MAX  /* Unset value for convert_type/convert_length fields */
#define MOD_STR_UNSET UINT8_MAX   /* Sentinel length for unset string options */
#define DFA_WS_DIMENSION 1000     /* Size of DFA workspace */
#define DEFAULT_OVECCOUNT 15      /* Default ovector count */
#define JUNK_OFFSET 0xdeadbeef    /* For initializing ovector */
#define LOCALESIZE 32             /* Size of locale name */
#define LOOPREPEAT 500000         /* Default loop count for timing */
#define MALLOCLISTSIZE 20         /* For remembering mallocs */
#define PARENS_NEST_DEFAULT 220   /* Default parentheses nest limit */
#define PATSTACKSIZE 20           /* Pattern stack for save/restore testing */
#define REPLACE_MODSIZE 100       /* Field for reading 8-bit replacement */
#define SUBSTITUTE_SUBJECT_MODSIZE 100 /* Field for reading 8-bit subject for substitute */
#define VERSION_SIZE 64           /* Size of buffer for the version strings */
#define REPLACE_BUFFSIZE 256      /* Code units for replacement buffer */

/* Default JIT compile options */

#define JIT_DEFAULT (PCRE2_JIT_COMPLETE|\
                     PCRE2_JIT_PARTIAL_SOFT|\
                     PCRE2_JIT_PARTIAL_HARD)

/* Execution modes */

#define PCRE2TEST_MODE_8   8
#define PCRE2TEST_MODE_16 16
#define PCRE2TEST_MODE_32 32

/* Processing returns */

enum { PR_OK, PR_SKIP, PR_ABEND, PR_ENDIF };

/* The macro EBCDIC_IO describes whether pcre2tests takes ASCII or EBCDIC as
its input files (or terminal input). If the compiler uses ASCII for character
literals, then we make pcre2test take ASCII as its input and output. This is
different to the core PCRE2 library, where we use macros like "CHAR_A" for every
single character and string literal used in pattern parsing and matching. It
would simply be too arduous to do the same for pcre2test, so we make its
input/output format match the compiler's codepage. */
#if defined(EBCDIC) && 'a' == 0x81
#define EBCDIC_IO 1
#else
#define EBCDIC_IO 0
#endif

/* The macro PRINTABLE determines whether to print an output character as-is or
as a hex value when showing compiled patterns. We use it in cases when the
locale has not been explicitly changed, so as to get consistent output from
systems that differ in their output from isprint() even in the "C" locale. */

#if defined(EBCDIC)
#define PRINTABLE(c) printable(c)
#else
#define PRINTABLE(c) ((c) >= 32 && (c) < 127)
#endif

/* The macro CHAR_OUTPUT is used to output characters in pcre2test's output
format. The input character is encoded in PCRE2's native codepage (EBCDIC, if
enabled), but the output may differ in the case where pcre2test uses ASCII input
and output. */
#if defined(EBCDIC) && !EBCDIC_IO
#define CHAR_OUTPUT(c)      ebcdic_to_ascii(c)
#define CHAR_OUTPUT_HEX(c)  CHAR_OUTPUT(c)
#define CHAR_INPUT(c)       ascii_to_ebcdic(c)
#define CHAR_INPUT_HEX(c)   CHAR_INPUT(c)
#elif defined(EBCDIC)
#define CHAR_OUTPUT(c)      (c)
#define CHAR_OUTPUT_HEX(c)  ebcdic_to_ascii(c)
#define CHAR_INPUT(c)       (c)
#define CHAR_INPUT_HEX(c)   ascii_to_ebcdic(c)
#else
#define CHAR_OUTPUT(c)      (c)
#define CHAR_OUTPUT_HEX(c)  CHAR_OUTPUT(c)
#define CHAR_INPUT(c)       (c)
#define CHAR_INPUT_HEX(c)   CHAR_INPUT(c)
#endif

/* We have to include some of the library source files because we need
to use some of the macros, internal structure definitions, and other internal
values - pcre2test has "inside information" compared to an application program
that strictly follows the PCRE2 API.

Before including pcre2_internal.h we define PRIV so that it does not get
defined therein. This ensures that PRIV names in the included files do not
clash with those in the libraries. Also, although pcre2_internal.h does itself
include pcre2.h, we explicitly include it beforehand, along with pcre2posix.h,
so that the PCRE2_EXP_xxx macros get set appropriately for an application, not
for building the library.

Setting PCRE2_CODE_UNIT_WIDTH to zero cuts out all the width-specific settings
in pcre2.h and pcre2_internal.h. Defining PCRE2_PCRE2TEST cuts out the check in
pcre2_internal.h that ensures PCRE2_CODE_UNIT_WIDTH is 8, 16, or 32 (which it
needs to be when compiling one of the libraries). */

#define PRIV(name) name
#define PCRE2_CODE_UNIT_WIDTH 0
#define PCRE2_PCRE2TEST
#include "pcre2.h"
#include "pcre2posix.h"
#include "pcre2_internal.h"

/* We need access to some of the data tables that PCRE2 uses. The previous
definition of PCRE2_PCRE2TEST makes some minor changes in the files. The
previous definition of PRIV avoids name clashes. */

#include "pcre2_tables.c"
#include "pcre2_ucd.c"

/* Forward-declarations for PRINTABLE(), etc. */

#if defined(EBCDIC)
static BOOL printable(uint32_t c);
#endif
#if defined(EBCDIC) && !EBCDIC_IO
static void ascii_to_ebcdic_str(uint8_t *buf, size_t len);
static void ebcdic_to_ascii_str(uint8_t *buf, size_t len);
#endif
#if defined(EBCDIC)
static uint32_t ascii_to_ebcdic(uint32_t c);
static uint32_t ebcdic_to_ascii(uint32_t c);
#endif

/* 32-bit integer values in the input are read by strtoul() or strtol(). The
check needed for overflow depends on whether long ints are in fact longer than
ints. They are defined not to be shorter. */

#if ULONG_MAX > UINT32_MAX
#define U32OVERFLOW(x) (x > UINT32_MAX)
#else
#define U32OVERFLOW(x) (x == UINT32_MAX)
#endif

#if LONG_MAX > INT32_MAX
#define S32OVERFLOW(x) (x > INT32_MAX || x < INT32_MIN)
#else
#define S32OVERFLOW(x) (x == INT32_MAX || x == INT32_MIN)
#endif

/* When PCRE2_CODE_UNIT_WIDTH is zero, pcre2_internal.h does not include
pcre2_intmodedep.h, which is where mode-dependent macros and structures are
defined. We can now include it for each supported code unit width. Because
PCRE2_CODE_UNIT_WIDTH was defined as zero before including pcre2.h, it will
have left PCRE2_SUFFIX defined as a no-op. We must re-define it appropriately
while including these files, and then restore it to a no-op. Because LINK_SIZE
may be changed in 16-bit mode and forced to 1 in 32-bit mode, the order of
these inclusions should not be changed. */

#undef PCRE2_SUFFIX
#undef PCRE2_CODE_UNIT_WIDTH

#ifdef   SUPPORT_PCRE2_8
#define  PCRE2_CODE_UNIT_WIDTH 8
#define  PCRE2_SUFFIX(a) G(a,8)
#include "pcre2_intmodedep.h"
#include "pcre2_printint_inc.h"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SUFFIX
#endif   /* SUPPORT_PCRE2_8 */

#ifdef   SUPPORT_PCRE2_16
#define  PCRE2_CODE_UNIT_WIDTH 16
#define  PCRE2_SUFFIX(a) G(a,16)
#include "pcre2_intmodedep.h"
#include "pcre2_printint_inc.h"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SUFFIX
#endif   /* SUPPORT_PCRE2_16 */

#ifdef   SUPPORT_PCRE2_32
#define  PCRE2_CODE_UNIT_WIDTH 32
#define  PCRE2_SUFFIX(a) G(a,32)
#include "pcre2_intmodedep.h"
#include "pcre2_printint_inc.h"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SUFFIX
#endif   /* SUPPORT_PCRE2_32 */

#define PCRE2_CODE_UNIT_WIDTH 0
#include "pcre2_intmodedep.h"  /* Clear out the stale macros */
#undef PCRE2_CODE_UNIT_WIDTH

#define PCRE2_SUFFIX(a) a

/* We need to be able to check input text for UTF-8 validity, whatever code
widths are actually available, because the input to pcre2test is always in
8-bit code units. So we include the UTF validity checking function for 8-bit
code units. */

extern int valid_utf(PCRE2_SPTR8, PCRE2_SIZE, PCRE2_SIZE *);

#define  PCRE2_CODE_UNIT_WIDTH 8
#undef   PCRE2_SPTR
#define  PCRE2_SPTR PCRE2_SPTR8
#include "pcre2_valid_utf.c"
#undef   PCRE2_CODE_UNIT_WIDTH
#undef   PCRE2_SPTR
#define  PCRE2_SPTR PCRE2_SUFFIX(PCRE2_SPTR)

/* If we have 8-bit support, default to it; if there is also 16-or 32-bit
support, it can be selected by a command line option. If there is no 8-bit
support, there must be 16-bit or 32-bit support, so default to one of them.

The contexts just happen to be exactly the same layout on all bit-widths
(although the contents are very much not the same). For example, the 8-bit
and 16-bit match contexts have the same fields, all at the same offsets and
sizes, but the function pointers for the callouts in the 8-bit context are not
of the same type as in the 16-bit context. When we are parsing the modifier
bits, it is convenient to be able to uniformly set flags in any of the contexts,
so for that purpose only we may ignore the differences between the contexts at
different bit-widths. Choose one arbitrarily (does not need to match the
test mode). */

#if defined SUPPORT_PCRE2_8
#define DEFAULT_TEST_MODE PCRE2TEST_MODE_8
#define PCRE2_REAL_COMPILE_CONTEXT pcre2_real_compile_context_8
#define PCRE2_REAL_MATCH_CONTEXT pcre2_real_match_context_8

#elif defined SUPPORT_PCRE2_16
#define DEFAULT_TEST_MODE PCRE2TEST_MODE_16
#define PCRE2_REAL_COMPILE_CONTEXT pcre2_real_compile_context_16
#define PCRE2_REAL_MATCH_CONTEXT pcre2_real_match_context_16

#elif defined SUPPORT_PCRE2_32
#define DEFAULT_TEST_MODE PCRE2TEST_MODE_32
#define PCRE2_REAL_COMPILE_CONTEXT pcre2_real_compile_context_32
#define PCRE2_REAL_MATCH_CONTEXT pcre2_real_match_context_32
#endif

/* ------------- Structure and table for handling #-commands ------------- */

typedef struct cmdstruct {
  const char *name;
  int  value;
} cmdstruct;

enum { CMD_ENDIF, CMD_FORBID_UTF, CMD_IF, CMD_LOAD, CMD_LOADTABLES,
  CMD_NEWLINE_DEFAULT, CMD_PATTERN, CMD_PERLTEST, CMD_POP, CMD_POPCOPY,
  CMD_SAVE, CMD_SUBJECT, CMD_UNKNOWN };

static cmdstruct cmdlist[] = {
  { "endif",           CMD_ENDIF },
  { "forbid_utf",      CMD_FORBID_UTF },
  { "if",              CMD_IF },
  { "load",            CMD_LOAD },
  { "loadtables",      CMD_LOADTABLES },
  { "newline_default", CMD_NEWLINE_DEFAULT },
  { "pattern",         CMD_PATTERN },
  { "perltest",        CMD_PERLTEST },
  { "pop",             CMD_POP },
  { "popcopy",         CMD_POPCOPY },
  { "save",            CMD_SAVE },
  { "subject",         CMD_SUBJECT }};

#define cmdlistcount (sizeof(cmdlist)/sizeof(cmdstruct))

/* ------------- Structures and tables for handling modifiers -------------- */

/* Table of names for newline types. Must be kept in step with the definitions
of PCRE2_NEWLINE_xx in pcre2.h. */

static const char *newlines[] = {
  "DEFAULT", "CR", "LF", "CRLF", "ANY", "ANYCRLF", "NUL" };

/* Structure and table for handling pattern conversion types. */

typedef struct convertstruct {
  const char *name;
  uint32_t option;
} convertstruct;

static convertstruct convertlist[] = {
  { "glob",                   PCRE2_CONVERT_GLOB },
  { "glob_no_starstar",       PCRE2_CONVERT_GLOB_NO_STARSTAR },
  { "glob_no_wild_separator", PCRE2_CONVERT_GLOB_NO_WILD_SEPARATOR },
  { "posix_basic",            PCRE2_CONVERT_POSIX_BASIC },
  { "posix_extended",         PCRE2_CONVERT_POSIX_EXTENDED },
  { "unset",                  CONVERT_UNSET }};

#define convertlistcount (sizeof(convertlist)/sizeof(convertstruct))

/* Modifier types and applicability */

enum { MOD_CTC,    /* Applies to a compile context */
       MOD_CTM,    /* Applies to a match context */
       MOD_PAT,    /* Applies to a pattern */
       MOD_PATP,   /* Ditto, OK for Perl test */
       MOD_DAT,    /* Applies to a data line */
       MOD_DATP,   /* Ditto, OK for Perl test */
       MOD_PD,     /* Applies to a pattern or a data line */
       MOD_PDP,    /* As MOD_PD, OK for Perl test */
       MOD_PND,    /* As MOD_PD, but not for a default pattern */
       MOD_PNDP,   /* As MOD_PND, OK for Perl test */
       MOD_CHR,    /* Is a single character */
       MOD_CON,    /* Is a "convert" type/options list */
       MOD_CTL,    /* Is a control bit */
       MOD_BSR,    /* Is a BSR value */
       MOD_IN2,    /* Is one or two unsigned integers */
       MOD_INS,    /* Is a signed integer */
       MOD_INT,    /* Is an unsigned integer */
       MOD_IND,    /* Is an unsigned integer, but no value => default */
       MOD_NL,     /* Is a newline value */
       MOD_NN,     /* Is a number or a name; more than one may occur */
       MOD_OPT,    /* Is an option bit */
       MOD_OPTMZ,  /* Is an optimization directive */
       MOD_SIZ,    /* Is a PCRE2_SIZE value */
       MOD_STR };  /* Is a string; Pascal-encoded with length in first byte */

/* Control bits. Some apply to compiling, some to matching, but some can be set
either on a pattern or a data line, so they must all be distinct. There are now
so many of them that they are split into two fields. */

#define CTL_AFTERTEXT                    0x00000001u
#define CTL_ALLAFTERTEXT                 0x00000002u
#define CTL_ALLCAPTURES                  0x00000004u
#define CTL_ALLUSEDTEXT                  0x00000008u
#define CTL_ALTGLOBAL                    0x00000010u
#define CTL_BINCODE                      0x00000020u
#define CTL_CALLOUT_CAPTURE              0x00000040u
#define CTL_CALLOUT_INFO                 0x00000080u
#define CTL_CALLOUT_NONE                 0x00000100u
#define CTL_DFA                          0x00000200u
#define CTL_EXPAND                       0x00000400u
#define CTL_FINDLIMITS                   0x00000800u
#define CTL_FINDLIMITS_NOHEAP            0x00001000u
#define CTL_FULLBINCODE                  0x00002000u
#define CTL_GETALL                       0x00004000u
#define CTL_GLOBAL                       0x00008000u
#define CTL_HEXPAT                       0x00010000u  /* Same word as USE_LENGTH */
#define CTL_INFO                         0x00020000u
#define CTL_JITFAST                      0x00040000u
#define CTL_JITVERIFY                    0x00080000u
#define CTL_MARK                         0x00100000u
#define CTL_MEMORY                       0x00200000u
#define CTL_NULLCONTEXT                  0x00400000u
#define CTL_POSIX                        0x00800000u
#define CTL_POSIX_NOSUB                  0x01000000u
#define CTL_PUSH                         0x02000000u  /* These three must be */
#define CTL_PUSHCOPY                     0x04000000u  /*   all in the same */
#define CTL_PUSHTABLESCOPY               0x08000000u  /*     word. */
#define CTL_STARTCHAR                    0x10000000u
#define CTL_USE_LENGTH                   0x20000000u  /* Same word as HEXPAT */
#define CTL_UTF8_INPUT                   0x40000000u
#define CTL_ZERO_TERMINATE               0x80000000u

/* Combinations */

#define CTL_DEBUG            (CTL_FULLBINCODE|CTL_INFO)  /* For setting */
#define CTL_ANYGLOB          (CTL_ALTGLOBAL|CTL_GLOBAL)

/* Second control word */

#define CTL2_SUBSTITUTE_CALLOUT          0x00000001u
#define CTL2_SUBSTITUTE_EXTENDED         0x00000002u
#define CTL2_SUBSTITUTE_LITERAL          0x00000004u
#define CTL2_SUBSTITUTE_MATCHED          0x00000008u
#define CTL2_SUBSTITUTE_OVERFLOW_LENGTH  0x00000010u
#define CTL2_SUBSTITUTE_REPLACEMENT_ONLY 0x00000020u
#define CTL2_SUBSTITUTE_UNKNOWN_UNSET    0x00000040u
#define CTL2_SUBSTITUTE_UNSET_EMPTY      0x00000080u
#define CTL2_SUBJECT_LITERAL             0x00000100u
#define CTL2_CALLOUT_NO_WHERE            0x00000200u
#define CTL2_CALLOUT_EXTRA               0x00000400u
#define CTL2_ALLVECTOR                   0x00000800u
#define CTL2_NULL_PATTERN                0x00001000u
#define CTL2_NULL_SUBJECT                0x00002000u
#define CTL2_NULL_REPLACEMENT            0x00004000u
#define CTL2_FRAMESIZE                   0x00008000u
#define CTL2_SUBSTITUTE_CASE_CALLOUT     0x00010000u
#define CTL2_NULL_SUBSTITUTE_MATCH_DATA  0x00020000u

#define CTL2_HEAPFRAMES_SIZE             0x20000000u  /* Informational */
#define CTL2_NL_SET                      0x40000000u  /* Informational */
#define CTL2_BSR_SET                     0x80000000u  /* Informational */

/* These are the matching controls that may be set either on a pattern or on a
data line. They are copied from the pattern controls as initial settings for
data line controls. Note that CTL_MEMORY is not included here, because it does
different things in the two cases. */

#define CTL_ALLPD  (CTL_AFTERTEXT|\
                    CTL_ALLAFTERTEXT|\
                    CTL_ALLCAPTURES|\
                    CTL_ALLUSEDTEXT|\
                    CTL_ALTGLOBAL|\
                    CTL_GLOBAL|\
                    CTL_MARK|\
                    CTL_STARTCHAR|\
                    CTL_UTF8_INPUT)

#define CTL2_ALLPD (CTL2_SUBSTITUTE_CALLOUT|\
                    CTL2_SUBSTITUTE_EXTENDED|\
                    CTL2_SUBSTITUTE_LITERAL|\
                    CTL2_SUBSTITUTE_MATCHED|\
                    CTL2_SUBSTITUTE_OVERFLOW_LENGTH|\
                    CTL2_SUBSTITUTE_REPLACEMENT_ONLY|\
                    CTL2_SUBSTITUTE_UNKNOWN_UNSET|\
                    CTL2_SUBSTITUTE_UNSET_EMPTY|\
                    CTL2_ALLVECTOR|\
                    CTL2_SUBSTITUTE_CASE_CALLOUT|\
                    CTL2_NULL_SUBSTITUTE_MATCH_DATA|\
                    CTL2_HEAPFRAMES_SIZE)

/* Structures for holding modifier information for patterns and subject strings
(data). Fields containing modifiers that can be set either for a pattern or a
subject (MOD_PD[P]/MOD_PND) must be at the start and in the same order in both
structures so that the same offset in the big table below works for both. */

typedef struct patctl {       /* Structure for pattern modifiers. */
  uint32_t  options;          /* Must be in same position as datctl */
  uint32_t  control;          /* Must be in same position as datctl */
  uint32_t  control2;         /* Must be in same position as datctl */
  uint32_t  jitstack;         /* Must be in same position as datctl */
   uint8_t  replacement[1+REPLACE_MODSIZE];         /* So must this */
  uint32_t  substitute_skip;  /* Must be in same position as datctl */
  uint32_t  substitute_stop;  /* Must be in same position as datctl */
  uint32_t  jit;
  uint32_t  stackguard_test;
  uint32_t  tables_id;
  uint32_t  convert_type;
  uint32_t  convert_length;
  uint32_t  convert_glob_escape;
  uint32_t  convert_glob_separator;
   int32_t  regerror_buffsize;
   uint8_t  locale[1+LOCALESIZE];
} patctl;

#define MAXCPYGET 10
#define LENCPYGET 64

typedef struct datctl {        /* Structure for data line modifiers. */
  uint32_t   options;          /* Must be in same position as patctl */
  uint32_t   control;          /* Must be in same position as patctl */
  uint32_t   control2;         /* Must be in same position as patctl */
  uint32_t   jitstack;         /* Must be in same position as patctl */
   uint8_t   replacement[1+REPLACE_MODSIZE];         /* So must this */
  uint32_t   substitute_skip;  /* Must be in same position as patctl */
  uint32_t   substitute_stop;  /* Must be in same position as patctl */
   uint8_t   substitute_subject[1+SUBSTITUTE_SUBJECT_MODSIZE];
  uint32_t   startend[2];
  uint32_t   cerror[2];
  uint32_t   cfail[2];
   int32_t   callout_data;
   int32_t   copy_numbers[MAXCPYGET];
   int32_t   get_numbers[MAXCPYGET];
  uint32_t   oveccount;
  PCRE2_SIZE offset;
  uint8_t    copy_names[LENCPYGET];
  uint8_t    get_names[LENCPYGET];
} datctl;

/* Helper functions to zero out the structures. */

static void patctl_zero(patctl *p)
{
memset(p, 0, sizeof(patctl));
p->replacement[0] = MOD_STR_UNSET;
p->convert_type = CONVERT_UNSET;
p->convert_length = CONVERT_UNSET;
p->regerror_buffsize = -1;
p->locale[0] = MOD_STR_UNSET;
}

static void datctl_zero(datctl *d)
{
memset(d, 0, sizeof(datctl));
d->replacement[0] = MOD_STR_UNSET;
d->substitute_subject[0] = MOD_STR_UNSET;
d->oveccount = DEFAULT_OVECCOUNT;
d->copy_numbers[0] = -1;
d->get_numbers[0] = -1;
d->startend[0] = d->startend[1] = CFORE_UNSET;
d->cerror[0] = d->cerror[1] = CFORE_UNSET;
d->cfail[0] = d->cfail[1] = CFORE_UNSET;
}

/* Ids for which context to modify. */

enum { CTX_PAT,            /* Active pattern context */
       CTX_POPPAT,         /* Ditto, for a popped pattern */
       CTX_DEFPAT,         /* Default pattern context */
       CTX_DAT,            /* Active data (match) context */
       CTX_DEFDAT };       /* Default data (match) context */

/* Macros to simplify the big table below. */

#define CO(name) offsetof(PCRE2_REAL_COMPILE_CONTEXT, name)
#define MO(name) offsetof(PCRE2_REAL_MATCH_CONTEXT, name)
#define PO(name) offsetof(patctl, name)
#define DO(name) offsetof(datctl, name)

/* Validate that the offsets for the shared fields do indeed match. */

STATIC_ASSERT(PO(options) == DO(options), options_mismatch);
STATIC_ASSERT(PO(control) == DO(control), control_mismatch);
STATIC_ASSERT(PO(control2) == DO(control2), control2_mismatch);
STATIC_ASSERT(PO(jitstack) == DO(jitstack), jitstack_mismatch);
STATIC_ASSERT(PO(replacement) == DO(replacement), replacement_mismatch);
STATIC_ASSERT(PO(substitute_skip) == DO(substitute_skip), substitute_skip_mismatch);
STATIC_ASSERT(PO(substitute_stop) == DO(substitute_stop), substitute_stop_mismatch);

/* Table of all long-form modifiers. Must be in collating sequence of modifier
name because it is searched by binary chop. */

typedef struct modstruct {
  const char   *name;
  uint16_t      which;
  uint16_t      type;
  uint32_t      value;
  PCRE2_SIZE    offset;
} modstruct;

#define PCRE2_EXTRA_ASCII_ALL (PCRE2_EXTRA_ASCII_BSD|PCRE2_EXTRA_ASCII_BSS| \
  PCRE2_EXTRA_ASCII_BSW|PCRE2_EXTRA_ASCII_POSIX)

static modstruct modlist[] = {
  { "aftertext",                   MOD_PNDP, MOD_CTL, CTL_AFTERTEXT,              PO(control) },
  { "allaftertext",                MOD_PNDP, MOD_CTL, CTL_ALLAFTERTEXT,           PO(control) },
  { "allcaptures",                 MOD_PND,  MOD_CTL, CTL_ALLCAPTURES,            PO(control) },
  { "allow_empty_class",           MOD_PAT,  MOD_OPT, PCRE2_ALLOW_EMPTY_CLASS,    PO(options) },
  { "allow_lookaround_bsk",        MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK, CO(extra_options) },
  { "allow_surrogate_escapes",     MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ALLOW_SURROGATE_ESCAPES, CO(extra_options) },
  { "allusedtext",                 MOD_PNDP, MOD_CTL, CTL_ALLUSEDTEXT,            PO(control) },
  { "allvector",                   MOD_PND,  MOD_CTL, CTL2_ALLVECTOR,             PO(control2) },
  { "alt_bsux",                    MOD_PAT,  MOD_OPT, PCRE2_ALT_BSUX,             PO(options) },
  { "alt_circumflex",              MOD_PAT,  MOD_OPT, PCRE2_ALT_CIRCUMFLEX,       PO(options) },
  { "alt_extended_class",          MOD_PAT,  MOD_OPT, PCRE2_ALT_EXTENDED_CLASS,   PO(options) },
  { "alt_verbnames",               MOD_PAT,  MOD_OPT, PCRE2_ALT_VERBNAMES,        PO(options) },
  { "altglobal",                   MOD_PND,  MOD_CTL, CTL_ALTGLOBAL,              PO(control) },
  { "anchored",                    MOD_PD,   MOD_OPT, PCRE2_ANCHORED,             PO(options) },
  { "ascii_all",                   MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ASCII_ALL,      CO(extra_options) },
  { "ascii_bsd",                   MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ASCII_BSD,      CO(extra_options) },
  { "ascii_bss",                   MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ASCII_BSS,      CO(extra_options) },
  { "ascii_bsw",                   MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ASCII_BSW,      CO(extra_options) },
  { "ascii_digit",                 MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ASCII_DIGIT,    CO(extra_options) },
  { "ascii_posix",                 MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ASCII_POSIX,    CO(extra_options) },
  { "auto_callout",                MOD_PAT,  MOD_OPT, PCRE2_AUTO_CALLOUT,         PO(options) },
  { "auto_possess",                MOD_CTC,  MOD_OPTMZ, PCRE2_AUTO_POSSESS,       0 },
  { "auto_possess_off",            MOD_CTC,  MOD_OPTMZ, PCRE2_AUTO_POSSESS_OFF,   0 },
  { "bad_escape_is_literal",       MOD_CTC,  MOD_OPT, PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL, CO(extra_options) },
  { "bincode",                     MOD_PAT,  MOD_CTL, CTL_BINCODE,                PO(control) },
  { "bsr",                         MOD_CTC,  MOD_BSR, 0,                          CO(bsr_convention) },
  { "callout_capture",             MOD_DAT,  MOD_CTL, CTL_CALLOUT_CAPTURE,        DO(control) },
  { "callout_data",                MOD_DAT,  MOD_INS, 0,                          DO(callout_data) },
  { "callout_error",               MOD_DAT,  MOD_IN2, 0,                          DO(cerror) },
  { "callout_extra",               MOD_DAT,  MOD_CTL, CTL2_CALLOUT_EXTRA,         DO(control2) },
  { "callout_fail",                MOD_DAT,  MOD_IN2, 0,                          DO(cfail) },
  { "callout_info",                MOD_PAT,  MOD_CTL, CTL_CALLOUT_INFO,           PO(control) },
  { "callout_no_where",            MOD_DAT,  MOD_CTL, CTL2_CALLOUT_NO_WHERE,      DO(control2) },
  { "callout_none",                MOD_DAT,  MOD_CTL, CTL_CALLOUT_NONE,           DO(control) },
  { "caseless",                    MOD_PATP, MOD_OPT, PCRE2_CASELESS,             PO(options) },
  { "caseless_restrict",           MOD_CTC,  MOD_OPT, PCRE2_EXTRA_CASELESS_RESTRICT, CO(extra_options) },
  { "convert",                     MOD_PAT,  MOD_CON, 0,                          PO(convert_type) },
  { "convert_glob_escape",         MOD_PAT,  MOD_CHR, 0,                          PO(convert_glob_escape) },
  { "convert_glob_separator",      MOD_PAT,  MOD_CHR, 0,                          PO(convert_glob_separator) },
  { "convert_length",              MOD_PAT,  MOD_INT, 0,                          PO(convert_length) },
  { "copy",                        MOD_DAT,  MOD_NN,  DO(copy_numbers),           DO(copy_names) },
  { "copy_matched_subject",        MOD_DAT,  MOD_OPT, PCRE2_COPY_MATCHED_SUBJECT, DO(options) },
  { "debug",                       MOD_PAT,  MOD_CTL, CTL_DEBUG,                  PO(control) },
  { "depth_limit",                 MOD_CTM,  MOD_INT, 0,                          MO(depth_limit) },
  { "dfa",                         MOD_DAT,  MOD_CTL, CTL_DFA,                    DO(control) },
  { "dfa_restart",                 MOD_DAT,  MOD_OPT, PCRE2_DFA_RESTART,          DO(options) },
  { "dfa_shortest",                MOD_DAT,  MOD_OPT, PCRE2_DFA_SHORTEST,         DO(options) },
  { "disable_recurseloop_check",   MOD_DAT,  MOD_OPT, PCRE2_DISABLE_RECURSELOOP_CHECK, DO(options) },
  { "dollar_endonly",              MOD_PAT,  MOD_OPT, PCRE2_DOLLAR_ENDONLY,       PO(options) },
  { "dotall",                      MOD_PATP, MOD_OPT, PCRE2_DOTALL,               PO(options) },
  { "dotstar_anchor",              MOD_CTC,  MOD_OPTMZ, PCRE2_DOTSTAR_ANCHOR,     0 },
  { "dotstar_anchor_off",          MOD_CTC,  MOD_OPTMZ, PCRE2_DOTSTAR_ANCHOR_OFF, 0 },
  { "dupnames",                    MOD_PATP, MOD_OPT, PCRE2_DUPNAMES,             PO(options) },
  { "endanchored",                 MOD_PD,   MOD_OPT, PCRE2_ENDANCHORED,          PO(options) },
  { "escaped_cr_is_lf",            MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ESCAPED_CR_IS_LF, CO(extra_options) },
  { "expand",                      MOD_PAT,  MOD_CTL, CTL_EXPAND,                 PO(control) },
  { "extended",                    MOD_PATP, MOD_OPT, PCRE2_EXTENDED,             PO(options) },
  { "extended_more",               MOD_PATP, MOD_OPT, PCRE2_EXTENDED_MORE,        PO(options) },
  { "extra_alt_bsux",              MOD_CTC,  MOD_OPT, PCRE2_EXTRA_ALT_BSUX,       CO(extra_options) },
  { "find_limits",                 MOD_DAT,  MOD_CTL, CTL_FINDLIMITS,             DO(control) },
  { "find_limits_noheap",          MOD_DAT,  MOD_CTL, CTL_FINDLIMITS_NOHEAP,      DO(control) },
  { "firstline",                   MOD_PAT,  MOD_OPT, PCRE2_FIRSTLINE,            PO(options) },
  { "framesize",                   MOD_PAT,  MOD_CTL, CTL2_FRAMESIZE,             PO(control2) },
  { "fullbincode",                 MOD_PAT,  MOD_CTL, CTL_FULLBINCODE,            PO(control) },
  { "get",                         MOD_DAT,  MOD_NN,  DO(get_numbers),            DO(get_names) },
  { "getall",                      MOD_DAT,  MOD_CTL, CTL_GETALL,                 DO(control) },
  { "global",                      MOD_PNDP, MOD_CTL, CTL_GLOBAL,                 PO(control) },
  { "heap_limit",                  MOD_CTM,  MOD_INT, 0,                          MO(heap_limit) },
  { "heapframes_size",             MOD_PND,  MOD_CTL, CTL2_HEAPFRAMES_SIZE,       PO(control2) },
  { "hex",                         MOD_PATP, MOD_CTL, CTL_HEXPAT,                 PO(control) },
  { "info",                        MOD_PAT,  MOD_CTL, CTL_INFO,                   PO(control) },
  { "jit",                         MOD_PAT,  MOD_IND, 7,                          PO(jit) },
  { "jitfast",                     MOD_PAT,  MOD_CTL, CTL_JITFAST,                PO(control) },
  { "jitstack",                    MOD_PNDP, MOD_INT, 0,                          PO(jitstack) },
  { "jitverify",                   MOD_PAT,  MOD_CTL, CTL_JITVERIFY,              PO(control) },
  { "literal",                     MOD_PAT,  MOD_OPT, PCRE2_LITERAL,              PO(options) },
  { "locale",                      MOD_PATP, MOD_STR, LOCALESIZE,                 PO(locale) },
  { "mark",                        MOD_PNDP, MOD_CTL, CTL_MARK,                   PO(control) },
  { "match_invalid_utf",           MOD_PAT,  MOD_OPT, PCRE2_MATCH_INVALID_UTF,    PO(options) },
  { "match_limit",                 MOD_CTM,  MOD_INT, 0,                          MO(match_limit) },
  { "match_line",                  MOD_CTC,  MOD_OPT, PCRE2_EXTRA_MATCH_LINE,     CO(extra_options) },
  { "match_unset_backref",         MOD_PAT,  MOD_OPT, PCRE2_MATCH_UNSET_BACKREF,  PO(options) },
  { "match_word",                  MOD_CTC,  MOD_OPT, PCRE2_EXTRA_MATCH_WORD,     CO(extra_options) },
  { "max_pattern_compiled_length", MOD_CTC,  MOD_SIZ, 0,                          CO(max_pattern_compiled_length) },
  { "max_pattern_length",          MOD_CTC,  MOD_SIZ, 0,                          CO(max_pattern_length) },
  { "max_varlookbehind",           MOD_CTC,  MOD_INT, 0,                          CO(max_varlookbehind) },
  { "memory",                      MOD_PD,   MOD_CTL, CTL_MEMORY,                 PO(control) },
  { "multiline",                   MOD_PATP, MOD_OPT, PCRE2_MULTILINE,            PO(options) },
  { "never_backslash_c",           MOD_PAT,  MOD_OPT, PCRE2_NEVER_BACKSLASH_C,    PO(options) },
  { "never_callout",               MOD_CTC,  MOD_OPT, PCRE2_EXTRA_NEVER_CALLOUT,  CO(extra_options) },
  { "never_ucp",                   MOD_PAT,  MOD_OPT, PCRE2_NEVER_UCP,            PO(options) },
  { "never_utf",                   MOD_PAT,  MOD_OPT, PCRE2_NEVER_UTF,            PO(options) },
  { "newline",                     MOD_CTC,  MOD_NL,  0,                          0 },
  { "no_auto_capture",             MOD_PAT,  MOD_OPT, PCRE2_NO_AUTO_CAPTURE,      PO(options) },
  { "no_auto_possess",             MOD_PATP, MOD_OPT, PCRE2_NO_AUTO_POSSESS,      PO(options) },
  { "no_bs0",                      MOD_CTC,  MOD_OPT, PCRE2_EXTRA_NO_BS0,         CO(extra_options) },
  { "no_dotstar_anchor",           MOD_PAT,  MOD_OPT, PCRE2_NO_DOTSTAR_ANCHOR,    PO(options) },
  { "no_jit",                      MOD_DATP, MOD_OPT, PCRE2_NO_JIT,               DO(options) },
  { "no_start_optimize",           MOD_PATP, MOD_OPT, PCRE2_NO_START_OPTIMIZE,    PO(options) },
  { "no_utf_check",                MOD_PD,   MOD_OPT, PCRE2_NO_UTF_CHECK,         PO(options) },
  { "notbol",                      MOD_DAT,  MOD_OPT, PCRE2_NOTBOL,               DO(options) },
  { "notempty",                    MOD_DAT,  MOD_OPT, PCRE2_NOTEMPTY,             DO(options) },
  { "notempty_atstart",            MOD_DAT,  MOD_OPT, PCRE2_NOTEMPTY_ATSTART,     DO(options) },
  { "noteol",                      MOD_DAT,  MOD_OPT, PCRE2_NOTEOL,               DO(options) },
  { "null_context",                MOD_PD,   MOD_CTL, CTL_NULLCONTEXT,            PO(control) },
  { "null_pattern",                MOD_PAT,  MOD_CTL, CTL2_NULL_PATTERN,          PO(control2) },
  { "null_replacement",            MOD_DAT,  MOD_CTL, CTL2_NULL_REPLACEMENT,      DO(control2) },
  { "null_subject",                MOD_DAT,  MOD_CTL, CTL2_NULL_SUBJECT,          DO(control2) },
  { "null_substitute_match_data",  MOD_PND,  MOD_CTL, CTL2_NULL_SUBSTITUTE_MATCH_DATA, PO(control2) },
  { "offset",                      MOD_DAT,  MOD_SIZ, 0,                          DO(offset) },
  { "offset_limit",                MOD_CTM,  MOD_SIZ, 0,                          MO(offset_limit)},
  { "optimization_full",           MOD_CTC,  MOD_OPTMZ, PCRE2_OPTIMIZATION_FULL,  0 },
  { "optimization_none",           MOD_CTC,  MOD_OPTMZ, PCRE2_OPTIMIZATION_NONE,  0 },
  { "ovector",                     MOD_DAT,  MOD_INT, 0,                          DO(oveccount) },
  { "parens_nest_limit",           MOD_CTC,  MOD_INT, 0,                          CO(parens_nest_limit) },
  { "partial_hard",                MOD_DAT,  MOD_OPT, PCRE2_PARTIAL_HARD,         DO(options) },
  { "partial_soft",                MOD_DAT,  MOD_OPT, PCRE2_PARTIAL_SOFT,         DO(options) },
  { "ph",                          MOD_DAT,  MOD_OPT, PCRE2_PARTIAL_HARD,         DO(options) },
  { "posix",                       MOD_PAT,  MOD_CTL, CTL_POSIX,                  PO(control) },
  { "posix_nosub",                 MOD_PAT,  MOD_CTL, CTL_POSIX|CTL_POSIX_NOSUB,  PO(control) },
  { "posix_startend",              MOD_DAT,  MOD_IN2, 0,                          DO(startend) },
  { "ps",                          MOD_DAT,  MOD_OPT, PCRE2_PARTIAL_SOFT,         DO(options) },
  { "push",                        MOD_PAT,  MOD_CTL, CTL_PUSH,                   PO(control) },
  { "pushcopy",                    MOD_PAT,  MOD_CTL, CTL_PUSHCOPY,               PO(control) },
  { "pushtablescopy",              MOD_PAT,  MOD_CTL, CTL_PUSHTABLESCOPY,         PO(control) },
  { "python_octal",                MOD_CTC,  MOD_OPT, PCRE2_EXTRA_PYTHON_OCTAL,   CO(extra_options) },
  { "recursion_limit",             MOD_CTM,  MOD_INT, 0,                          MO(depth_limit) },  /* Obsolete synonym */
  { "regerror_buffsize",           MOD_PAT,  MOD_INS, 0,                          PO(regerror_buffsize) },
  { "replace",                     MOD_PND,  MOD_STR, REPLACE_MODSIZE,            PO(replacement) },
  { "stackguard",                  MOD_PAT,  MOD_INT, 0,                          PO(stackguard_test) },
  { "start_optimize",              MOD_CTC,  MOD_OPTMZ, PCRE2_START_OPTIMIZE,     0 },
  { "start_optimize_off",          MOD_CTC,  MOD_OPTMZ, PCRE2_START_OPTIMIZE_OFF, 0 },
  { "startchar",                   MOD_PND,  MOD_CTL, CTL_STARTCHAR,              PO(control) },
  { "startoffset",                 MOD_DAT,  MOD_SIZ, 0,                          DO(offset) },
  { "subject_literal",             MOD_PATP, MOD_CTL, CTL2_SUBJECT_LITERAL,       PO(control2) },
  { "substitute_callout",          MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_CALLOUT,    PO(control2) },
  { "substitute_case_callout",     MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_CASE_CALLOUT, PO(control2) },
  { "substitute_extended",         MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_EXTENDED,   PO(control2) },
  { "substitute_literal",          MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_LITERAL,    PO(control2) },
  { "substitute_matched",          MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_MATCHED,    PO(control2) },
  { "substitute_overflow_length",  MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_OVERFLOW_LENGTH, PO(control2) },
  { "substitute_replacement_only", MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_REPLACEMENT_ONLY, PO(control2) },
  { "substitute_skip",             MOD_PND,  MOD_INT, 0,                          PO(substitute_skip) },
  { "substitute_stop",             MOD_PND,  MOD_INT, 0,                          PO(substitute_stop) },
  { "substitute_subject",          MOD_DAT,  MOD_STR, SUBSTITUTE_SUBJECT_MODSIZE, DO(substitute_subject) },
  { "substitute_unknown_unset",    MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_UNKNOWN_UNSET, PO(control2) },
  { "substitute_unset_empty",      MOD_PND,  MOD_CTL, CTL2_SUBSTITUTE_UNSET_EMPTY, PO(control2) },
  { "tables",                      MOD_PAT,  MOD_INT, 0,                          PO(tables_id) },
  { "turkish_casing",              MOD_CTC,  MOD_OPT, PCRE2_EXTRA_TURKISH_CASING, CO(extra_options) },
  { "ucp",                         MOD_PATP, MOD_OPT, PCRE2_UCP,                  PO(options) },
  { "ungreedy",                    MOD_PAT,  MOD_OPT, PCRE2_UNGREEDY,             PO(options) },
  { "use_length",                  MOD_PAT,  MOD_CTL, CTL_USE_LENGTH,             PO(control) },
  { "use_offset_limit",            MOD_PAT,  MOD_OPT, PCRE2_USE_OFFSET_LIMIT,     PO(options) },
  { "utf",                         MOD_PATP, MOD_OPT, PCRE2_UTF,                  PO(options) },
  { "utf8_input",                  MOD_PAT,  MOD_CTL, CTL_UTF8_INPUT,             PO(control) },
  { "zero_terminate",              MOD_DAT,  MOD_CTL, CTL_ZERO_TERMINATE,         DO(control) }
};

#define MODLISTCOUNT sizeof(modlist)/sizeof(modstruct)

/* Controls and options that are supported for use with the POSIX interface. */

#define POSIX_SUPPORTED_COMPILE_OPTIONS ( \
  PCRE2_CASELESS|PCRE2_DOTALL|PCRE2_LITERAL|PCRE2_MULTILINE|PCRE2_UCP| \
  PCRE2_UTF|PCRE2_UNGREEDY)

#define POSIX_SUPPORTED_COMPILE_EXTRA_OPTIONS (0)

#define POSIX_SUPPORTED_COMPILE_CONTROLS ( \
  CTL_AFTERTEXT|CTL_ALLAFTERTEXT|CTL_EXPAND|CTL_HEXPAT|CTL_POSIX| \
  CTL_POSIX_NOSUB|CTL_USE_LENGTH)

#define POSIX_SUPPORTED_COMPILE_CONTROLS2 (0)

#define POSIX_SUPPORTED_MATCH_OPTIONS ( \
  PCRE2_NOTBOL|PCRE2_NOTEMPTY|PCRE2_NOTEOL)

#define POSIX_SUPPORTED_MATCH_CONTROLS  (CTL_AFTERTEXT|CTL_ALLAFTERTEXT)
#define POSIX_SUPPORTED_MATCH_CONTROLS2 (CTL2_NULL_SUBJECT)

/* Control bits that are not ignored with 'push'. */

#define PUSH_SUPPORTED_COMPILE_CONTROLS ( \
  CTL_BINCODE|CTL_CALLOUT_INFO|CTL_FULLBINCODE|CTL_HEXPAT|CTL_INFO| \
  CTL_JITVERIFY|CTL_MEMORY|CTL_PUSH|CTL_PUSHCOPY| \
  CTL_PUSHTABLESCOPY|CTL_USE_LENGTH)

#define PUSH_SUPPORTED_COMPILE_CONTROLS2 (CTL2_BSR_SET| \
  CTL2_HEAPFRAMES_SIZE|CTL2_FRAMESIZE|CTL2_NL_SET)

/* Controls that apply only at compile time with 'push'. */

#define PUSH_COMPILE_ONLY_CONTROLS   CTL_JITVERIFY
#define PUSH_COMPILE_ONLY_CONTROLS2  (0)

/* Controls that are forbidden with #pop or #popcopy. */

#define NOTPOP_CONTROLS (CTL_HEXPAT|CTL_POSIX|CTL_POSIX_NOSUB|CTL_PUSH| \
  CTL_PUSHCOPY|CTL_PUSHTABLESCOPY|CTL_USE_LENGTH)

/* Pattern controls that are mutually exclusive. At present these are all in
the first control word. Note that CTL_POSIX_NOSUB is always accompanied by
CTL_POSIX, so it doesn't need its own entries. */

static uint32_t exclusive_pat_controls[] = {
  CTL_POSIX    | CTL_PUSH,
  CTL_POSIX    | CTL_PUSHCOPY,
  CTL_POSIX    | CTL_PUSHTABLESCOPY,
  CTL_PUSH     | CTL_PUSHCOPY,
  CTL_PUSH     | CTL_PUSHTABLESCOPY,
  CTL_PUSHCOPY | CTL_PUSHTABLESCOPY,
  CTL_EXPAND   | CTL_HEXPAT };

/* Data controls that are mutually exclusive. At present these are all in the
first control word. */

static uint32_t exclusive_dat_controls[] = {
  CTL_ALLUSEDTEXT        | CTL_STARTCHAR,
  CTL_FINDLIMITS         | CTL_NULLCONTEXT,
  CTL_FINDLIMITS_NOHEAP  | CTL_NULLCONTEXT };

/* Table of single-character abbreviated modifiers. The index field is
initialized to -1, but the first time the modifier is encountered, it is filled
in with the index of the full entry in modlist, to save repeated searching when
processing multiple test items. This short list is searched serially, so its
order does not matter. */

typedef struct c1modstruct {
  const char *fullname;
  uint32_t    onechar;
  int         index;
} c1modstruct;

static c1modstruct c1modlist[] = {
  { "bincode",           'B',           -1 },
  { "info",              'I',           -1 },
  { "ascii_all",         'a',           -1 },
  { "global",            'g',           -1 },
  { "caseless",          'i',           -1 },
  { "multiline",         'm',           -1 },
  { "no_auto_capture",   'n',           -1 },
  { "caseless_restrict", 'r',           -1 },
  { "dotall",            's',           -1 },
  { "extended",          'x',           -1 }
};

#define C1MODLISTCOUNT sizeof(c1modlist)/sizeof(c1modstruct)

/* Table of arguments for the -C command line option. Use macros to make the
table itself easier to read. */

#if defined SUPPORT_PCRE2_8
#define SUPPORT_8 1
#endif
#if defined SUPPORT_PCRE2_16
#define SUPPORT_16 1
#endif
#if defined SUPPORT_PCRE2_32
#define SUPPORT_32 1
#endif

#ifndef SUPPORT_8
#define SUPPORT_8 0
#endif
#ifndef SUPPORT_16
#define SUPPORT_16 0
#endif
#ifndef SUPPORT_32
#define SUPPORT_32 0
#endif

#if defined EBCDIC
#define SUPPORT_EBCDIC 1
#define SUPPORT_EBCDIC_NL25 CHAR_LF == 0x25
#else
#define SUPPORT_EBCDIC 0
#define SUPPORT_EBCDIC_NL25 0
#endif

#ifdef NEVER_BACKSLASH_C
#define BACKSLASH_C 0
#else
#define BACKSLASH_C 1
#endif

typedef struct coptstruct {
  const char *name;
  uint32_t    type;
  uint32_t    value;
} coptstruct;

enum { CONF_BSR,
       CONF_FIX,
       CONF_INT,
       CONF_NL,
       CONF_JU
};

static coptstruct coptlist[] = {
  { "backslash-C", CONF_FIX, BACKSLASH_C },
  { "bsr",         CONF_BSR, PCRE2_CONFIG_BSR },
  { "ebcdic",      CONF_FIX, SUPPORT_EBCDIC },
  { "ebcdic-io",   CONF_FIX, EBCDIC_IO },
  { "ebcdic-nl25", CONF_FIX, SUPPORT_EBCDIC_NL25 },
  { "jit",         CONF_INT, PCRE2_CONFIG_JIT },
  { "jitusable",   CONF_JU,  0 },
  { "linksize",    CONF_INT, PCRE2_CONFIG_EFFECTIVE_LINKSIZE },
  { "newline",     CONF_NL,  PCRE2_CONFIG_NEWLINE },
  { "pcre2-16",    CONF_FIX, SUPPORT_16 },
  { "pcre2-32",    CONF_FIX, SUPPORT_32 },
  { "pcre2-8",     CONF_FIX, SUPPORT_8 },
  { "unicode",     CONF_INT, PCRE2_CONFIG_UNICODE }
};

#define COPTLISTCOUNT sizeof(coptlist)/sizeof(coptstruct)

#undef SUPPORT_8
#undef SUPPORT_16
#undef SUPPORT_32
#undef SUPPORT_EBCDIC
#undef SUPPORT_EBDCIC_NL25
#undef BACKSLASH_C

/* Types for the parser, to be used in process_data() */

enum force_encoding {
  FORCE_NONE,         /* No preference, follow utf modifier */
  FORCE_RAW,          /* Encode as a code point or error if too wide */
  FORCE_UTF           /* Encode as a character or error if too wide */
};

/* ----------------------- Static variables ------------------------ */

static FILE *infile;
static FILE *outfile;

static const void *last_callout_mark;

static BOOL first_callout;
static BOOL jit_was_used;
static BOOL restrict_for_perl_test = FALSE;
static BOOL show_memory = FALSE;
static BOOL preprocess_only = FALSE;
static BOOL inside_if = FALSE;
static BOOL malloc_testing = FALSE;

static int jitrc;                             /* Return from JIT compile */
static int timeit = 0;
static int timeitm = 0;
static int mallocs_until_failure = INT_MAX;
static int mallocs_called = 0;

static clock_t total_compile_time = 0;
static clock_t total_jit_compile_time = 0;
static clock_t total_match_time = 0;

static uint32_t dfa_matched;
static uint32_t forbid_utf = 0;
static uint32_t maxlookbehind;
static uint32_t max_oveccount;
static uint32_t callout_count;
static uint32_t maxcapcount;

static uint16_t local_newline_default = 0;

static patctl def_patctl;
static patctl pat_patctl;
static datctl def_datctl;
static datctl dat_datctl;

static void *malloclist[MALLOCLISTSIZE];
static PCRE2_SIZE malloclistlength[MALLOCLISTSIZE];
static uint32_t malloclistptr = 0;

#ifdef SUPPORT_PCRE2_8
static regex_t preg = { NULL, NULL, 0, 0, 0, 0 };
#endif

static int *dfa_workspace = NULL;
static const uint8_t *locale_tables = NULL;
static const uint8_t *use_tables = NULL;
static uint8_t locale_name[LOCALESIZE];
static uint8_t *tables3 = NULL;         /* For binary-loaded tables */
static uint32_t loadtables_length = 0;

/* We need buffers for building 16/32-bit strings; 8-bit strings don't need
rebuilding, but set up the same naming scheme for use in macros. The "buffer"
buffer is where all input lines are read. Its size is the same as pbuffer8. */

static size_t    pbuffer8_size  = 50000;        /* Initial size, bytes */
static uint8_t  *pbuffer8 = NULL;
#ifdef SUPPORT_PCRE2_16
static size_t    pbuffer16_size = 0;   /* Size, bytes! Set only when needed */
static uint16_t *pbuffer16 = NULL;
#endif
#ifdef SUPPORT_PCRE2_32
static size_t    pbuffer32_size = 0;   /* Size, bytes! Set only when needed */
static uint32_t *pbuffer32 = NULL;
#endif
static uint8_t  *buffer = NULL;

/* The dbuffer is where all processed data lines are put. In non-8-bit modes it
is cast as needed. For long data lines it grows as necessary. */

static size_t dbuffer_size = 1u << 14;    /* Initial size, bytes */
static uint8_t *dbuffer = NULL;

/* ------------------ Colour highlighting definitions -------------------- */

/* Colour of input text that was a comment, when echoing back to the terminal */
static const int clr_comment = 37; /* grey */
/* Colour of other input text that is echoed back to the terminal */
static const int clr_input = 32; /* green */
/* Colour of prompt output */
static const int clr_prompt = 34; /* blue */
/* Colour of output that represents a PCRE2 API error */
static const int clr_api_error = 35; /* magenta */
/* Colour of error messages for the test script itself
(i.e. an error in the testing tool, not an API error) */
static const int clr_test_error = 31; /* red */
/* Colour of profiling information, which doesn't have a "right" answer */
static const int clr_profiling = 36; /* cyan */
/* No colour, for APIs that take a colour value */
static const int clr_none = -1;

enum { COLOUR_NEVER, COLOUR_ALWAYS, COLOUR_AUTO };
static int colour_setting = COLOUR_AUTO;
static int colour_last_fd = -1;
static BOOL colour_fd_interactive = FALSE;

static BOOL
should_print_colour(int clr, FILE* f)
{
if (f == NULL) return FALSE;
if (clr == clr_none) return FALSE;
if (colour_setting == COLOUR_NEVER) return FALSE;
if (colour_setting == COLOUR_AUTO)
  {
  if (fileno(f) != colour_last_fd)
    {
    colour_last_fd = fileno(f);
    colour_fd_interactive = INTERACTIVE(f);
    }
  if (!colour_fd_interactive) return FALSE;
  }
return TRUE;
}

/* Starts a block of colour (but only if colour is enabled). */
static void
colour_begin(int clr, FILE* f)
{
if (should_print_colour(clr, f))
  fprintf(f, "\x1b[%dm", clr);
}

/* Ends a block of colour (but only if colour is enabled). */
static void
colour_end(FILE* f)
{
colour_begin(0, f);
}

/* cfprintf is like fprintf but takes a colour to wrap its output. */
static int
cfprintf(int clr, FILE *file, const char* fmt, ...)
{
va_list args;
int ret;
va_start(args, fmt);
colour_begin(clr, file);
ret = vfprintf(file, fmt, args);
va_end(args);
colour_end(file);
return ret;
}



/*************************************************
*         Alternate character tables             *
*************************************************/

/* By default, the "tables" pointer in the compile context when calling
pcre2_compile() is not set (= NULL), thereby using the default tables of the
library. However, the tables modifier can be used to select alternate sets of
tables, for different kinds of testing. Note that the locale modifier also
adjusts the tables. */

/* This is the set of tables distributed as default with PCRE2. It recognizes
only ASCII characters. */

static const uint8_t tables1[] = {

/* This table is a lower casing table. */

    0,  1,  2,  3,  4,  5,  6,  7,
    8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23,
   24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39,
   40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55,
   56, 57, 58, 59, 60, 61, 62, 63,
   64, 97, 98, 99,100,101,102,103,
  104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,
  120,121,122, 91, 92, 93, 94, 95,
   96, 97, 98, 99,100,101,102,103,
  104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,
  120,121,122,123,124,125,126,127,
  128,129,130,131,132,133,134,135,
  136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,
  152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,
  168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,
  184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,
  200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,
  216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,
  232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,
  248,249,250,251,252,253,254,255,

/* This table is a case flipping table. */

    0,  1,  2,  3,  4,  5,  6,  7,
    8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23,
   24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39,
   40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55,
   56, 57, 58, 59, 60, 61, 62, 63,
   64, 97, 98, 99,100,101,102,103,
  104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,
  120,121,122, 91, 92, 93, 94, 95,
   96, 65, 66, 67, 68, 69, 70, 71,
   72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87,
   88, 89, 90,123,124,125,126,127,
  128,129,130,131,132,133,134,135,
  136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,
  152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,
  168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,
  184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,
  200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,
  216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,
  232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,
  248,249,250,251,252,253,254,255,

/* This table contains bit maps for various character classes. Each map is 32
bytes long and the bits run from the least significant end of each byte. The
classes that have their own maps are: space, xdigit, digit, upper, lower, word,
graph, print, punct, and cntrl. Other classes are built from combinations. */

  0x00,0x3e,0x00,0x00,0x01,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x03,
  0x7e,0x00,0x00,0x00,0x7e,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x03,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0xfe,0xff,0xff,0x07,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xfe,0xff,0xff,0x07,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x03,
  0xfe,0xff,0xff,0x87,0xfe,0xff,0xff,0x07,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0xfe,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0x00,0x00,0x00,0x00,0xfe,0xff,0x00,0xfc,
  0x01,0x00,0x00,0xf8,0x01,0x00,0x00,0x78,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

  0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,

/* This table identifies various classes of character by individual bits:
  0x01   white space character
  0x02   letter
  0x04   decimal digit
  0x08   hexadecimal digit
  0x10   alphanumeric or '_'
  0x80   regular expression metacharacter or binary zero
*/

  0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*   0-  7 */
  0x00,0x01,0x01,0x01,0x01,0x01,0x00,0x00, /*   8- 15 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*  16- 23 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /*  24- 31 */
  0x01,0x00,0x00,0x00,0x80,0x00,0x00,0x00, /*    - '  */
  0x80,0x80,0x80,0x80,0x00,0x00,0x80,0x00, /*  ( - /  */
  0x1c,0x1c,0x1c,0x1c,0x1c,0x1c,0x1c,0x1c, /*  0 - 7  */
  0x1c,0x1c,0x00,0x00,0x00,0x00,0x00,0x80, /*  8 - ?  */
  0x00,0x1a,0x1a,0x1a,0x1a,0x1a,0x1a,0x12, /*  @ - G  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  H - O  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  P - W  */
  0x12,0x12,0x12,0x80,0x80,0x00,0x80,0x10, /*  X - _  */
  0x00,0x1a,0x1a,0x1a,0x1a,0x1a,0x1a,0x12, /*  ` - g  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  h - o  */
  0x12,0x12,0x12,0x12,0x12,0x12,0x12,0x12, /*  p - w  */
  0x12,0x12,0x12,0x80,0x80,0x00,0x00,0x00, /*  x -127 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 128-135 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 136-143 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 144-151 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 152-159 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 160-167 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 168-175 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 176-183 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 184-191 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 192-199 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 200-207 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 208-215 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 216-223 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 224-231 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 232-239 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, /* 240-247 */
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};/* 248-255 */

/* This is a set of tables that came originally from a Windows user. It seems
to be at least an approximation of ISO 8859. In particular, there are
characters greater than 128 that are marked as spaces, letters, etc. */

static const uint8_t tables2[] = {
0,1,2,3,4,5,6,7,
8,9,10,11,12,13,14,15,
16,17,18,19,20,21,22,23,
24,25,26,27,28,29,30,31,
32,33,34,35,36,37,38,39,
40,41,42,43,44,45,46,47,
48,49,50,51,52,53,54,55,
56,57,58,59,60,61,62,63,
64,97,98,99,100,101,102,103,
104,105,106,107,108,109,110,111,
112,113,114,115,116,117,118,119,
120,121,122,91,92,93,94,95,
96,97,98,99,100,101,102,103,
104,105,106,107,108,109,110,111,
112,113,114,115,116,117,118,119,
120,121,122,123,124,125,126,127,
128,129,130,131,132,133,134,135,
136,137,138,139,140,141,142,143,
144,145,146,147,148,149,150,151,
152,153,154,155,156,157,158,159,
160,161,162,163,164,165,166,167,
168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,
184,185,186,187,188,189,190,191,
224,225,226,227,228,229,230,231,
232,233,234,235,236,237,238,239,
240,241,242,243,244,245,246,215,
248,249,250,251,252,253,254,223,
224,225,226,227,228,229,230,231,
232,233,234,235,236,237,238,239,
240,241,242,243,244,245,246,247,
248,249,250,251,252,253,254,255,
0,1,2,3,4,5,6,7,
8,9,10,11,12,13,14,15,
16,17,18,19,20,21,22,23,
24,25,26,27,28,29,30,31,
32,33,34,35,36,37,38,39,
40,41,42,43,44,45,46,47,
48,49,50,51,52,53,54,55,
56,57,58,59,60,61,62,63,
64,97,98,99,100,101,102,103,
104,105,106,107,108,109,110,111,
112,113,114,115,116,117,118,119,
120,121,122,91,92,93,94,95,
96,65,66,67,68,69,70,71,
72,73,74,75,76,77,78,79,
80,81,82,83,84,85,86,87,
88,89,90,123,124,125,126,127,
128,129,130,131,132,133,134,135,
136,137,138,139,140,141,142,143,
144,145,146,147,148,149,150,151,
152,153,154,155,156,157,158,159,
160,161,162,163,164,165,166,167,
168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,
184,185,186,187,188,189,190,191,
224,225,226,227,228,229,230,231,
232,233,234,235,236,237,238,239,
240,241,242,243,244,245,246,215,
248,249,250,251,252,253,254,223,
192,193,194,195,196,197,198,199,
200,201,202,203,204,205,206,207,
208,209,210,211,212,213,214,247,
216,217,218,219,220,221,222,255,
0,62,0,0,1,0,0,0,
0,0,0,0,0,0,0,0,
32,0,0,0,1,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,255,3,
126,0,0,0,126,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,255,3,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,12,2,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
254,255,255,7,0,0,0,0,
0,0,0,0,0,0,0,0,
255,255,127,127,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,254,255,255,7,
0,0,0,0,0,4,32,4,
0,0,0,128,255,255,127,255,
0,0,0,0,0,0,255,3,
254,255,255,135,254,255,255,7,
0,0,0,0,0,4,44,6,
255,255,127,255,255,255,127,255,
0,0,0,0,254,255,255,255,
255,255,255,255,255,255,255,127,
0,0,0,0,254,255,255,255,
255,255,255,255,255,255,255,255,
0,2,0,0,255,255,255,255,
255,255,255,255,255,255,255,127,
0,0,0,0,255,255,255,255,
255,255,255,255,255,255,255,255,
0,0,0,0,254,255,0,252,
1,0,0,248,1,0,0,120,
0,0,0,0,254,255,255,255,
0,0,128,0,0,0,128,0,
255,255,255,255,0,0,0,0,
0,0,0,0,0,0,0,128,
255,255,255,255,0,0,0,0,
0,0,0,0,0,0,0,0,
128,0,0,0,0,0,0,0,
0,1,1,0,1,1,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
1,0,0,0,128,0,0,0,
128,128,128,128,0,0,128,0,
28,28,28,28,28,28,28,28,
28,28,0,0,0,0,0,128,
0,26,26,26,26,26,26,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,128,128,0,128,16,
0,26,26,26,26,26,26,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,128,128,0,0,0,
0,0,0,0,0,1,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
0,0,0,0,0,0,0,0,
1,0,0,0,0,0,0,0,
0,0,18,0,0,0,0,0,
0,0,20,20,0,18,0,0,
0,20,18,0,0,0,0,0,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,0,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,18,
18,18,18,18,18,18,18,0,
18,18,18,18,18,18,18,18
};



/*************************************************
*              Callout state reset               *
*************************************************/

/* Several callout functions use global state to track progress. For convenience
this function resets all relevant state variables, for all the different
callouts. This ensures consistent execution when repeating a match. */

static void
reset_callout_state(void)
{
mallocs_called = 0;
first_callout = TRUE;
last_callout_mark = NULL;
callout_count = 0;
}



/*************************************************
*            Local memory functions              *
*************************************************/

/* Alternative memory functions, to test functionality. */

static void *my_malloc(size_t size, void *data)
{
void *block;

(void)data;

mallocs_called++;
if (mallocs_until_failure != INT_MAX && mallocs_until_failure-- <= 0)
  return NULL;

block = malloc(size);
if (show_memory && outfile != NULL)
  {
  if (block == NULL)
    {
    cfprintf(clr_test_error, outfile, "** malloc() failed for %" SIZ_FORM "\n", size);
    }
  else
    {
    cfprintf(clr_profiling, outfile, "malloc  %5" SIZ_FORM, size);
#ifdef DEBUG_SHOW_MALLOC_ADDRESSES
    cfprintf(clr_profiling, outfile, " %p", block);   /* Not portable */
#endif
    if (malloclistptr < MALLOCLISTSIZE)
      {
      malloclist[malloclistptr] = block;
      malloclistlength[malloclistptr++] = size;
      }
    else
      cfprintf(clr_profiling, outfile, " (not remembered)");
    fprintf(outfile, "\n");
    }
  }
return block;
}

static void my_free(void *block, void *data)
{
(void)data;

if (show_memory && outfile != NULL && block != NULL)
  {
  uint32_t i, j;
  BOOL found = FALSE;

  cfprintf(clr_profiling, outfile, "free");
  for (i = 0; i < malloclistptr; i++)
    {
    if (block == malloclist[i])
      {
      cfprintf(clr_profiling, outfile, "    %5" SIZ_FORM, malloclistlength[i]);
      malloclistptr--;
      for (j = i; j < malloclistptr; j++)
        {
        malloclist[j] = malloclist[j+1];
        malloclistlength[j] = malloclistlength[j+1];
        }
      found = TRUE;
      break;
      }
    }
  if (!found) cfprintf(clr_profiling, outfile, " unremembered block");
#ifdef DEBUG_SHOW_MALLOC_ADDRESSES
  cfprintf(clr_profiling, outfile, " %p", block);  /* Not portable */
#endif
  fprintf(outfile, "\n");
  }
free(block);
}



/*************************************************
*       Callback function for stack guard        *
*************************************************/

/* This is set up to be called from pcre2_compile() when the stackguard=n
modifier sets a value greater than zero. The test we do is whether the
parenthesis nesting depth is greater than the value set by the modifier.

Argument:  the current parenthesis nesting depth
Returns:   non-zero to kill the compilation
*/

static int
stack_guard(uint32_t depth, void *user_data)
{
(void)user_data;
return depth > pat_patctl.stackguard_test;
}



/*************************************************
*         EBCDIC support functions               *
*************************************************/

#if defined(EBCDIC)
static BOOL
printable(uint32_t c)
{
if ((c >= CHAR_a && c <= CHAR_i) ||
    (c >= CHAR_j && c <= CHAR_r) ||
    (c >= CHAR_s && c <= CHAR_z) ||
    (c >= CHAR_A && c <= CHAR_I) ||
    (c >= CHAR_J && c <= CHAR_R) ||
    (c >= CHAR_S && c <= CHAR_Z) ||
    (c >= CHAR_0 && c <= CHAR_9))
  return TRUE;

switch (c)
  {
  case CHAR_SPACE:
  case CHAR_EXCLAMATION_MARK:
  case CHAR_QUOTATION_MARK:
  case CHAR_NUMBER_SIGN:
  case CHAR_DOLLAR_SIGN:
  case CHAR_PERCENT_SIGN:
  case CHAR_AMPERSAND:
  case CHAR_APOSTROPHE:
  case CHAR_LEFT_PARENTHESIS:
  case CHAR_RIGHT_PARENTHESIS:
  case CHAR_ASTERISK:
  case CHAR_PLUS:
  case CHAR_COMMA:
  case CHAR_MINUS:
  case CHAR_DOT:
  case CHAR_SLASH:
  case CHAR_COLON:
  case CHAR_SEMICOLON:
  case CHAR_LESS_THAN_SIGN:
  case CHAR_EQUALS_SIGN:
  case CHAR_GREATER_THAN_SIGN:
  case CHAR_QUESTION_MARK:
  case CHAR_COMMERCIAL_AT:
  case CHAR_LEFT_SQUARE_BRACKET:
  case CHAR_BACKSLASH:
  case CHAR_RIGHT_SQUARE_BRACKET:
  case CHAR_CIRCUMFLEX_ACCENT:
  case CHAR_UNDERSCORE:
  case CHAR_GRAVE_ACCENT:
  case CHAR_LEFT_CURLY_BRACKET:
  case CHAR_VERTICAL_LINE:
  case CHAR_RIGHT_CURLY_BRACKET:
  case CHAR_TILDE:
  return TRUE;
  }

return FALSE;
}
#endif

#if defined(EBCDIC) && !EBCDIC_IO
static void
ascii_to_ebcdic_str(uint8_t *buf, size_t len)
{
for (size_t i = 0; i < len; ++i)
  buf[i] = ascii_to_ebcdic_1047[buf[i]];
}

static void
ebcdic_to_ascii_str(uint8_t *buf, size_t len)
{
for (size_t i = 0; i < len; ++i)
  buf[i] = ebcdic_1047_to_ascii[buf[i]];
}
#endif

#if defined(EBCDIC)
static uint32_t
ascii_to_ebcdic(uint32_t c)
{
return (c < 256)? ascii_to_ebcdic_1047[c] : c;
}

static uint32_t
ebcdic_to_ascii(uint32_t c)
{
return (c < 256)? ebcdic_1047_to_ascii[c] : c;
}
#endif



/*************************************************
*      Convert UTF-8 character to code point     *
*************************************************/

/* This function reads one or more bytes that represent a UTF-8 character,
and returns the codepoint of that character. Note that the function supports
the original UTF-8 definition of RFC 2279, allowing for values in the range 0
to 0x7fffffff, up to 6 bytes long. This makes it possible to generate
codepoints greater than 0x10ffff which are useful for testing PCRE2's error
checking, and also for generating 32-bit non-UTF data values above the UTF
limit.

Argument:
  utf8bytes   a pointer to the byte vector
  end         a pointer to the end of the byte vector
  vptr        a pointer to an int to receive the value

Returns:      >  0 => the number of bytes consumed
              -6 to 0 => malformed UTF-8 character at offset = (-return)
*/

static int
utf8_to_ord(PCRE2_SPTR8 utf8bytes, PCRE2_SPTR8 end, uint32_t *vptr)
{
uint32_t c = *utf8bytes++;
uint32_t d = c;
int i, j, s;

for (i = -1; i < 6; i++)               /* i is number of additional bytes */
  {
  if ((d & 0x80) == 0) break;
  d <<= 1;
  }

if (i == -1) { *vptr = c; return 1; }  /* ascii character */
if (i == 0 || i == 6) return 0;        /* invalid UTF-8 */

/* i now has a value in the range 1-5 */

s = 6*i;
d = (c & utf8_table3[i]) << s;

for (j = 0; j < i; j++)
  {
  if (utf8bytes >= end) return 0;

  c = *utf8bytes++;
  if ((c & 0xc0) != 0x80) return -(j+1);
  s -= 6;
  d |= (c & 0x3f) << s;
  }

/* Check that encoding was the correct unique one */

for (j = 0; j < (int)utf8_table1_size; j++)
  if (d <= (uint32_t)utf8_table1[j]) break;
if (j != i) return -(i+1);

/* Valid value */

*vptr = d;
return i+1;
}



#ifdef SUPPORT_PCRE2_16
/*************************************************
*      Convert UTF-16 character to code point     *
*************************************************/

/* This function reads one or more UTF-16 code units, and returns the
codepoint of that character.

Argument:
  utf16units  a pointer to the units vector
  end         a pointer to the end of the units vector
  vptr        a pointer to an int to receive the value

Returns:      > 0  => the number of 16-bit units consumed
              -1   => malformed UTF-16
*/

static int
utf16_to_ord(PCRE2_SPTR16 utf16units, PCRE2_SPTR16 end, uint32_t *vptr)
{
uint32_t c = *utf16units++;

if (c >= 0xdc00 && c <= 0xdfff) return -1;

if (c >= 0xd800 && c < 0xdc00)
  {
  uint32_t c2;

  if (utf16units >= end) return -1;

  c2 = *utf16units++;
  if (c2 < 0xdc00 || c2 > 0xdfff) return -1;
  *vptr = ((c & 0x3ff) << 10) + (c2 & 0x3ff) + 0x10000;
  return 2;
  }

*vptr = c;
return 1;
}
#endif  /* SUPPORT_PCRE2_16 */



/*************************************************
*       Convert character value to UTF-8         *
*************************************************/

/* This function takes an integer value in the range 0 - 0x7fffffff
and encodes it as a UTF-8 character in 0 to 6 bytes. It is needed even when the
8-bit library is not supported, to generate UTF-8 output for non-ASCII
characters.

Arguments:
  cvalue     the character value
  utf8bytes  pointer to buffer for result - at least 6 bytes long

Returns:     number of characters placed in the buffer
*/

static int
ord_to_utf8(uint32_t cvalue, uint8_t *utf8bytes)
{
int i, j;
if (cvalue > 0x7fffffffu)
  return -1;
for (i = 0; i < (int)utf8_table1_size; i++)
  if (cvalue <= (uint32_t)utf8_table1[i]) break;
utf8bytes += i;
for (j = i; j > 0; j--)
  {
  *utf8bytes-- = 0x80 | (cvalue & 0x3f);
  cvalue >>= 6;
  }
*utf8bytes = utf8_table2[i] | cvalue;
return i + 1;
}



/*************************************************
*             Print one character                *
*************************************************/

/* Print a single character either literally, or as a hex escape, and count how
many printed characters are used.

Arguments:
  c            the character
  utf          TRUE in UTF mode
  f            the FILE to print to, or NULL just to count characters

Returns:       number of characters written
*/

static int
pchar(uint32_t c, BOOL utf, FILE *f)
{
int n = 0;
char tempbuffer[16];

if (PRINTABLE(c))
  {
  c = CHAR_OUTPUT(c);
  if (f != NULL) fprintf(f, "%c", c);
  return 1;
  }

c = CHAR_OUTPUT_HEX(c);

if (c < 0x100)
  {
  if (utf)
    {
    if (f != NULL) fprintf(f, "\\x{%02x}", c);
    return 6;
    }
  else
    {
    if (f != NULL) fprintf(f, "\\x%02x", c);
    return 4;
    }
  }

if (f != NULL) n = fprintf(f, "\\x{%02x}", c);
  else n = snprintf(tempbuffer, sizeof(tempbuffer), "\\x{%02x}", c);

return n >= 0 ? n : 0;
}



/*************************************************
*           Expand input buffers                 *
*************************************************/

/* This function doubles the size of the input buffer and the buffer for
keeping an 8-bit copy of patterns (pbuffer8), and copies the current buffers to
the new ones.

Arguments: none
Returns:   nothing (aborts if malloc() fails)
*/

static void
expand_input_buffers(void)
{
size_t new_pbuffer8_size = 2*pbuffer8_size;
uint8_t *new_buffer = (uint8_t *)malloc(new_pbuffer8_size);
uint8_t *new_pbuffer8 = (uint8_t *)malloc(new_pbuffer8_size);

if (new_buffer == NULL || new_pbuffer8 == NULL)
  {
  cfprintf(clr_test_error, stderr, "pcre2test: malloc(%" SIZ_FORM ") failed\n",
          new_pbuffer8_size);
  exit(1);
  }

memcpy(new_buffer, buffer, pbuffer8_size);
memcpy(new_pbuffer8, pbuffer8, pbuffer8_size);

pbuffer8_size = new_pbuffer8_size;

free(buffer);
free(pbuffer8);

buffer = new_buffer;
pbuffer8 = new_pbuffer8;
}



/*************************************************
*        Read or extend an input line            *
*************************************************/

/* Input lines are read into buffer, but both patterns and data lines can be
continued over multiple input lines. In addition, if the buffer fills up, we
want to automatically expand it so as to be able to handle extremely large
lines that are needed for certain stress tests, although this is less likely
now that there are repetition features for both patterns and data. When the
input buffer is expanded, the other two buffers must also be expanded likewise,
and the contents of pbuffer, which are a copy of the input for callouts, must
be preserved (for when expansion happens for a data line). This is not the most
optimal way of handling this, but hey, this is just a test program!

Arguments:
  f            the file to read
  start        where in buffer to start (this *must* be within buffer)
  prompt       for stdin or readline()

Returns:       pointer to the start of new data
               could be a copy of start, or could be moved
               NULL if no data read and EOF reached
*/

static uint8_t *
extend_inputline(FILE *f, uint8_t *start, const char *prompt)
{
uint8_t *here = start;

for (;;)
  {
  size_t dlen;
  size_t rlen = (size_t)(pbuffer8_size - (here - buffer));

  /* If libreadline or libedit support is required, use readline() to read a
  line if the input is a terminal. Note that readline() removes the trailing
  newline, so we must put it back again, to be compatible with fgets(). */

#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
  if (INTERACTIVE(f))
    {
    char promptbuf[80];
    int snprintf_rc;
    char *s;
    if (should_print_colour(clr_prompt, stdout) &&
        (snprintf_rc = snprintf(promptbuf, sizeof(promptbuf), "\x1b[%dm%s\x1b[0m", clr_prompt, prompt)) > 0 &&
        snprintf_rc < (int)sizeof(promptbuf))
      s = readline(promptbuf);
    else
      s = readline(prompt);
    if (s == NULL) return (here == start)? NULL : start;
    dlen = strlen(s);
    if (dlen > rlen - 2)
      {
      cfprintf(clr_test_error, outfile, "** Interactive input exceeds buffer space\n");
      exit(1);
      }
    if (dlen > 0) add_history(s);
    memcpy(here, s, dlen);
    here[dlen] = '\n';
    here[dlen+1] = 0;
    free(s);
    return start;
    }
#endif

  if (rlen > 1000)
    {
    int rlen_trunc = (rlen > (unsigned)INT_MAX)? INT_MAX : (int)rlen;

    /* Read the next line by normal means, prompting if the file is a tty. */

    if (INTERACTIVE(f)) cfprintf(clr_prompt, stdout, "%s", prompt);
    if (fgets((char *)here, rlen_trunc, f) == NULL)
      return (here == start)? NULL : start;

    dlen = strlen((char *)here);
    here += dlen;

    /* Check for end of line reached. Take care not to read data from before
    start (dlen will be zero for a file starting with a binary zero). */

    if (here > start && here[-1] == '\n') return start;

    /* If we have not read a newline when reading a file, we have either filled
    the buffer or reached the end of the file. We can detect the former by
    checking that the string fills the buffer, and the latter by feof(). If
    neither of these is true, it means we read a binary zero which has caused
    strlen() to give a short length. This is a hard error because pcre2test
    expects to work with C strings. */

    if (dlen < (unsigned)rlen_trunc - 1 && !feof(f))
      {
      cfprintf(clr_test_error, outfile, "** Binary zero encountered in input\n");
      cfprintf(clr_test_error, outfile, "** pcre2test run abandoned\n");
      exit(1);
      }
    }

  else
    {
    size_t start_offset = start - buffer;
    size_t here_offset = here - buffer;
    expand_input_buffers();
    start = buffer + start_offset;
    here = buffer + here_offset;
    }
  }

PCRE2_UNREACHABLE(); /* Control never reaches here */
}



/*************************************************
*         Case-independent strncmp() function    *
*************************************************/

/*
Arguments:
  s         first string
  t         second string
  n         number of characters to compare

Returns:    < 0, = 0, or > 0, according to the comparison
*/

static int
strncmpic(const uint8_t *s, const uint8_t *t, size_t n)
{
if (n > 0) do
  {
  int c = tolower(*s++) - tolower(*t++);
  if (c != 0) return c;
  }
while (--n > 0);

return 0;
}



/*************************************************
*          Scan the main modifier list           *
*************************************************/

/* This function searches the modifier list for a long modifier name.

Argument:
  p         start of the name
  lenp      length of the name

Returns:    an index in the modifier list, or -1 on failure
*/

static int
scan_modifiers(const uint8_t *p, size_t len)
{
int bot = 0;
int top = MODLISTCOUNT;

while (top > bot)
  {
  int mid = (bot + top)/2;
  size_t mlen = strlen(modlist[mid].name);
  int c = strncmp((const char *)p, modlist[mid].name, (len < mlen)? len : mlen);
  if (c == 0)
    {
    if (len == mlen) return mid;
    c = len > mlen ? 1 : -1;
    }
  if (c > 0) bot = mid + 1; else top = mid;
  }

return -1;
}



/*************************************************
*     Determine how to print an error offset     *
*************************************************/

/* Each error code has an associated direction - does it refer
to the characters to the right or to the left of the offset?

Arguments:
  rc           the error code associated with the offset
  erroroffset  the offset in the pattern where the error occurred

Returns:      -1 if the error is unimplemented
               0 if the offset is to be ignored (should be zero)
               1 if the error refers to the left of the offset
               2 if the error refers to the right of the offset
               3 if the error refers to both sides of the offset
*/

static int
error_direction(int rc, PCRE2_SIZE erroroffset)
{
switch (rc)
  {
  /* These cases are all for things which don't affect a specific part of the
  pattern, and should always return zero offset. */

  case PCRE2_ERROR_NULL_PATTERN:
  case PCRE2_ERROR_BAD_OPTIONS:
  case PCRE2_ERROR_PATTERN_TOO_LARGE:
  case PCRE2_ERROR_HEAP_FAILED:
  case PCRE2_ERROR_UNICODE_NOT_SUPPORTED:
  case PCRE2_ERROR_PARENTHESES_STACK_CHECK:
  case PCRE2_ERROR_PATTERN_TOO_COMPLICATED:
  case PCRE2_ERROR_PATTERN_STRING_TOO_LONG:
  case PCRE2_ERROR_NO_SURROGATES_IN_UTF16:
  case PCRE2_ERROR_BAD_LITERAL_OPTIONS:
  case PCRE2_ERROR_PATTERN_COMPILED_SIZE_TOO_BIG:
  case PCRE2_ERROR_EXTRA_CASING_REQUIRES_UNICODE:
  case PCRE2_ERROR_TURKISH_CASING_REQUIRES_UTF:
  case PCRE2_ERROR_EXTRA_CASING_INCOMPATIBLE:
  return 0;

  /* A few exceptional cases use the errorofset to point rightwards. These are
  used when indicating an error in a capture group or lookaround parentheses.
  It is more user-friendly to identify the capture group by its start. */

  case PCRE2_ERROR_PARENTHESES_NEST_TOO_DEEP:
  case PCRE2_ERROR_LOOKBEHIND_NOT_FIXED_LENGTH:
  case PCRE2_ERROR_TOO_MANY_CONDITION_BRANCHES:
  case PCRE2_ERROR_LOOKBEHIND_TOO_COMPLICATED:
  case PCRE2_ERROR_LOOKBEHIND_INVALID_BACKSLASH_C:
  case PCRE2_ERROR_CALLOUT_NO_STRING_DELIMITER:
  case PCRE2_ERROR_QUERY_BARJX_NEST_TOO_DEEP:
  case PCRE2_ERROR_LOOKBEHIND_TOO_LONG:
  case PCRE2_ERROR_MAX_VAR_LOOKBEHIND_EXCEEDED:
  case PCRE2_ERROR_ECLASS_NEST_TOO_DEEP:
  return 2;

  /* The standard erroroffset should occur just after the affected portion of
  the pattern, unless there is a good reason not to do this. Consistency is
  good, but if there's a specific need then that's more important. */

  case PCRE2_ERROR_END_BACKSLASH:
  case PCRE2_ERROR_END_BACKSLASH_C:
  case PCRE2_ERROR_UNKNOWN_ESCAPE:
  case PCRE2_ERROR_QUANTIFIER_OUT_OF_ORDER:
  case PCRE2_ERROR_QUANTIFIER_TOO_BIG:
  case PCRE2_ERROR_MISSING_SQUARE_BRACKET:
  case PCRE2_ERROR_ESCAPE_INVALID_IN_CLASS:
  case PCRE2_ERROR_CLASS_RANGE_ORDER:
  case PCRE2_ERROR_QUANTIFIER_INVALID:
  case PCRE2_ERROR_INVALID_AFTER_PARENS_QUERY:
  case PCRE2_ERROR_POSIX_CLASS_NOT_IN_CLASS:
  case PCRE2_ERROR_POSIX_NO_SUPPORT_COLLATING:
  case PCRE2_ERROR_MISSING_CLOSING_PARENTHESIS:
  return 1;
  case PCRE2_ERROR_BAD_SUBPATTERN_REFERENCE:
  return 3; /* TODO I'd like to fix this, but some of the cases are _hard_ */
  case PCRE2_ERROR_MISSING_COMMENT_CLOSING:
  case PCRE2_ERROR_UNMATCHED_CLOSING_PARENTHESIS:
  case PCRE2_ERROR_MISSING_CONDITION_CLOSING:
  case PCRE2_ERROR_ZERO_RELATIVE_REFERENCE:
  case PCRE2_ERROR_CONDITION_ASSERTION_EXPECTED:
  case PCRE2_ERROR_BAD_RELATIVE_REFERENCE:
  case PCRE2_ERROR_UNKNOWN_POSIX_CLASS:
  case PCRE2_ERROR_CODE_POINT_TOO_BIG:
  case PCRE2_ERROR_UNSUPPORTED_ESCAPE_SEQUENCE:
  case PCRE2_ERROR_CALLOUT_NUMBER_TOO_BIG:
  case PCRE2_ERROR_MISSING_CALLOUT_CLOSING:
  case PCRE2_ERROR_ESCAPE_INVALID_IN_VERB:
  case PCRE2_ERROR_UNRECOGNIZED_AFTER_QUERY_P:
  case PCRE2_ERROR_MISSING_NAME_TERMINATOR:
  case PCRE2_ERROR_DUPLICATE_SUBPATTERN_NAME:
  case PCRE2_ERROR_INVALID_SUBPATTERN_NAME:
  case PCRE2_ERROR_UNICODE_PROPERTIES_UNAVAILABLE:
  case PCRE2_ERROR_MALFORMED_UNICODE_PROPERTY:
  case PCRE2_ERROR_UNKNOWN_UNICODE_PROPERTY:
  case PCRE2_ERROR_SUBPATTERN_NAME_TOO_LONG:
  case PCRE2_ERROR_TOO_MANY_NAMED_SUBPATTERNS:
  case PCRE2_ERROR_CLASS_INVALID_RANGE:
  case PCRE2_ERROR_OCTAL_BYTE_TOO_BIG:
  return 1;
  case PCRE2_ERROR_DEFINE_TOO_MANY_BRANCHES:
  return 2; /* TODO Not ideally placed; I'd like to fix this */
  case PCRE2_ERROR_BACKSLASH_O_MISSING_BRACE:
  case PCRE2_ERROR_BACKSLASH_G_SYNTAX:
  case PCRE2_ERROR_PARENS_QUERY_R_MISSING_CLOSING:
  case PCRE2_ERROR_VERB_UNKNOWN:
  case PCRE2_ERROR_SUBPATTERN_NUMBER_TOO_BIG:
  case PCRE2_ERROR_SUBPATTERN_NAME_EXPECTED:
  case PCRE2_ERROR_INVALID_OCTAL:
  case PCRE2_ERROR_SUBPATTERN_NAMES_MISMATCH:
  case PCRE2_ERROR_MARK_MISSING_ARGUMENT:
  case PCRE2_ERROR_INVALID_HEXADECIMAL:
  case PCRE2_ERROR_BACKSLASH_C_SYNTAX:
  case PCRE2_ERROR_BACKSLASH_K_SYNTAX:
  case PCRE2_ERROR_BACKSLASH_N_IN_CLASS:
  case PCRE2_ERROR_CALLOUT_STRING_TOO_LONG:
  case PCRE2_ERROR_UNICODE_DISALLOWED_CODE_POINT:
  return 1;
  case PCRE2_ERROR_VERB_NAME_TOO_LONG:
  case PCRE2_ERROR_BACKSLASH_U_CODE_POINT_TOO_BIG:
  case PCRE2_ERROR_MISSING_OCTAL_OR_HEX_DIGITS:
  case PCRE2_ERROR_VERSION_CONDITION_SYNTAX:
  case PCRE2_ERROR_CALLOUT_BAD_STRING_DELIMITER:
  case PCRE2_ERROR_BACKSLASH_C_CALLER_DISABLED:
  case PCRE2_ERROR_BACKSLASH_C_LIBRARY_DISABLED:
  case PCRE2_ERROR_SUPPORTED_ONLY_IN_UNICODE:
  case PCRE2_ERROR_INVALID_HYPHEN_IN_OPTIONS:
  case PCRE2_ERROR_ALPHA_ASSERTION_UNKNOWN:
  case PCRE2_ERROR_SCRIPT_RUN_NOT_AVAILABLE:
  case PCRE2_ERROR_TOO_MANY_CAPTURES:
  case PCRE2_ERROR_MISSING_OCTAL_DIGIT:
  return 1;
  case PCRE2_ERROR_BACKSLASH_K_IN_LOOKAROUND:
  return 3; /* TODO No erroroffset implemented yet, sadly */
  case PCRE2_ERROR_OVERSIZE_PYTHON_OCTAL:
  case PCRE2_ERROR_CALLOUT_CALLER_DISABLED:
  case PCRE2_ERROR_ECLASS_INVALID_OPERATOR:
  case PCRE2_ERROR_ECLASS_UNEXPECTED_OPERATOR:
  case PCRE2_ERROR_ECLASS_EXPECTED_OPERAND:
  case PCRE2_ERROR_ECLASS_MIXED_OPERATORS:
  case PCRE2_ERROR_ECLASS_HINT_SQUARE_BRACKET:
  case PCRE2_ERROR_PERL_ECLASS_UNEXPECTED_EXPR:
  case PCRE2_ERROR_PERL_ECLASS_EMPTY_EXPR:
  case PCRE2_ERROR_PERL_ECLASS_MISSING_CLOSE:
  case PCRE2_ERROR_PERL_ECLASS_UNEXPECTED_CHAR:
  case PCRE2_ERROR_EXPECTED_CAPTURE_GROUP:
  case PCRE2_ERROR_MISSING_OPENING_PARENTHESIS:
  case PCRE2_ERROR_MISSING_NUMBER_TERMINATOR:
  return 1;

  /* These two are a little fiddly. They can be triggered by passed-in options
  (when erroroffset is zero), or by text in the pattern "(*UTF)". We only
  indicate an pattern error in the latter case. */

  case PCRE2_ERROR_UTF_IS_DISABLED:
  case PCRE2_ERROR_UCP_IS_DISABLED:
  return (erroroffset > 0)? 1 : 0;

  case PCRE2_ERROR_UTF8_ERR1:
  case PCRE2_ERROR_UTF8_ERR2:
  case PCRE2_ERROR_UTF8_ERR3:
  case PCRE2_ERROR_UTF8_ERR4:
  case PCRE2_ERROR_UTF8_ERR5:
  case PCRE2_ERROR_UTF8_ERR6:
  case PCRE2_ERROR_UTF8_ERR7:
  case PCRE2_ERROR_UTF8_ERR8:
  case PCRE2_ERROR_UTF8_ERR9:
  case PCRE2_ERROR_UTF8_ERR10:
  case PCRE2_ERROR_UTF8_ERR11:
  case PCRE2_ERROR_UTF8_ERR12:
  case PCRE2_ERROR_UTF8_ERR13:
  case PCRE2_ERROR_UTF8_ERR14:
  case PCRE2_ERROR_UTF8_ERR15:
  case PCRE2_ERROR_UTF8_ERR16:
  case PCRE2_ERROR_UTF8_ERR17:
  case PCRE2_ERROR_UTF8_ERR18:
  case PCRE2_ERROR_UTF8_ERR19:
  case PCRE2_ERROR_UTF8_ERR20:
  case PCRE2_ERROR_UTF8_ERR21:
  case PCRE2_ERROR_UTF16_ERR1:
  case PCRE2_ERROR_UTF16_ERR2:
  case PCRE2_ERROR_UTF16_ERR3:
  case PCRE2_ERROR_UTF32_ERR1:
  case PCRE2_ERROR_UTF32_ERR2:
  return 2;
  }

return -1;
}



#ifdef SUPPORT_PCRE2_8
/*************************************************
*             Show something in a list           *
*************************************************/

/* This function just helps to keep the code that uses it tidier. It's used for
various lists of things where there needs to be introductory text before the
first item. As these calls are all in the POSIX-support code, they happen only
when 8-bit mode is supported. */

static void
prmsg(const char **msg, const char *s)
{
cfprintf(clr_test_error, outfile, "%s %s", *msg, s);
*msg = "";
}
#endif  /* SUPPORT_PCRE2_8 */



/*************************************************
*                Show control bits               *
*************************************************/

/* Called for mutually exclusive controls and for unsupported POSIX controls.
Because the bits are unique, this can be used for both pattern and data control
words.

Arguments:
  clr         colour for output
  controls    control bits
  controls2   more control bits
  before      text to print before

Returns:      nothing
*/

static void
show_controls(int clr, uint32_t controls, uint32_t controls2, const char *before)
{
cfprintf(clr, outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
  before,
  ((controls & CTL_AFTERTEXT) != 0)? " aftertext" : "",
  ((controls & CTL_ALLAFTERTEXT) != 0)? " allaftertext" : "",
  ((controls & CTL_ALLCAPTURES) != 0)? " allcaptures" : "",
  ((controls & CTL_ALLUSEDTEXT) != 0)? " allusedtext" : "",
  ((controls2 & CTL2_ALLVECTOR) != 0)? " allvector" : "",
  ((controls & CTL_ALTGLOBAL) != 0)? " altglobal" : "",
  ((controls & CTL_BINCODE) != 0)? " bincode" : "",
  ((controls2 & CTL2_BSR_SET) != 0)? " bsr" : "",
  ((controls & CTL_CALLOUT_CAPTURE) != 0)? " callout_capture" : "",
  ((controls2 & CTL2_CALLOUT_EXTRA) != 0)? " callout_extra" : "",
  ((controls & CTL_CALLOUT_INFO) != 0)? " callout_info" : "",
  ((controls & CTL_CALLOUT_NONE) != 0)? " callout_none" : "",
  ((controls2 & CTL2_CALLOUT_NO_WHERE) != 0)? " callout_no_where" : "",
  ((controls & CTL_DFA) != 0)? " dfa" : "",
  ((controls & CTL_EXPAND) != 0)? " expand" : "",
  ((controls & CTL_FINDLIMITS) != 0)? " find_limits" : "",
  ((controls & CTL_FINDLIMITS_NOHEAP) != 0)? " find_limits_noheap" : "",
  ((controls2 & CTL2_FRAMESIZE) != 0)? " framesize" : "",
  ((controls & CTL_FULLBINCODE) != 0)? " fullbincode" : "",
  ((controls & CTL_GETALL) != 0)? " getall" : "",
  ((controls & CTL_GLOBAL) != 0)? " global" : "",
  ((controls2 & CTL2_HEAPFRAMES_SIZE) != 0)? " heapframes_size" : "",
  ((controls & CTL_HEXPAT) != 0)? " hex" : "",
  ((controls & CTL_INFO) != 0)? " info" : "",
  ((controls & CTL_JITFAST) != 0)? " jitfast" : "",
  ((controls & CTL_JITVERIFY) != 0)? " jitverify" : "",
  ((controls & CTL_MARK) != 0)? " mark" : "",
  ((controls & CTL_MEMORY) != 0)? " memory" : "",
  ((controls2 & CTL2_NL_SET) != 0)? " newline" : "",
  ((controls & CTL_NULLCONTEXT) != 0)? " null_context" : "",
  ((controls2 & CTL2_NULL_REPLACEMENT) != 0)? " null_replacement" : "",
  ((controls2 & CTL2_NULL_SUBJECT) != 0)? " null_subject" : "",
  ((controls2 & CTL2_NULL_SUBSTITUTE_MATCH_DATA) != 0)? " null_substitute_match_data" : "",
  ((controls & CTL_POSIX) != 0)? " posix" : "",
  ((controls & CTL_POSIX_NOSUB) != 0)? " posix_nosub" : "",
  ((controls & CTL_PUSH) != 0)? " push" : "",
  ((controls & CTL_PUSHCOPY) != 0)? " pushcopy" : "",
  ((controls & CTL_PUSHTABLESCOPY) != 0)? " pushtablescopy" : "",
  ((controls & CTL_STARTCHAR) != 0)? " startchar" : "",
  ((controls2 & CTL2_SUBSTITUTE_CALLOUT) != 0)? " substitute_callout" : "",
  ((controls2 & CTL2_SUBSTITUTE_CASE_CALLOUT) != 0)? " substitute_case_callout" : "",
  ((controls2 & CTL2_SUBSTITUTE_EXTENDED) != 0)? " substitute_extended" : "",
  ((controls2 & CTL2_SUBSTITUTE_LITERAL) != 0)? " substitute_literal" : "",
  ((controls2 & CTL2_SUBSTITUTE_MATCHED) != 0)? " substitute_matched" : "",
  ((controls2 & CTL2_SUBSTITUTE_OVERFLOW_LENGTH) != 0)? " substitute_overflow_length" : "",
  ((controls2 & CTL2_SUBSTITUTE_REPLACEMENT_ONLY) != 0)? " substitute_replacement_only" : "",
  ((controls2 & CTL2_SUBSTITUTE_UNKNOWN_UNSET) != 0)? " substitute_unknown_unset" : "",
  ((controls2 & CTL2_SUBSTITUTE_UNSET_EMPTY) != 0)? " substitute_unset_empty" : "",
  ((controls & CTL_USE_LENGTH) != 0)? " use_length" : "",
  ((controls & CTL_UTF8_INPUT) != 0)? " utf8_input" : "",
  ((controls & CTL_ZERO_TERMINATE) != 0)? " zero_terminate" : "");
}



/*************************************************
*                Show compile options            *
*************************************************/

/* Called from show_pattern_info() and for unsupported POSIX options.

Arguments:
  clr         colour for output
  options     an options word
  before      text to print before
  after       text to print after

Returns:      nothing
*/

static void
show_compile_options(int clr, uint32_t options, const char *before, const char *after)
{
if (options == 0) cfprintf(clr, outfile, "%s <none>%s", before, after);
else cfprintf(clr, outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
  before,
  ((options & PCRE2_ALT_BSUX) != 0)? " alt_bsux" : "",
  ((options & PCRE2_ALT_CIRCUMFLEX) != 0)? " alt_circumflex" : "",
  ((options & PCRE2_ALT_EXTENDED_CLASS) != 0)? " alt_extended_class" : "",
  ((options & PCRE2_ALT_VERBNAMES) != 0)? " alt_verbnames" : "",
  ((options & PCRE2_ALLOW_EMPTY_CLASS) != 0)? " allow_empty_class" : "",
  ((options & PCRE2_ANCHORED) != 0)? " anchored" : "",
  ((options & PCRE2_AUTO_CALLOUT) != 0)? " auto_callout" : "",
  ((options & PCRE2_CASELESS) != 0)? " caseless" : "",
  ((options & PCRE2_DOLLAR_ENDONLY) != 0)? " dollar_endonly" : "",
  ((options & PCRE2_DOTALL) != 0)? " dotall" : "",
  ((options & PCRE2_DUPNAMES) != 0)? " dupnames" : "",
  ((options & PCRE2_ENDANCHORED) != 0)? " endanchored" : "",
  ((options & PCRE2_EXTENDED) != 0)? " extended" : "",
  ((options & PCRE2_EXTENDED_MORE) != 0)? " extended_more" : "",
  ((options & PCRE2_FIRSTLINE) != 0)? " firstline" : "",
  ((options & PCRE2_LITERAL) != 0)? " literal" : "",
  ((options & PCRE2_MATCH_INVALID_UTF) != 0)? " match_invalid_utf" : "",
  ((options & PCRE2_MATCH_UNSET_BACKREF) != 0)? " match_unset_backref" : "",
  ((options & PCRE2_MULTILINE) != 0)? " multiline" : "",
  ((options & PCRE2_NEVER_BACKSLASH_C) != 0)? " never_backslash_c" : "",
  ((options & PCRE2_NEVER_UCP) != 0)? " never_ucp" : "",
  ((options & PCRE2_NEVER_UTF) != 0)? " never_utf" : "",
  ((options & PCRE2_NO_AUTO_CAPTURE) != 0)? " no_auto_capture" : "",
  ((options & PCRE2_NO_AUTO_POSSESS) != 0)? " no_auto_possess" : "",
  ((options & PCRE2_NO_DOTSTAR_ANCHOR) != 0)? " no_dotstar_anchor" : "",
  ((options & PCRE2_NO_UTF_CHECK) != 0)? " no_utf_check" : "",
  ((options & PCRE2_NO_START_OPTIMIZE) != 0)? " no_start_optimize" : "",
  ((options & PCRE2_UCP) != 0)? " ucp" : "",
  ((options & PCRE2_UNGREEDY) != 0)? " ungreedy" : "",
  ((options & PCRE2_USE_OFFSET_LIMIT) != 0)? " use_offset_limit" : "",
  ((options & PCRE2_UTF) != 0)? " utf" : "",
  after);
}


/*************************************************
*           Show compile extra options           *
*************************************************/

/* Called from show_pattern_info() and for unsupported POSIX options.

Arguments:
  clr         colour for output
  options     an options word
  before      text to print before
  after       text to print after

Returns:      nothing
*/

static void
show_compile_extra_options(int clr, uint32_t options, const char *before,
  const char *after)
{
if (options == 0) cfprintf(clr, outfile, "%s <none>%s", before, after);
else cfprintf(clr, outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
  before,
  ((options & PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK) != 0) ? " allow_lookaround_bsk" : "",
  ((options & PCRE2_EXTRA_ALLOW_SURROGATE_ESCAPES) != 0)? " allow_surrogate_escapes" : "",
  ((options & PCRE2_EXTRA_ALT_BSUX) != 0)? " alt_bsux" : "",
  ((options & PCRE2_EXTRA_ASCII_BSD) != 0)? " ascii_bsd" : "",
  ((options & PCRE2_EXTRA_ASCII_BSS) != 0)? " ascii_bss" : "",
  ((options & PCRE2_EXTRA_ASCII_BSW) != 0)? " ascii_bsw" : "",
  ((options & PCRE2_EXTRA_ASCII_DIGIT) != 0)? " ascii_digit" : "",
  ((options & PCRE2_EXTRA_ASCII_POSIX) != 0)? " ascii_posix" : "",
  ((options & PCRE2_EXTRA_BAD_ESCAPE_IS_LITERAL) != 0)? " bad_escape_is_literal" : "",
  ((options & PCRE2_EXTRA_CASELESS_RESTRICT) != 0)? " caseless_restrict" : "",
  ((options & PCRE2_EXTRA_ESCAPED_CR_IS_LF) != 0)? " escaped_cr_is_lf" : "",
  ((options & PCRE2_EXTRA_MATCH_WORD) != 0)? " match_word" : "",
  ((options & PCRE2_EXTRA_MATCH_LINE) != 0)? " match_line" : "",
  ((options & PCRE2_EXTRA_NEVER_CALLOUT) != 0)? " never_callout" : "",
  ((options & PCRE2_EXTRA_NO_BS0) != 0)? " no_bs0" : "",
  ((options & PCRE2_EXTRA_PYTHON_OCTAL) != 0)? " python_octal" : "",
  ((options & PCRE2_EXTRA_TURKISH_CASING) != 0)? " turkish_casing" : "",
  after);
}


/*************************************************
*           Show optimization flags              *
*************************************************/

/*
Arguments:
  clr         colour for output
  flags       an options word
  before      text to print before
  after       text to print after

Returns:      nothing
*/

static void
show_optimize_flags(int clr, uint32_t flags, const char *before, const char *after)
{
if (flags == 0) cfprintf(clr, outfile, "%s<none>%s", before, after);
else cfprintf(clr, outfile, "%s%s%s%s%s%s%s",
  before,
  ((flags & PCRE2_OPTIM_AUTO_POSSESS) != 0) ? "auto_possess" : "",
  ((flags & PCRE2_OPTIM_AUTO_POSSESS) != 0 && (flags >> 1) != 0) ? "," : "",
  ((flags & PCRE2_OPTIM_DOTSTAR_ANCHOR) != 0) ? "dotstar_anchor" : "",
  ((flags & PCRE2_OPTIM_DOTSTAR_ANCHOR) != 0 && (flags >> 2) != 0) ? "," : "",
  ((flags & PCRE2_OPTIM_START_OPTIMIZE) != 0) ? "start_optimize" : "",
  after);
}


#ifdef SUPPORT_PCRE2_8
/*************************************************
*                Show match options              *
*************************************************/

/* Called for unsupported POSIX options. */

static void
show_match_options(int clr, uint32_t options)
{
cfprintf(clr, outfile, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
  ((options & PCRE2_ANCHORED) != 0)? " anchored" : "",
  ((options & PCRE2_COPY_MATCHED_SUBJECT) != 0)? " copy_matched_subject" : "",
  ((options & PCRE2_DFA_RESTART) != 0)? " dfa_restart" : "",
  ((options & PCRE2_DFA_SHORTEST) != 0)? " dfa_shortest" : "",
  ((options & PCRE2_DISABLE_RECURSELOOP_CHECK) != 0)? " disable_recurseloop_check" : "",
  ((options & PCRE2_ENDANCHORED) != 0)? " endanchored" : "",
  ((options & PCRE2_NO_JIT) != 0)? " no_jit" : "",
  ((options & PCRE2_NO_UTF_CHECK) != 0)? " no_utf_check" : "",
  ((options & PCRE2_NOTBOL) != 0)? " notbol" : "",
  ((options & PCRE2_NOTEMPTY) != 0)? " notempty" : "",
  ((options & PCRE2_NOTEMPTY_ATSTART) != 0)? " notempty_atstart" : "",
  ((options & PCRE2_NOTEOL) != 0)? " noteol" : "",
  ((options & PCRE2_PARTIAL_HARD) != 0)? " partial_hard" : "",
  ((options & PCRE2_PARTIAL_SOFT) != 0)? " partial_soft" : "");
}
#endif  /* SUPPORT_PCRE2_8 */



/*************************************************
*        Open file for save/load commands        *
*************************************************/

/* This function decodes the file name and opens the file.

Arguments:
  buffptr     point after the #command
  mode        open mode
  fptr        points to the FILE variable
  name        name of # command

Returns:      PR_OK or PR_ABEND
*/

static int
open_file(uint8_t *buffptr, const char *mode, FILE **fptr, const char *name)
{
char *endf;
char *filename = (char *)buffptr;
while (isspace((unsigned char)*filename)) filename++;
endf = filename + strlen(filename);
while (endf > filename && isspace((unsigned char)endf[-1])) endf--;

if (endf == filename)
  {
  cfprintf(clr_test_error, outfile, "** File name expected after %s\n", name);
  return PR_ABEND;
  }

*endf = 0;
*fptr = fopen((const char *)filename, mode);
if (*fptr == NULL)
  {
  cfprintf(clr_test_error, outfile, "** Failed to open \"%s\": %s\n", filename, strerror(errno));
  return PR_ABEND;
  }

return PR_OK;
}




/*************************************************
*       Substitute case callout transform        *
*************************************************/

/* Function to implement our test-only custom case mappings.
To ease implementation, we only work in the ASCII range (so that we don't need
to read & write UTF sequences).
However, we aim to implement case mappings which fairly well represent the range
of interesting behaviours that exist for Unicode codepoints. */

static BOOL
case_transform(int to_case, int num_in, int *num_read, int *num_write,
  uint32_t *c1, uint32_t *c2)
{
/* Let's have one character which aborts the substitution. */
if (*c1 == CHAR_EXCLAMATION_MARK) return FALSE;

/* Default behaviour is to read one character, and write back that same one
character (treating all characters as "uncased"). */
*num_read = *num_write = 1;

/* Add a normal case pair 'a' (l) <-> 'B' (t,u). Standard ASCII letter
behaviour, but with switched letters for testing. */
if (*c1 == CHAR_a && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_B;
else if (*c1 == CHAR_B && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_a;

/* Add a titlecased triplet 'd' (l) <-> 'D' (t) <-> 'Z' (u). Example: the
'dz'/'Dz'/'DZ' ligature character ("Latin Small Letter DZ" <-> "Latin Capital
Letter D with Small Letter Z" <-> "Latin Capital Letter DZ"). */
else if (*c1 == CHAR_d && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = (to_case == PCRE2_SUBSTITUTE_CASE_TITLE_FIRST)? CHAR_D : CHAR_Z;
else if (*c1 == CHAR_D && to_case != PCRE2_SUBSTITUTE_CASE_TITLE_FIRST)
  *c1 = (to_case == PCRE2_SUBSTITUTE_CASE_LOWER)? CHAR_d : CHAR_Z;
else if (*c1 == CHAR_Z && to_case != PCRE2_SUBSTITUTE_CASE_UPPER)
  *c1 = (to_case == PCRE2_SUBSTITUTE_CASE_LOWER)? CHAR_d : CHAR_D;

/* Expands when uppercased. Example: Esszet 'f' <-> 'SS'. */
else if (*c1 == CHAR_f && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  {
  *c1 = CHAR_S;
  *c2 = CHAR_S;
  *num_write = 2;
  }
else if (*c1 == CHAR_s && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_S;
else if (*c1 == CHAR_S && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_s;

/* Expanding and contracting characters, 'o' <-> 'OO'. You can get this purely
due to UTF-8 encoding length, for example uppercase Omega (3 bytes in UTF-8)
lowercases to 2 bytes in UTF-8. */
else if (num_in == 2 && *c1 == CHAR_O && *c2 == CHAR_O &&
         to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  {
  *c1 = CHAR_o;
  *num_read = 2;
  }
else if (*c1 == CHAR_o && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  {
  *c1 = CHAR_O;
  *c2 = CHAR_O;
  *num_write = 2;
  }
else if (num_in == 2 && *c1 == CHAR_p && *c2 == CHAR_p &&
         to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  {
  *c1 = CHAR_P;
  *num_read = 2;
  }
else if (*c1 == CHAR_P && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  {
  *c1 = CHAR_p;
  *c2 = CHAR_p;
  *num_write = 2;
  }

/* Use 'l' -> 'Mn' or 'MN' as an expanding ligature, like 'fi' -> 'Fi' ->
'FI'. */
else if (*c1 == CHAR_l && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  {
  *c1 = CHAR_M;
  *c2 = (to_case == PCRE2_SUBSTITUTE_CASE_TITLE_FIRST)? CHAR_n : CHAR_N;
  *num_write = 2;
  }
else if (*c1 == CHAR_M && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_m;
else if (*c1 == CHAR_m && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_M;
else if (*c1 == CHAR_N && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_n;
else if (*c1 == CHAR_n && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_N;

/* An example of a context-dependent mapping, the Greek Sigma. It lowercases
depending on the following character. Use 'c'/'k' -> 'K'. */
else if ((*c1 == CHAR_c || *c1 == CHAR_k) &&
         to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_K;
else if (*c1 == CHAR_K && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = (num_in == 1 || *c2 == CHAR_SPACE)? CHAR_c : CHAR_k;

/* An example of a context-dependent multi mapping, the Dutch IJ. When those
letters appear together, they titlecase 'ij' (l) <-> 'IJ' (t) <-> 'IJ' (u).
Namely, English titlecasing of 'ijnssel' would be 'Ijnssel' (just uppercase the
first letter), but the Dutch rule is 'IJnssel'. */
else if (num_in == 2 && (*c1 == CHAR_i || *c1 == CHAR_I) &&
         (*c2 == CHAR_j || *c2 == CHAR_J) &&
         to_case == PCRE2_SUBSTITUTE_CASE_TITLE_FIRST)
  {
  *c1 = CHAR_I;
  *c2 = CHAR_J;
  *num_read = 2;
  *num_write = 2;
  }
else if (*c1 == CHAR_i && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_I;
else if (*c1 == CHAR_I && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_i;
else if (*c1 == CHAR_j && to_case != PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_J;
else if (*c1 == CHAR_J && to_case == PCRE2_SUBSTITUTE_CASE_LOWER)
  *c1 = CHAR_j;

return TRUE;
}



/*************************************************
*            Show an entire ovector              *
*************************************************/

/* This function is called after partial matching or match failure, when the
"allvector" modifier is set. It is a means of checking the contents of the
entire ovector, to ensure no modification of fields that should be unchanged.

Arguments:
  ovector      points to the ovector
  oveccount    number of pairs

Returns:       nothing
*/

static void
show_ovector(PCRE2_SIZE *ovector, uint32_t oveccount)
{
uint32_t i;
for (i = 0; i < 2*oveccount; i += 2)
  {
  PCRE2_SIZE start = ovector[i];
  PCRE2_SIZE end = ovector[i+1];

  fprintf(outfile, "%2d: ", i/2);
  if (start == PCRE2_UNSET && end == PCRE2_UNSET)
    fprintf(outfile, "<unset>\n");
  else if (start == JUNK_OFFSET && end == JUNK_OFFSET)
    fprintf(outfile, "<unchanged>\n");
  else
    fprintf(outfile, "%ld %ld\n", (unsigned long int)start,
      (unsigned long int)end);
  }
}



/*************************************************
*            Mode-dependent code                 *
*************************************************/

/* All the mode-independent utilities should go above this section, so that
the mode-dependent code can use them.

The structure is:
   main
     -> calls into usage, command line parsing, top-level dispatch
       -> calls into mode-dependent code to handle input lines
         -> calls into mode-independent utilities

The ordering of the code blocks is therefore:
  - mode-independent utilities (ABOVE THIS SECTION)
  - mode-dependent code to handle input lines (THIS SECTION)
  - usage, command line parsing, top-level dispatch (NEXT SECTION)
  - main (AT THE BOTTOM)
*/

/* --- Repeated pre-processor inclusions to build the mode-dependent code -- */

#undef PCRE2_SUFFIX

#ifdef SUPPORT_PCRE2_8
#define PCRE2_CODE_UNIT_WIDTH 8
#define PCRE2_SUFFIX(a) G(a,8)
#include "pcre2_intmodedep.h"
#include "pcre2test_inc.h"
#undef PCRE2_CODE_UNIT_WIDTH
#undef PCRE2_SUFFIX
#endif

#ifdef SUPPORT_PCRE2_16
#define PCRE2_CODE_UNIT_WIDTH 16
#define PCRE2_SUFFIX(a) G(a,16)
#include "pcre2_intmodedep.h"
#include "pcre2test_inc.h"
#undef PCRE2_CODE_UNIT_WIDTH
#undef PCRE2_SUFFIX
#endif

#ifdef SUPPORT_PCRE2_32
#define PCRE2_CODE_UNIT_WIDTH 32
#define PCRE2_SUFFIX(a) G(a,32)
#include "pcre2_intmodedep.h"
#include "pcre2test_inc.h"
#undef PCRE2_CODE_UNIT_WIDTH
#undef PCRE2_SUFFIX
#endif

#define PCRE2_CODE_UNIT_WIDTH 0
#include "pcre2_intmodedep.h"  /* Clear out the stale macros */
#undef PCRE2_CODE_UNIT_WIDTH

#define PCRE2_SUFFIX(a) a

/* --------------------------- Static variables ---------------------------- */

/* Declared after mode-dependent code. */

static int test_mode = DEFAULT_TEST_MODE;

/* -------------------- Mode-dependent dispatch helper --------------------- */

/* When there are three supported bit widths, use a three-way ternary. */

#if defined(SUPPORT_PCRE2_8) && defined(SUPPORT_PCRE2_16) && defined(SUPPORT_PCRE2_32)

#define DISPATCH(opt_ret, fname, fargs) opt_ret \
  ((test_mode == PCRE2TEST_MODE_8)? G(fname,8) fargs : \
   (test_mode == PCRE2TEST_MODE_16)? G(fname,16) fargs : \
   G(fname,32) fargs)

#elif (defined(SUPPORT_PCRE2_8) + defined(SUPPORT_PCRE2_16) + \
       defined(SUPPORT_PCRE2_32)) == 2

/* With some macro trickery, we can make a single definition work to dispatch
between any two bit widths. */

#if defined(SUPPORT_PCRE2_32) && defined(SUPPORT_PCRE2_16)
#define BITONE 32
#define BITTWO 16
#elif defined(SUPPORT_PCRE2_32) && defined(SUPPORT_PCRE2_8)
#define BITONE 32
#define BITTWO 8
#else
#define BITONE 16
#define BITTWO 8
#endif

#define DISPATCH(opt_ret, fname, fargs) opt_ret \
  ((test_mode == G(PCRE2TEST_MODE_,BITONE))? G(fname,BITONE) fargs : \
   G(fname,BITTWO) fargs)

#else  /* Only one bit width supported */

#if defined(SUPPORT_PCRE2_32)
#define BITONE 32
#elif defined(SUPPORT_PCRE2_16)
#define BITONE 16
#else
#define BITONE 8
#endif

#define DISPATCH(opt_ret, fname, fargs) \
  opt_ret (G(fname,BITONE) fargs)

#endif

/* -------------------- Mode-dependent dispatch wrappers ------------------- */

static int jit_compile_test(void)
{
DISPATCH(return, pcre2_jit_compile_, (NULL, PCRE2_JIT_TEST_ALLOC));
}

static int pcre2_config(uint32_t what, void *where)
{
DISPATCH(return, pcre2_config_, (what, where));
}

static void config_str(uint32_t what, char *where)
{
DISPATCH(, config_str_, (what, where));
}

static BOOL decode_modifiers(uint8_t *p, int ctx, patctl *pctl, datctl *dctl)
{
DISPATCH(return, decode_modifiers_, (p, ctx, pctl, dctl));
}

static BOOL
print_error_message_file(FILE *file, int errorcode, const char *before,
  const char *after, BOOL badcode_ok)
{
DISPATCH(return, print_error_message_file_, \
  (file, errorcode, before, after, badcode_ok));
}

static int process_command(void)
{
DISPATCH(return, process_command_, ());
}

static int process_pattern(void)
{
DISPATCH(return, process_pattern_, ());
}

static BOOL have_active_pattern(void)
{
DISPATCH(return, have_active_pattern_, ());
}

static void free_active_pattern(void)
{
DISPATCH(, free_active_pattern_, ());
}

static int process_data(void)
{
DISPATCH(return, process_data_, ());
}

static void init_globals(void)
{
DISPATCH(, init_globals_, ());
}

static void free_globals(void)
{
DISPATCH(, free_globals_, ());
}

static void unittest(void)
{
DISPATCH(, unittest_, ());
}

#undef DISPATCH
#undef BITONE
#undef BITTWO



/*************************************************
*               Print PCRE2 version              *
*************************************************/

static void
print_version(FILE *f, BOOL include_mode)
{
char buf[VERSION_SIZE];
config_str(PCRE2_CONFIG_VERSION, buf);
fprintf(f, "PCRE2 version %s", buf);
if (include_mode)
  {
  fprintf(f, " (%d-bit)", test_mode);
  }
fprintf(f, "\n");
}



/*************************************************
*               Print Unicode version            *
*************************************************/

static void
print_unicode_version(FILE *f)
{
char buf[VERSION_SIZE];
config_str(PCRE2_CONFIG_UNICODE_VERSION, buf);
fprintf(f, "Unicode version %s", buf);
}



/*************************************************
*               Print JIT target                 *
*************************************************/

static void
print_jit_target(FILE *f)
{
char buf[VERSION_SIZE];
config_str(PCRE2_CONFIG_JITTARGET, buf);
fputs(buf, f);
}



/*************************************************
*       Print newline configuration              *
*************************************************/

/* Output is always to stdout.

Arguments:
  rc         the return code from PCRE2_CONFIG_NEWLINE
  isc        TRUE if called from "-C newline"
Returns:     nothing
*/

static void
print_newline_config(uint32_t optval, BOOL isc)
{
if (!isc) printf("  Default newline sequence is ");
if (optval < sizeof(newlines)/sizeof(char *))
  printf("%s\n", newlines[optval]);
else
  printf("a non-standard value: %d\n", optval);
}



/*************************************************
*             Usage function                     *
*************************************************/

static void
usage(void)
{
printf("Usage:     pcre2test [options] [<input file> [<output file>]]\n\n");
printf("Input and output default to stdin and stdout.\n");
#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
printf("If input is a terminal, readline() is used to read from it.\n");
#else
printf("This version of pcre2test is not linked with readline().\n");
#endif
printf("\nOptions:\n");
#ifdef SUPPORT_PCRE2_8
printf("  -8            use the 8-bit library\n");
#endif
#ifdef SUPPORT_PCRE2_16
printf("  -16           use the 16-bit library\n");
#endif
#ifdef SUPPORT_PCRE2_32
printf("  -32           use the 32-bit library\n");
#endif
printf("  -ac           set default pattern modifier PCRE2_AUTO_CALLOUT\n");
printf("  -AC           as -ac, but also set subject 'callout_extra' modifier\n");
printf("  -b            set default pattern modifier 'fullbincode'\n");
printf("  -C            show PCRE2 compile-time options and exit\n");
printf("  -C arg        show a specific compile-time option and exit with its\n");
printf("                  value if numeric (else 0). The arg can be:\n");
printf("     backslash-C    use of \\C is enabled [0, 1]\n");
printf("     bsr            \\R type [ANYCRLF, ANY]\n");
printf("     ebcdic         compiled for EBCDIC character code [0, 1]\n");
printf("     ebcdic-io      if compiled for EBCDIC, whether pcre2test's input\n");
printf("                      and output is EBCDIC or ASCII [0, 1]\n");
printf("     ebcdic-nl25    if compiled for EBCDIC, whether NL is 0x25 [0, 1]\n");
printf("     jit            just-in-time compiler supported [0, 1]\n");
printf("     jitusable      test JIT usability [0, 1, 2, 3]\n");
printf("     linksize       internal link size [2, 3, 4]\n");
printf("     newline        newline type [CR, LF, CRLF, ANYCRLF, ANY, NUL]\n");
printf("     pcre2-8        8 bit library support enabled [0, 1]\n");
printf("     pcre2-16       16 bit library support enabled [0, 1]\n");
printf("     pcre2-32       32 bit library support enabled [0, 1]\n");
printf("     unicode        Unicode and UTF support enabled [0, 1]\n");
printf("  --colo[u]r[=<always,auto,never>]\n");
printf("                show output in colour\n");
printf("  -d            set default pattern modifier 'debug'\n");
printf("  -dfa          set default subject modifier 'dfa'\n");
printf("  -E            preprocess input only (#if ... #endif)\n");
printf("  -error <n,m,..>  show messages for error numbers, then exit\n");
printf("  -help         show usage information\n");
printf("  -i            set default pattern modifier 'info'\n");
printf("  -jit          set default pattern modifier 'jit'\n");
printf("  -jitfast      set default pattern modifier 'jitfast'\n");
printf("  -jitverify    set default pattern modifier 'jitverify'\n");
printf("  -LM           list pattern and subject modifiers, then exit\n");
printf("  -LP           list non-script properties, then exit\n");
printf("  -LS           list supported scripts, then exit\n");
printf("  -malloc       exercise malloc() failures\n");
printf("  -q            quiet: do not output PCRE2 version number at start\n");
printf("  -pattern <s>  set default pattern modifier fields\n");
printf("  -subject <s>  set default subject modifier fields\n");
printf("  -S <n>        set stack size to <n> mebibytes\n");
printf("  -t [<n>]      time compilation and execution, repeating <n> times\n");
printf("  -tm [<n>]     time execution (matching) only, repeating <n> times\n");
printf("  -T            same as -t, but show total times at the end\n");
printf("  -TM           same as -tm, but show total time at the end\n");
printf("  -unittest     run unit tests, then exit\n");
printf("  -v|--version  show PCRE2 version and exit\n");
}



/*************************************************
*             Handle -C option                   *
*************************************************/

/* This option outputs configuration options and sets an appropriate return
code when asked for a single option. The code is abstracted into a separate
function because of its size.

Most, but not all, of the data is independent of the test mode.

Argument:   an option name or NULL
Returns:    the return code
*/

static int
c_option(const char *arg)
{
uint32_t optval;
unsigned int i = COPTLISTCOUNT;
int rc, yield = 0;

if (arg != NULL && arg[0] != '-')
  {
  for (i = 0; i < COPTLISTCOUNT; i++)
    if (strcmp(arg, coptlist[i].name) == 0) break;

  if (i >= COPTLISTCOUNT)
    {
    cfprintf(clr_test_error, stderr, "pcre2test: Unknown -C option \"%s\"\n", arg);
    return 0;
    }

  switch (coptlist[i].type)
    {
    case CONF_BSR:
    (void)pcre2_config(coptlist[i].value, &optval);
    printf("%s\n", (optval == PCRE2_BSR_ANYCRLF)? "ANYCRLF" : "ANY");
    break;

    case CONF_FIX:
    yield = coptlist[i].value;
    printf("%d\n", yield);
    break;

    case CONF_INT:
    (void)pcre2_config(coptlist[i].value, &yield);
    printf("%d\n", yield);
    break;

    case CONF_NL:
    (void)pcre2_config(coptlist[i].value, &optval);
    print_newline_config(optval, TRUE);
    break;

    case CONF_JU:
    rc = jit_compile_test();
    switch(rc)
      {
      case 0: yield = 0; break;
      case PCRE2_ERROR_NOMEMORY: yield = 1; break;
      case PCRE2_ERROR_JIT_UNSUPPORTED: yield = 2; break;
      default: yield = 3; break;
      }
    printf("%d\n", yield);
    break;
    }

/* For VMS, return the value by setting a symbol, for certain values only. This
is contributed code which the PCRE2 developers have no means of testing. */

#ifdef __VMS

/* This is the original code provided by the first VMS contributor. */
#ifdef NEVER
  if (copytlist[i].type == CONF_FIX || coptlist[i].type == CONF_INT)
    {
    char ucname[16];
    strcpy(ucname, coptlist[i].name);
    for (i = 0; ucname[i] != 0; i++) ucname[i] = toupper[ucname[i]];
    vms_setsymbol(ucname, 0, optval);
    }
#endif

/* This is the new code, provided by a second VMS contributor. */

  if (coptlist[i].type == CONF_FIX || coptlist[i].type == CONF_INT)
    {
    char nam_buf[22], val_buf[4];
    $DESCRIPTOR(nam, nam_buf);
    $DESCRIPTOR(val, val_buf);

    strcpy(nam_buf, coptlist[i].name);
    nam.dsc$w_length = strlen(nam_buf);
    sprintf(val_buf, "%d", yield);
    val.dsc$w_length = strlen(val_buf);
    lib$set_symbol(&nam, &val);
    }
#endif  /* __VMS */

  return yield;
  }

/* No argument for -C: output all configuration information. */

print_version(stdout, FALSE);
printf("Compiled with\n");

#ifdef EBCDIC
printf("  EBCDIC code support: LF is 0x%02x\n", CHAR_LF);
#if defined NATIVE_ZOS
printf("  EBCDIC code page %s or similar\n", pcrz_cpversion());
#endif
#if EBCDIC_IO
printf("  Input/output for pcre2test is EBCDIC\n");
#else
printf("  Input/output for pcre2test is ASCII, not EBCDIC\n");
#endif
#endif

(void)pcre2_config(PCRE2_CONFIG_COMPILED_WIDTHS, &optval);
if (optval & 1) printf("  8-bit support\n");
if (optval & 2) printf("  16-bit support\n");
if (optval & 4) printf("  32-bit support\n");

#ifdef SUPPORT_VALGRIND
printf("  Valgrind support\n");
#endif

(void)pcre2_config(PCRE2_CONFIG_UNICODE, &optval);
if (optval != 0)
  {
  printf("  UTF and UCP support (");
  print_unicode_version(stdout);
  printf(")\n");
  }
else printf("  No Unicode support\n");

(void)pcre2_config(PCRE2_CONFIG_JIT, &optval);
if (optval != 0)
  {
  printf("  Just-in-time compiler support\n");
  printf("    Architecture: ");
  print_jit_target(stdout);
  printf("\n");

  printf("    Can allocate executable memory: ");
  rc = jit_compile_test();
  switch(rc)
    {
    case 0:
    printf("Yes\n");
    break;

    case PCRE2_ERROR_NOMEMORY:
    printf("No (so cannot work)\n");
    break;

    default:
    cfprintf(clr_test_error, stdout, "\n** Unexpected return %d from "
      "pcre2_jit_compile(NULL, PCRE2_JIT_TEST_ALLOC)\n", rc);
    cfprintf(clr_test_error, stdout, "** Should not occur\n");
    yield = 1;
    break;
    }
  }
else
  {
  printf("  No just-in-time compiler support\n");
  }

(void)pcre2_config(PCRE2_CONFIG_NEWLINE, &optval);
print_newline_config(optval, FALSE);
(void)pcre2_config(PCRE2_CONFIG_BSR, &optval);
printf("  \\R matches %s\n",
  (optval == PCRE2_BSR_ANYCRLF)? "CR, LF, or CRLF only" :
                                 "all Unicode newlines");
(void)pcre2_config(PCRE2_CONFIG_NEVER_BACKSLASH_C, &optval);
printf("  \\C is %ssupported\n", optval? "not ":"");
printf("  Internal link size\n");
(void)pcre2_config(PCRE2_CONFIG_LINKSIZE, &optval);
printf("    Requested = %d\n", optval);
(void)pcre2_config(PCRE2_CONFIG_EFFECTIVE_LINKSIZE, &optval);
printf("    Effective = %d\n", optval);
(void)pcre2_config(PCRE2_CONFIG_PARENSLIMIT, &optval);
printf("  Parentheses nest limit = %d\n", optval);
(void)pcre2_config(PCRE2_CONFIG_HEAPLIMIT, &optval);
printf("  Default heap limit = %d kibibytes\n", optval);
(void)pcre2_config(PCRE2_CONFIG_MATCHLIMIT, &optval);
printf("  Default match limit = %d\n", optval);
(void)pcre2_config(PCRE2_CONFIG_DEPTHLIMIT, &optval);
printf("  Default depth limit = %d\n", optval);

#if defined SUPPORT_LIBREADLINE
printf("  pcre2test has libreadline support\n");
#elif defined SUPPORT_LIBEDIT
printf("  pcre2test has libedit support\n");
#else
printf("  pcre2test has neither libreadline nor libedit support\n");
#endif

return yield;
}


/*************************************************
*      Format one property/script list item      *
*************************************************/

#ifdef SUPPORT_UNICODE
static void
format_list_item(int16_t *ff, char *buff, BOOL isscript)
{
int count;
int maxi = 0;
const char *maxs = "";
size_t max = 0;

for (count = 0; ff[count] >= 0; count++) {}

/* Find the name to put first. For scripts, any 3-character name is chosen.
For non-scripts, or if there is no 3-character name, take the longest. */

for (int i = 0; ff[i] >= 0; i++)
  {
  const char *s = PRIV(utt_names) + ff[i];
  size_t len = strlen(s);
  if (isscript && len == 3)
    {
    maxi = i;
    max = len;
    maxs = s;
    break;
    }
  else if (len > max)
    {
    max = len;
    maxi = i;
    maxs = s;
    }
  }

strcpy(buff, maxs);
buff += max;

if (count > 1)
  {
  const char *sep = " (";
  for (int i = 0; i < count; i++)
    {
    if (i == maxi) continue;
    buff += sprintf(buff, "%s%s", sep, PRIV(utt_names) + ff[i]);
    sep = ", ";
    }
  (void)sprintf(buff, ")");
  }
}
#endif  /* SUPPORT_UNICODE */



/*************************************************
*        Display scripts or properties           *
*************************************************/

#define MAX_SYNONYMS 5

static void
display_properties(BOOL wantscripts)
{
#ifndef SUPPORT_UNICODE
(void)wantscripts;
printf("** This version of PCRE2 was compiled without Unicode support.\n");
#else

uint16_t seentypes[1024];
uint16_t seenvalues[1024];
int seencount = 0;
int16_t found[256][MAX_SYNONYMS + 1];
int fc = 0;
int colwidth = 40;
int n = wantscripts? ucp_Script_Count : ucp_Bprop_Count;

for (size_t i = 0; i < PRIV(utt_size); i++)
  {
  int k;
  int m = 0;
  int16_t *fv;
  const ucp_type_table *t = PRIV(utt) + i;
  unsigned int value = t->value;

  if (wantscripts)
    {
    if (t->type != PT_SC && t->type != PT_SCX) continue;
    }
  else
    {
    if (t->type != PT_BOOL) continue;
    }

  for (k = 0; k < seencount; k++)
    {
    if (t->type == seentypes[k] && t->value == seenvalues[k]) break;
    }
  if (k < seencount) continue;

  seentypes[seencount] = t->type;
  seenvalues[seencount++] = t->value;

  fv = found[fc++];
  fv[m++] = t->name_offset;

  for (size_t j = i + 1; j < PRIV(utt_size); j++)
    {
    const ucp_type_table *tt = PRIV(utt) + j;
    if (tt->type != t->type || tt->value != value) continue;
    if (m >= MAX_SYNONYMS)
      cfprintf(clr_test_error, stdout, "** Too many synonyms: %s ignored\n",
        PRIV(utt_names) + tt->name_offset);
    else fv[m++] = tt->name_offset;
    }

  fv[m] = -1;
  }

printf("-------------------------- SUPPORTED %s --------------------------\n\n",
  wantscripts? "SCRIPTS" : "PROPERTIES");

if (!wantscripts) printf(
"This release of PCRE2 supports Unicode's general category properties such\n"
"as Lu (upper case letter), bi-directional properties such as Bidi_Class,\n"
"and the following binary (yes/no) properties:\n\n");


for (int k = 0; k < (n+1)/2; k++)
  {
  int x;
  char buff1[128];
  char buff2[128];

  format_list_item(found[k], buff1, wantscripts);
  x = k + (n+1)/2;
  if (x < n) format_list_item(found[x], buff2, wantscripts);
    else buff2[0] = 0;

  x = printf("%s", buff1);
  while (x++ < colwidth) printf(" ");
  printf("%s\n", buff2);
  }

#endif  /* SUPPORT_UNICODE */
}



/*************************************************
*              Display one modifier              *
*************************************************/

static void
display_one_modifier(modstruct *m, BOOL for_pattern)
{
uint32_t c = (!for_pattern && (m->which == MOD_PND || m->which == MOD_PNDP))?
  '*' : ' ';
printf("%c%s", c, m->name);
for (size_t i = 0; i < C1MODLISTCOUNT; i++)
  {
  if (strcmp(m->name, c1modlist[i].fullname) == 0)
    printf(" (%c)", c1modlist[i].onechar);
  }
}



/*************************************************
*       Display pattern or subject modifiers     *
*************************************************/

/* In order to print in two columns, first scan without printing to get a list
of the modifiers that are required.

Arguments:
  for_pattern   TRUE for pattern modifiers, FALSE for subject modifiers
  title         string to be used in title

Returns:        nothing
*/

static void
display_selected_modifiers(BOOL for_pattern, const char *title)
{
uint32_t i, j;
uint32_t n = 0;
uint32_t list[MODLISTCOUNT];
uint32_t extra[MODLISTCOUNT];

for (i = 0; i < MODLISTCOUNT; i++)
  {
  BOOL is_pattern = TRUE;
  modstruct *m = modlist + i;

  switch (m->which)
    {
    case MOD_CTC:       /* Compile context */
    case MOD_PAT:       /* Pattern */
    case MOD_PATP:      /* Pattern, OK for Perl-compatible test */
    break;

    /* The MOD_PND and MOD_PNDP modifiers are precisely those that affect
    subjects, but can be given with a pattern. We list them as subject
    modifiers, but marked with an asterisk.*/

    case MOD_CTM:       /* Match context */
    case MOD_DAT:       /* Subject line */
    case MOD_DATP:      /* Subject line, OK for Perl-compatible test */
    case MOD_PND:       /* As PD, but not default pattern */
    case MOD_PNDP:      /* As PND, OK for Perl-compatible test */
    is_pattern = FALSE;
    break;

    case MOD_PD:        /* Pattern or subject */
    case MOD_PDP:       /* As PD, OK for Perl-compatible test */
    is_pattern = for_pattern;
    break;

    default:
    printf("** Unknown type for modifier \"%s\"\n", m->name);
    PCRE2_DEBUG_UNREACHABLE();
    exit(1);
    }

  if (for_pattern == is_pattern)
    {
    extra[n] = 0;
    for (size_t k = 0; k < C1MODLISTCOUNT; k++)
      {
      if (strcmp(m->name, c1modlist[k].fullname) == 0)
        {
        extra[n] += 4;
        break;
        }
      }
    list[n++] = i;
    }
  }

/* Now print from the list in two columns. */

printf("-------------- %s MODIFIERS --------------\n", title);

for (i = 0, j = (n+1)/2; i < (n+1)/2; i++, j++)
  {
  modstruct *m = modlist + list[i];
  display_one_modifier(m, for_pattern);
  if (j < n)
    {
    size_t k = 27 - strlen(m->name) - extra[i];
    while (k-- > 0) printf(" ");
    display_one_modifier(modlist + list[j], for_pattern);
    }
  printf("\n");
  }
}



/*************************************************
*          Display the list of modifiers         *
*************************************************/

static void
display_modifiers(void)
{
printf(
  "An asterisk on a subject modifier means that it may be given on a pattern\n"
  "line, in order to apply to all subjects matched by that pattern. Modifiers\n"
  "that are listed for both patterns and subjects have different effects in\n"
  "each case.\n\n");
display_selected_modifiers(TRUE, "PATTERN");
printf("\n");
display_selected_modifiers(FALSE, "SUBJECT");
}



/*************************************************
*                Main Program                    *
*************************************************/

int
main(int argc, char **argv)
{
uint32_t yield = 0;
uint32_t op = 1;
BOOL notdone = TRUE;
BOOL quiet = FALSE;
BOOL showtotaltimes = FALSE;
BOOL skipping = FALSE;
BOOL skipping_endif = FALSE;
char *arg_subject = NULL;
char *arg_pattern = NULL;
char *arg_error = NULL;

/* Get buffers from malloc() so that valgrind will check their misuse when
debugging. They grow automatically when very long lines are read. The 16-
and 32-bit buffers (pbuffer16, pbuffer32) are obtained only if needed. */

buffer = (uint8_t *)malloc(pbuffer8_size);
pbuffer8 = (uint8_t *)malloc(pbuffer8_size);

/* The following  _setmode() stuff is some Windows magic that tells its runtime
library to translate CRLF into a single LF character. At least, that's what
I've been told: never having used Windows I take this all on trust. Originally
it set 0x8000, but then I was advised that _O_BINARY was better. */

#if defined(_WIN32) || defined(WIN32)
_setmode( _fileno( stdout ), _O_BINARY );
#endif

/* Initialization that does not depend on the running mode. */

locale_name[0] = 0;

patctl_zero(&def_patctl);
datctl_zero(&def_datctl);

/* Scan command line options. */

while (argc > 1 && argv[op][0] == '-' && argv[op][1] != 0)
  {
  char *endptr;
  char *arg = argv[op];
  unsigned long uli;

  /* List modifiers and exit. */

  if (strcmp(arg, "-LM") == 0)
    {
    display_modifiers();
    goto EXIT;
    }

  /* List properties and exit */

  if (strcmp(arg, "-LP") == 0)
    {
    display_properties(FALSE);
    goto EXIT;
    }

  /* List scripts and exit */

  if (strcmp(arg, "-LS") == 0)
    {
    display_properties(TRUE);
    goto EXIT;
    }

  /* Perform additional edge-case and error-handling tests of public API
  functions, which wouldn't otherwise be covered by the standard use of the API
  in pcre2test. */

  if (strcmp(arg, "-unittest") == 0)
    {
    unittest();
    goto EXIT;
    }

  /* Display and/or set return code for configuration options. */

  if (strcmp(arg, "-C") == 0)
    {
    yield = c_option(argv[op + 1]);
    goto EXIT;
    }

  /* Select operating mode. */

  if (strcmp(arg, "-8") == 0)
    {
#ifdef SUPPORT_PCRE2_8
    test_mode = PCRE2TEST_MODE_8;
#else
    cfprintf(clr_test_error, stderr,
      "pcre2test: This version of PCRE2 was built without 8-bit support\n");
    exit(1);
#endif
    }

  else if (strcmp(arg, "-16") == 0)
    {
#ifdef SUPPORT_PCRE2_16
    test_mode = PCRE2TEST_MODE_16;
#else
    cfprintf(clr_test_error, stderr,
      "pcre2test: This version of PCRE2 was built without 16-bit support\n");
    exit(1);
#endif
    }

  else if (strcmp(arg, "-32") == 0)
    {
#ifdef SUPPORT_PCRE2_32
    test_mode = PCRE2TEST_MODE_32;
#else
    cfprintf(clr_test_error, stderr,
      "pcre2test: This version of PCRE2 was built without 32-bit support\n");
    exit(1);
#endif
    }

  /* Set preprocess-only (only handle #if ... #endif) */

  else if (strcmp(arg, "-E") == 0) preprocess_only = TRUE;

  /* Set quiet (no version verification) */

  else if (strcmp(arg, "-q") == 0) quiet = TRUE;

  /* Set system stack size */

  else if (strcmp(arg, "-S") == 0 && argc > 2 &&
      ((uli = strtoul(argv[op+1], &endptr, 10)), *endptr == 0))
    {
#if defined(_WIN32) || defined(WIN32) || defined(__HAIKU__) || defined(NATIVE_ZOS) || defined(__VMS)
    cfprintf(clr_test_error, stderr, "pcre2test: -S is not supported on this OS\n");
    exit(1);
#else
    int rc = 0;
    uint32_t stack_size;
    struct rlimit rlim, rlim_old;
    if (uli > INT32_MAX / (1024 * 1024))
      {
      cfprintf(clr_test_error, stderr, "pcre2test: Argument for -S is too big\n");
      exit(1);
      }
    stack_size = (uint32_t)uli;
    getrlimit(RLIMIT_STACK, &rlim_old);
    rlim = rlim_old;
    rlim.rlim_cur = stack_size * 1024 * 1024;
    if (rlim.rlim_max != RLIM_INFINITY && rlim.rlim_cur > rlim.rlim_max)
      {
      cfprintf(clr_test_error, stderr,
        "pcre2test: requested stack size %luMiB is greater than hard limit ",
          (unsigned long int)stack_size);
      if (rlim.rlim_max % (1024*1024) == 0)
        cfprintf(clr_test_error, stderr, "%luMiB\n", (unsigned long)(rlim.rlim_max/(1024*1024)));
      else if (rlim.rlim_max % 1024 == 0)
        cfprintf(clr_test_error, stderr, "%luKiB\n", (unsigned long)(rlim.rlim_max/1024));
      else
        cfprintf(clr_test_error, stderr, "%lu bytes\n", (unsigned long)(rlim.rlim_max));
      exit(1);
      }
    if (rlim_old.rlim_cur != RLIM_INFINITY && rlim_old.rlim_cur <= INT32_MAX &&
        rlim.rlim_cur > rlim_old.rlim_cur)
      rc = setrlimit(RLIMIT_STACK, &rlim);
    if (rc != 0)
      {
      cfprintf(clr_test_error, stderr, "pcre2test: setting stack size %luMiB failed: %s\n",
        (unsigned long int)stack_size, strerror(errno));
      exit(1);
      }
    op++;
    argc--;
#endif
    }

  /* Set some common pattern and subject controls */

  else if (strcmp(arg, "-AC") == 0)
    {
    def_patctl.options |= PCRE2_AUTO_CALLOUT;
    def_datctl.control2 |= CTL2_CALLOUT_EXTRA;
    }
  else if (strcmp(arg, "-ac") == 0)  def_patctl.options |= PCRE2_AUTO_CALLOUT;
  else if (strcmp(arg, "-b") == 0)   def_patctl.control |= CTL_FULLBINCODE;
  else if (strcmp(arg, "-d") == 0)   def_patctl.control |= CTL_DEBUG;
  else if (strcmp(arg, "-dfa") == 0) def_datctl.control |= CTL_DFA;
  else if (strcmp(arg, "-i") == 0)   def_patctl.control |= CTL_INFO;
  else if (strcmp(arg, "-jit") == 0 || strcmp(arg, "-jitverify") == 0 ||
           strcmp(arg, "-jitfast") == 0)
    {
    if (arg[4] == 'v') def_patctl.control |= CTL_JITVERIFY;
      else if (arg[4] == 'f') def_patctl.control |= CTL_JITFAST;
    def_patctl.jit = JIT_DEFAULT;  /* full & partial */
#ifndef SUPPORT_JIT
    cfprintf(clr_test_error, stderr, "pcre2test: Warning: JIT support is not available: "
                    "-jit[fast|verify] calls functions that do nothing.\n");
#endif
    }

  /* Set timing parameters */

  else if (strcmp(arg, "-t") == 0 || strcmp(arg, "-tm") == 0 ||
           strcmp(arg, "-T") == 0 || strcmp(arg, "-TM") == 0)
    {
    int both = arg[2] == 0;
    showtotaltimes = arg[1] == 'T';
    if (argc > 2 && (uli = strtoul(argv[op+1], &endptr, 10), *endptr == 0))
      {
      if (uli == 0)
        {
        cfprintf(clr_test_error, stderr, "pcre2test: Argument for %s must not be zero\n", arg);
        exit(1);
        }
      if (U32OVERFLOW(uli))
        {
        cfprintf(clr_test_error, stderr, "pcre2test: Argument for %s is too big\n", arg);
        exit(1);
        }
      timeitm = (int)uli;
      op++;
      argc--;
      }
    else timeitm = LOOPREPEAT;
    if (both) timeit = timeitm;
    }

  /* Set malloc testing */

  else if (strcmp(arg, "-malloc") == 0)
    {
    malloc_testing = TRUE;
    }

  /* Give help */

  else if (strcmp(arg, "-help") == 0 ||
           strcmp(arg, "--help") == 0)
    {
    usage();
    goto EXIT;
    }

  /* Show version */

  else if (memcmp(arg, "-v", 2) == 0 ||
           strcmp(arg, "--version") == 0)
    {
    print_version(stdout, FALSE);
    goto EXIT;
    }

  /* The following options save their data for processing once we know what
  the running mode is. */

  else if (strcmp(arg, "-error") == 0)
    {
    arg_error = argv[op+1];
    goto CHECK_VALUE_EXISTS;
    }

  else if (strcmp(arg, "-subject") == 0)
    {
    arg_subject = argv[op+1];
    goto CHECK_VALUE_EXISTS;
    }

  else if (strcmp(arg, "-pattern") == 0)
    {
    arg_pattern = argv[op+1];
    CHECK_VALUE_EXISTS:
    if (argc <= 2)
      {
      cfprintf(clr_test_error, stderr, "pcre2test: Missing value for %s\n", arg);
      yield = 1;
      goto EXIT;
      }
    op++;
    argc--;
    }

  else if (strcmp(arg, "--color") == 0 || strcmp(arg, "--colour") == 0)
    {
    colour_setting = COLOUR_ALWAYS;
    }

  else if (strstr(arg, "--color=") == arg || strstr(arg, "--colour=") == arg)
    {
    char *val = strchr(arg, '=') + 1;
    if (strcmp(val, "always") == 0) colour_setting = COLOUR_ALWAYS;
    else if (strcmp(val, "never") == 0) colour_setting = COLOUR_NEVER;
    else if (strcmp(val, "auto") == 0) colour_setting = COLOUR_AUTO;
    else
      {
      cfprintf(clr_test_error, stderr,
        "pcre2test: Invalid value for \"%.*s\"\n", (int)(val - 1 - arg), arg);
      yield = 1;
      goto EXIT;
      }
    }

  /* Unrecognized option */

  else
    {
    cfprintf(clr_test_error, stderr, "pcre2test: Unknown or malformed option \"%s\"\n", arg);
    usage();
    yield = 1;
    goto EXIT;
    }
  op++;
  argc--;
  }

/* If -error was present, get the error numbers, show the messages, and exit.
We wait to do this until we know which mode we are in. */

if (arg_error != NULL)
  {
  int errcode;
  char *endptr;
  long li;

  /* Loop along a list of error numbers. */

  for (;;)
    {
    li = strtol(arg_error, &endptr, 10);
    if (S32OVERFLOW(li) || (*endptr != 0 && *endptr != ','))
      {
      cfprintf(clr_test_error, stderr, "pcre2test: \"%s\" is not a valid error number list\n", arg_error);
      yield = 1;
      goto EXIT;
      }
    errcode = (int)li;
    printf("Error %d: ", errcode);
    print_error_message_file(stdout, errcode, "", "\n", TRUE);
    if (*endptr == 0) goto EXIT;
    arg_error = endptr + 1;
    }

  PCRE2_UNREACHABLE(); /* Control never reaches here */
  }  /* End of -error handling */

/* Initialize things that cannot be done until we know which test mode we are
running in. */

max_oveccount = DEFAULT_OVECCOUNT;

/* Initialise the globals for the current mode. */

init_globals();

/* Handle command line modifier settings, sending any error messages to
stderr. We need to know the mode before modifying the context, and it is tidier
to do them all in the same way. */

outfile = stderr;
if ((arg_pattern != NULL &&
    !decode_modifiers((uint8_t *)arg_pattern, CTX_DEFPAT, &def_patctl, NULL)) ||
    (arg_subject != NULL &&
    !decode_modifiers((uint8_t *)arg_subject, CTX_DEFDAT, NULL, &def_datctl)))
  {
  yield = 1;
  goto EXIT;
  }

/* Sort out the input and output files, defaulting to stdin/stdout. */

infile = stdin;
outfile = stdout;

if (argc > 1 && strcmp(argv[op], "-") != 0)
  {
  infile = fopen(argv[op], INPUT_MODE);
  if (infile == NULL)
    {
    cfprintf(clr_test_error, stderr, "pcre2test: Failed to open \"%s\": %s\n", argv[op], strerror(errno));
    yield = 1;
    goto EXIT;
    }
  }

#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
if (INTERACTIVE(infile)) using_history();
#endif

if (argc > 2)
  {
  outfile = fopen(argv[op+1], OUTPUT_MODE);
  if (outfile == NULL)
    {
    cfprintf(clr_test_error, stderr, "pcre2test: Failed to open \"%s\": %s\n", argv[op+1], strerror(errno));
    yield = 1;
    goto EXIT;
    }
  }

/* Output a heading line unless quiet, then process input lines. */

if (!quiet) print_version(outfile, TRUE);

#ifdef SUPPORT_PCRE2_8
preg.re_pcre2_code = NULL;
preg.re_match_data = NULL;
#endif

while (notdone)
  {
  const uint8_t *p;
  const uint8_t *p_notsp;
  int rc = PR_OK;
  BOOL expectdata = have_active_pattern();
  BOOL is_pattern_comment;
  BOOL is_data_comment;
#ifdef SUPPORT_PCRE2_8
  expectdata |= preg.re_pcre2_code != NULL;
#endif

  if (extend_inputline(infile, buffer, expectdata? "data> " : "  re> ") == NULL)
    break;

  /* Pre-process input lines with #if...#endif. */

  if (skipping_endif)
    {
    if (strncmp((char*)buffer, "#endif", 6) != 0 ||
        !(buffer[6] == 0 || isspace(buffer[6])))
      continue;
    skipping_endif = FALSE;
    }

  /* Begin processing the line. */

  p = p_notsp = buffer;
  while (isspace(*p_notsp)) p_notsp++;

  is_pattern_comment = p[0] == '#' &&
    (isspace(p[1]) || p[1] == '!' || p[1] == 0);
  is_data_comment = expectdata && p_notsp[0] == '\\' && p_notsp[1] == '=' &&
    (isspace(p_notsp[2]) || p_notsp[2] == 0);

  if (!INTERACTIVE(infile))
    cfprintf((is_pattern_comment || is_data_comment)? clr_comment : clr_input,
      outfile, "%s", (char *)buffer);
  fflush(outfile);

  if (preprocess_only && *p != '#') continue;

  /* If we have a pattern set up for testing, or we are skipping after a
  compile failure, a blank line terminates this test. */

  if (expectdata || skipping)
    {
    if (*p_notsp == 0)
      {
#ifdef SUPPORT_PCRE2_8
      if (preg.re_pcre2_code != NULL)
        {
        regfree(&preg);
        preg.re_pcre2_code = NULL;
        preg.re_match_data = NULL;
        }
#endif  /* SUPPORT_PCRE2_8 */
      free_active_pattern();
      skipping = FALSE;
      setlocale(LC_CTYPE, "C");
      }

    /* Otherwise, if we are not skipping, and the line is not a data comment
    line starting with "\=", process a data line. */

    else if (!skipping && !is_data_comment)
      {
      rc = process_data();
      }
    }

  /* We do not have a pattern set up for testing. Lines starting with # are
  either comments or special commands. Blank lines are ignored. Otherwise, the
  line must start with a valid delimiter. It is then processed as a pattern
  line. The pattern remains in pbuffer8/16/32 after compilation, for use by
  callouts. Under valgrind, make the unused part of the buffer undefined, to
  catch overruns. */

  else if (*p == '#')
    {
    if (is_pattern_comment) continue;
    rc = process_command();
    }

  else if (strchr("/!\"'`%&-=_:;,@~", *p) != NULL)
    {
    rc = process_pattern();
    dfa_matched = 0;
    }

  else
    {
    if (*p_notsp != 0)
      {
      cfprintf(clr_test_error, outfile, "** Invalid pattern delimiter '%c' (x%x).\n", *buffer,
        *buffer);
      rc = PR_SKIP;
      }
    }

  if (rc == PR_SKIP && !INTERACTIVE(infile)) skipping = TRUE;
  else if (rc == PR_ENDIF) skipping_endif = TRUE;
  else if (rc == PR_ABEND)
    {
    cfprintf(clr_test_error, outfile, "** pcre2test run abandoned\n");
    yield = 1;
    goto EXIT;
    }
  }

/* Finish off a normal run. */

if (skipping_endif)
  {
  cfprintf(clr_test_error, outfile, "** Expected #endif\n");
  yield = 1;
  goto EXIT;
  }

if (INTERACTIVE(infile)) fprintf(outfile, "\n");

if (showtotaltimes)
  {
  const char *pad = "";
  cfprintf(clr_profiling, outfile, "--------------------------------------\n");
  if (timeit > 0)
    {
    cfprintf(clr_profiling, outfile, "Total compile time %8.2f microseconds\n",
      ((1000000 / CLOCKS_PER_SEC) * (double)total_compile_time) / timeit);
    if (total_jit_compile_time > 0)
      cfprintf(clr_profiling, outfile, "Total JIT compile  %8.2f microseconds\n",
        ((1000000 / CLOCKS_PER_SEC) * (double)total_jit_compile_time) / \
        timeit);
    pad = "  ";
    }
  cfprintf(clr_profiling, outfile, "Total match time %s%8.2f microseconds\n", pad,
    ((1000000 / CLOCKS_PER_SEC) * (double)total_match_time) / timeitm);
  }


EXIT:

#if defined(SUPPORT_LIBREADLINE) || defined(SUPPORT_LIBEDIT)
if (infile != NULL && INTERACTIVE(infile)) clear_history();
#endif

if (infile != NULL && infile != stdin) fclose(infile);
if (outfile != NULL && outfile != stdout) fclose(outfile);

#ifdef SUPPORT_PCRE2_8
if (preg.re_pcre2_code != NULL) regfree(&preg);
#endif

free(buffer);
free(dbuffer);
free(pbuffer8);
#ifdef SUPPORT_PCRE2_16
free(pbuffer16);
#endif
#ifdef SUPPORT_PCRE2_32
free(pbuffer32);
#endif
free(dfa_workspace);
free(tables3);
free_globals();

#if defined(__VMS)
  yield = SS$_NORMAL;  /* Return values via DCL symbols */
#endif

return yield;
}

/* End of pcre2test.c */
