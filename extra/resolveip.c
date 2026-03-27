/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* Resolves IP's to hostname and hostnames to IP's */

#define VER "2.4"

#include <my_global.h>
#include <m_ctype.h>
#include <my_sys.h>
#include <m_string.h>
#ifndef WIN32
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#endif
#include <my_net.h>
#include <my_getopt.h>
#include <welcome_copyright_notice.h>


static my_bool silent;

static struct my_option my_long_options[] =
{
  {"help", '?', "Displays this help and exits.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"info", 'I', "Synonym for --help.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"silent", 's', "Be more silent.", &silent, &silent,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Displays version information and exits.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static void usage(void)
{
  print_version();
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,\nand you are welcome to modify and redistribute it under the GPL license\n");
  puts("Get hostname based on IP-address or IP-address based on hostname.\n");
  printf("Usage: %s [OPTIONS] hostname or IP-address\n",my_progname);
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
}


static my_bool
get_one_option(const struct my_option *opt,
	       const char *argument __attribute__((unused)),
               const char *filename __attribute__((unused)))
{
  switch (opt->id) {
  case 'V': print_version(); exit(0);
  case 'I':
  case '?':
    usage();
    my_end(0);
    exit(0);
  }
  return 0;
}

/*static char * load_default_groups[]= { "resolveip","client",0 }; */

static int get_options(int *argc,char ***argv)
{
  int ho_error;

  if ((ho_error=handle_options(argc, argv, my_long_options, get_one_option)))
  {
    my_end(0);
    exit(ho_error);
  }

  if (*argc == 0)
  {
    usage();
    return 1;
  }
  return 0;
} /* get_options */



int main(int argc, char **argv)
{
  char *ip;
  int error=0;

  MY_INIT(argv[0]);

  if (get_options(&argc,&argv))
  {
    my_end(0);
    exit(1);
  }

  while (argc--)
  {
    struct in_addr addr4;
    struct in6_addr addr6;
    int is_ipv4, is_ipv6;

    ip = *argv++;

    is_ipv4= (inet_pton(AF_INET, ip, &addr4) == 1);
    is_ipv6= (!is_ipv4 && inet_pton(AF_INET6, ip, &addr6) == 1);

    if (is_ipv4 || is_ipv6)
    {
      /* Reverse lookup: IP address -> hostname */

      if (is_ipv4)
      {
	in_addr_t taddr= addr4.s_addr;
	if (taddr == htonl(INADDR_BROADCAST))
	{
	  puts("Broadcast");
	  continue;
	}
	if (taddr == htonl(INADDR_ANY))
	{
	  if (!taddr)
	    puts("Null-IP-Addr");
	  else
	    puts("Old-Bcast");
	  continue;
	}
      }
      else if (IN6_IS_ADDR_UNSPECIFIED(&addr6))
      {
	puts("Null-IP-Addr");
	continue;
      }

      {
	struct sockaddr_storage sa;
	socklen_t sa_len;
	char hostname[NI_MAXHOST];
	int err;

	memset(&sa, 0, sizeof(sa));

	if (is_ipv4)
	{
	  struct sockaddr_in *sa4= (struct sockaddr_in *) &sa;
	  sa4->sin_family= AF_INET;
	  sa4->sin_addr= addr4;
	  sa_len= sizeof(struct sockaddr_in);
	}
	else
	{
	  struct sockaddr_in6 *sa6= (struct sockaddr_in6 *) &sa;
	  sa6->sin6_family= AF_INET6;
	  sa6->sin6_addr= addr6;
	  sa_len= sizeof(struct sockaddr_in6);
	}

	err= getnameinfo((struct sockaddr *) &sa, sa_len,
			  hostname, sizeof(hostname), NULL, 0, NI_NAMEREQD);
	if (err == 0)
	{
	  if (silent)
	    puts(hostname);
	  else
	    printf("Host name of %s is %s\n", ip, hostname);
	}
	else
	{
	  error= 2;
	  fflush(stdout);
	  fprintf(stderr, "%s: Unable to find hostname for '%s'\n",
		  my_progname, ip);
	}
      }
    }
    else
    {
      /* Forward lookup: hostname -> IP address(es) */

      struct addrinfo hints, *res, *rp;
      int err;

      memset(&hints, 0, sizeof(hints));
      hints.ai_family= AF_UNSPEC;
      hints.ai_socktype= SOCK_STREAM;

      err= getaddrinfo(ip, NULL, &hints, &res);
      if (err != 0)
      {
	fflush(stdout);
	fprintf(stderr, "%s: Unable to find hostid for '%s': %s\n",
		my_progname, ip, gai_strerror(err));
	error= 2;
      }
      else if (silent)
      {
	char addr_str[INET6_ADDRSTRLEN];
	const void *addr_ptr;

	if (res->ai_family == AF_INET)
	  addr_ptr= &((struct sockaddr_in *) res->ai_addr)->sin_addr;
	else
	  addr_ptr= &((struct sockaddr_in6 *) res->ai_addr)->sin6_addr;

	if (inet_ntop(res->ai_family, addr_ptr, addr_str, sizeof(addr_str)))
	  puts(addr_str);

	freeaddrinfo(res);
      }
      else
      {
	for (rp= res; rp != NULL; rp= rp->ai_next)
	{
	  char addr_str[INET6_ADDRSTRLEN];
	  const void *addr_ptr;

	  if (rp->ai_family == AF_INET)
	    addr_ptr= &((struct sockaddr_in *) rp->ai_addr)->sin_addr;
	  else if (rp->ai_family == AF_INET6)
	    addr_ptr= &((struct sockaddr_in6 *) rp->ai_addr)->sin6_addr;
	  else
	    continue;

	  if (inet_ntop(rp->ai_family, addr_ptr, addr_str, sizeof(addr_str)))
	    printf("IP address of %s is %s\n", ip, addr_str);
	}
	freeaddrinfo(res);
      }
    }
  }
  my_end(0);
  exit(error);
}
