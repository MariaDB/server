/* Transform mir binary form from stdin into mir text to stdout.  */

#include "mir.h"

#ifdef _WIN32
/* <stdio.h> provides _fileno */
#include <fcntl.h> /* provides _O_BINARY */
#include <io.h>    /* provides _setmode */
#define set_filemode_binary(F) _setmode (_fileno (F), _O_BINARY)
#else
#define set_filemode_binary(F) 0
#endif

int main (int argc, char *argv[]) {
  MIR_context_t ctx = MIR_init ();

  if (set_filemode_binary (stdin) == -1) return 1;
  if (argc != 1) {
    fprintf (stderr, "Usage: %s < mir-binary-file  > mir-text-file\n", argv[1]);
    return 1;
  }
  MIR_read (ctx, stdin);
  MIR_output (ctx, stdout);
  MIR_finish (ctx);
  return 0;
}
