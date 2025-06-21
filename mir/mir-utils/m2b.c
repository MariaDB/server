/* Transform mir textual form from stdin into mir binary to
   stdout.  */

#include "mir-alloc.h"
#include "mir.h"

#ifdef _WIN32
/* <stdio.h> provides _fileno */
#include <fcntl.h> /* provides _O_BINARY */
#include <io.h>    /* provides _setmode */
#define set_filemode_binary(F) _setmode (_fileno (F), _O_BINARY)
#else
#define set_filemode_binary(F) 0
#endif

DEF_VARR (char);

int main (int argc, char *argv[]) {
  MIR_context_t ctx = MIR_init ();
  MIR_alloc_t alloc = MIR_get_alloc (ctx);
  VARR (char) * str;
  int c;

  if (set_filemode_binary (stdout) == -1) return 1;
  if (argc != 1) {
    fprintf (stderr, "Usage: %s < mir-text-file > mir-binary-file\n", argv[1]);
    return 1;
  }
  VARR_CREATE (char, str, alloc, 1024 * 1024);
  while ((c = getchar ()) != EOF) VARR_PUSH (char, str, c);
  VARR_PUSH (char, str, 0);
  MIR_scan_string (ctx, VARR_ADDR (char, str));
  MIR_write (ctx, stdout);
  MIR_finish (ctx);
  VARR_DESTROY (char, str);
  return 0;
}
