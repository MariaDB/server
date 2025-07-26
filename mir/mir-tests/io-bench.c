#include "../mir.h"
#include <time.h>

#include "api-sieve.h"
#include "scan-sieve.h"

int main (void) {
  FILE *f;
  const char *fname = "/tmp/__tmp.mirb";
  double start_time;
  const int nfunc = 100000;
  size_t len, text_len;
  double api_time_creation, scan_api_time_creation, read_api_time_creation;
  MIR_context_t ctx = MIR_init ();

  start_time = clock ();
  for (int i = 0; i < nfunc; i++) create_mir_func_sieve_api (ctx, NULL);
  api_time_creation = (clock () - start_time) / CLOCKS_PER_SEC;
  fprintf (stderr, "Creating %d sieve functions by API: %.3f CPU sec\n", nfunc, api_time_creation);
  MIR_finish (ctx);
  ctx = MIR_init ();
  start_time = clock ();
  for (int i = 0; i < nfunc; i++) create_mir_func_sieve (ctx, &text_len, NULL);
  scan_api_time_creation = (clock () - start_time) / CLOCKS_PER_SEC;
  fprintf (stderr, "Creating %d sieve functions from MIR text (%.3f MB): %.3f CPU sec - API use\n",
           nfunc, text_len / 1000000.0 * nfunc, scan_api_time_creation);
  f = fopen (fname, "wb");
  mir_assert (f != NULL);
  MIR_write (ctx, f);
  fclose (f);
  MIR_finish (ctx);
  f = fopen (fname, "rb");
  mir_assert (f != NULL);
  start_time = clock ();
  for (len = 0; fgetc (f) != EOF; len++)
    ;
  fprintf (stderr,
           "Just reading MIR binary file containing %d sieve functions (%.3f MB): %.3f CPU sec\n",
           nfunc, len / 1000000.0, (clock () - start_time) / CLOCKS_PER_SEC);
  fclose (f);
  ctx = MIR_init ();
  f = fopen (fname, "rb");
  mir_assert (f != NULL);
  start_time = clock ();
  MIR_read (ctx, f);
  read_api_time_creation = (clock () - start_time) / CLOCKS_PER_SEC;
  fprintf (stderr,
           "Reading and creating MIR binary %d sieve functions (%.3f MB): %.3f CPU sec - API use\n",
           nfunc, len / 1000000.0, read_api_time_creation);
  fclose (f);
  remove (fname);
  MIR_finish (ctx);
  fprintf (stderr, "=========>Binary MIR / Text MIR: read time = %.3f, size = %.3f\n",
           (read_api_time_creation - api_time_creation)
             / (scan_api_time_creation - api_time_creation),
           1.0 * len / (text_len * nfunc));
  return 0;
}
