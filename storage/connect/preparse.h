#if !defined(PREPARSE_DEFINED)
#define PREPARSE_DEFINED

#include "checklvl.h"

/***********************************************************************/
/*  Struct of variables used by the SQL pre-parsers.                   */
/***********************************************************************/
typedef struct _prepar {
  struct _prepar *Next;
  char *Debinp;           // Start of input buffer
  char *Endinp;           // End of input buffer
  char *Pluginp;          // Points on current parsing position
  char *Plugbuf;          // Start of output buffer
  char *Plugptr;          // Current output position
  char *Debchar;          // Next/current start of command
  char *Debselp;          // Beginning of selection
  char *Debline;          // Start of current line
  char *Plugpar[32];      // Parameters
  int   Numparms;         // Number of defined parameters
  int   Nprms;            // Number of ODBC parameters
  int   Lines;            // Line number
  int   Chars;            // Index of selection start in line
  int   Endchars;         // Index of selection end in line
  int   Frinp, Frbuf;     // 0: no, 1: free, 2: delete Debinp/Plugbuf
  int   Outsize;          // Size of output buffer
  FILE *Argfile;          // File containing arguments
  int   Addargs;          // 1 if arguments are added to the list
  } PREPAR, *PPREP;

/***********************************************************************/
/*  Struct of variables used by the date format pre-parser.            */
/***********************************************************************/
typedef struct _datpar {
  char *Format;           // Points to format to decode
  char *Curp;             // Points to current parsing position
  char *InFmt;            // Start of input format
  char *OutFmt;           // Start of output format
  int   Index[8];         // Indexes of date values
  int   Num;              // Number of values to retrieve
  int   Flag;             // 1: Input, 2: Output, 4: no output blank
  int  Outsize;          // Size of output buffers
  } DATPAR, *PDTP;

/***********************************************************************/
/*  Preparsers used by SQL language.                                   */
/***********************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

int sqlflex(PPREP pp);
int sqpflex(PPREP pp);
int fmdflex(PDTP pp);

#ifdef __cplusplus
}
#endif

#endif // PREPARSE_DEFINED

