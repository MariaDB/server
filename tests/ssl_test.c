/* Copyright (c) 2000, 2003, 2004, 2007 MySQL AB
   Use is subject to license terms

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */


#ifdef __WIN__
#include <windows.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include "mysql.h"
#include "config.h"
#define SELECT_QUERY "select name from test where num = %d"


int main(int argc, char **argv)
{
#ifdef HAVE_OPENSSL
  int	count, num;
  MYSQL mysql,*sock;
  MYSQL_RES *res;
  char	qbuf[160];

  if (argc != 3)
  {
    fprintf(stderr,"usage : ssl_test <dbname> <num>\n\n");
    exit(1);
  }

  mysql_init(&mysql);
#ifdef HAVE_OPENSSL
  mysql_ssl_set(&mysql,"../SSL/MySQL-client-key.pem",
    "../SSL/MySQL-client-cert.pem",
    "../SSL/MySQL-ca-cert.pem", 0, 0);
#endif
  if (!(sock = mysql_real_connect(&mysql,"127.0.0.1",0,0,argv[1],MYSQL_PORT,NULL,0)))
  {
    fprintf(stderr,"Couldn't connect to engine!\n%s\n\n",mysql_error(&mysql));
    perror("");
    exit(1);
  }
  mysql.reconnect= 1;
  count = 0;
  num = atoi(argv[2]);
  while (count < num)
  {
    sprintf(qbuf,SELECT_QUERY,count);
    if(mysql_query(sock,qbuf))
    {
      fprintf(stderr,"Query failed (%s)\n",mysql_error(sock));
      exit(1);
    }
    if (!(res=mysql_store_result(sock)))
    {
      fprintf(stderr,"Couldn't get result from query failed (%s)\n",
	      mysql_error(sock));
      exit(1);
    }
#ifdef TEST
    printf("number of fields: %d\n",mysql_num_fields(res));
#endif
    mysql_free_result(res);
    count++;
  }
  mysql_close(sock);
#else /* HAVE_OPENSSL */
  printf("ssl_test: SSL not configured.\n");
#endif /* HAVE_OPENSSL */
  exit(0);
  return 0;					/* Keep some compilers happy */
}
