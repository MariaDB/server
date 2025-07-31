

/* -*- mode: c -*-
 * $Id: strcat.gcc,v 1.7 2001/05/02 05:55:44 doug Exp $
 * http://www.bagley.org/~doug/shootout/
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STUFF "hello\n"

int
main(int argc, char *argv[]) {
    int n = ((argc == 2) ? atoi(argv[1]) : 1);
    int i, buflen = 32;

    for (int j = 0; j < 50; j++) {
      char *strbuf = calloc(sizeof(char), buflen);
      char *strend = strbuf;
      int stufflen = strlen(STUFF);
      
      if (!strbuf) { perror("calloc strbuf"); exit(1); }
      for (i=0; i<n; i++) {
	if (((strbuf+buflen)-strend) < (stufflen+1)) {
	  buflen = 2*buflen;
	  strbuf = realloc(strbuf, buflen);
	  if (!strbuf) { perror("realloc strbuf"); exit(1); }
	  strend = strbuf + strlen(strbuf);
	}
	
	strcat(strend, STUFF);
	strend += stufflen;
      }
      if (j == 0)
	fprintf(stdout, "%d\n", strlen(strbuf));
      free(strbuf);
    }
    return(0);
}
