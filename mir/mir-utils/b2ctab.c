/* Transform mir binary form from stdin into C code array containing mir binary.  */

#include "mir.h"

#ifdef _WIN32
/* <stdio.h> provides _fileno */
#include <fcntl.h> /* provides _O_BINARY */
#include <io.h>    /* provides _setmode */
#define set_filemode_binary(F) _setmode (_fileno (F), _O_BINARY)
#else
#define set_filemode_binary(F) 0
#endif

static size_t output_mir_code_byte_num;
static FILE *output_mir_code_file;

static int output_mir_code_byte (MIR_context_t ctx MIR_UNUSED, uint8_t byte) {
  fprintf (output_mir_code_file, "0x%02x, ", byte);
  if (++output_mir_code_byte_num % 16 == 0) fprintf (output_mir_code_file, "\n");
  return 1;
}

int main (int argc, char *argv[]) {
  MIR_context_t ctx = MIR_init ();

  if (set_filemode_binary (stdin) == -1) return 1;
  if (argc != 1) {
    fprintf (stderr, "Usage: %s < mir-binary-file  > C-file\n", argv[1]);
    return 1;
  }
  MIR_read (ctx, stdin);
  output_mir_code_file = stdout;
  fprintf (output_mir_code_file, "static const uint8_t mir_code[] = {\n");
  output_mir_code_byte_num = 0;
  MIR_write_with_func (ctx, output_mir_code_byte);
  fprintf (output_mir_code_file, "};\n");
  MIR_finish (ctx);
  return ferror (output_mir_code_file);
}
