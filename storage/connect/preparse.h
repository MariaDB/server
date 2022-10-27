#if !defined(PREPARSE_DEFINED)
#define PREPARSE_DEFINED

#include "checklvl.h"

/***********************************************************************/
/*  Struct of variables used by the date format pre-parser.            */
/***********************************************************************/
typedef struct _datpar {
  const char *Format;     // Points to format to decode
  const char *Curp;       // Points to current parsing position
  char *InFmt;            // Start of input format
  char *OutFmt;           // Start of output format
  int   Index[8];         // Indexes of date values
  int   Num;              // Number of values to retrieve
  int   Flag;             // 1: Input, 2: Output, 4: no output blank
  int   Outsize;          // Size of output buffers
  } DATPAR, *PDTP;

/***********************************************************************/
/*  Preparsers used by SQL language.                                   */
/***********************************************************************/
#ifdef __cplusplus
extern "C" {
#endif

int fmdflex(PDTP pp);

#ifdef __cplusplus
}
#endif

#endif // PREPARSE_DEFINED

