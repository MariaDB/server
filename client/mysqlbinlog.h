
#ifndef _mysqlbinlog_h
#define _mysqlbinlog_h

/**
  Exit status for functions in this file.
*/
enum Exit_status {
  /** No error occurred and execution should continue. */
  OK_CONTINUE= 0,
  /** An error occurred and execution should stop. */
  ERROR_STOP,
  /** No error occurred but execution should stop. */
  OK_STOP,
  /** No error occurred - end of file reached. */
  OK_EOF,
};

#endif
