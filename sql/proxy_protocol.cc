/* Copyright (c) 2017, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */

#include <mariadb.h>
#include <mysql.h>
#include <mysql_com.h>
#include <mysqld_error.h>
#include <my_sys.h>
#include <m_string.h>
#include <my_net.h>
#include <violite.h>
#include <proxy_protocol.h>
#include <log.h>

#define PROXY_PROTOCOL_V1_SIGNATURE "PROXY"
#define PROXY_PROTOCOL_V2_SIGNATURE "\x0D\x0A\x0D\x0A\x00\x0D\x0A\x51\x55\x49\x54\x0A"
#define MAX_PROXY_HEADER_LEN 256

/*
  Parse proxy protocol version 1 header (text)
*/
static int parse_v1_header(char *hdr, size_t len, proxy_peer_info *peer_info)
{
  char address_family[MAX_PROXY_HEADER_LEN + 1];
  char client_address[MAX_PROXY_HEADER_LEN + 1];
  char server_address[MAX_PROXY_HEADER_LEN + 1];
  int client_port;
  int server_port;

  int ret = sscanf(hdr, "PROXY %s %s %s %d %d",
    address_family, client_address, server_address,
    &client_port, &server_port);

  if (ret != 5)
  {
    if (ret >= 1 && !strcmp(address_family, "UNKNOWN"))
    {
      peer_info->is_local_command= true;
      return 0;
    }
    return -1;
  }

  if (client_port < 0 || client_port > 0xffff
    || server_port < 0 || server_port > 0xffff)
    return -1;

  if (!strcmp(address_family, "UNKNOWN"))
  {
    peer_info->is_local_command= true;
    return 0;
  }
  else if (!strcmp(address_family, "TCP4"))
  {
    /* Initialize IPv4 peer address.*/
    peer_info->peer_addr.ss_family= AF_INET;
    if (!inet_pton(AF_INET, client_address,
      &((struct sockaddr_in *)(&peer_info->peer_addr))->sin_addr))
      return -1;
  }
  else if (!strcmp(address_family, "TCP6"))
  {
    /* Initialize IPv6 peer address.*/
    peer_info->peer_addr.ss_family= AF_INET6;
    if (!inet_pton(AF_INET6, client_address,
      &((struct sockaddr_in6 *)(&peer_info->peer_addr))->sin6_addr))
      return -1;
  }
  peer_info->port= client_port;
  /* Check if server address is legal.*/
  char addr_bin[16];
  if (!inet_pton(peer_info->peer_addr.ss_family,
    server_address, addr_bin))
    return -1;

  return 0;
}


/*
  Parse proxy protocol V2 (binary) header
*/
static int parse_v2_header(uchar *hdr, size_t len,proxy_peer_info *peer_info)
{
  /* V2 Signature */
  if (memcmp(hdr, PROXY_PROTOCOL_V2_SIGNATURE, 12))
    return -1;

  /* version  + command */
  uint8 ver= (hdr[12] & 0xF0);
  if (ver != 0x20)
    return -1; /* Wrong version*/

  uint cmd= (hdr[12] & 0xF);

  /* Address family */
  uchar fam= hdr[13];

  if (cmd == 0)
  {
    /* LOCAL command*/
    peer_info->is_local_command= true;
    return 0;
  }

  if (cmd != 0x01)
  {
    /* Not PROXY COMMAND */
    return -1;
  }

  struct sockaddr_in *sin= (struct sockaddr_in *)(&peer_info->peer_addr);
  struct sockaddr_in6 *sin6= (struct sockaddr_in6 *)(&peer_info->peer_addr);
  switch (fam)
  {
    case 0x11:  /* TCPv4 */
      sin->sin_family= AF_INET;
      memcpy(&(sin->sin_addr), hdr + 16, 4);
      peer_info->port= (hdr[24] << 8) + hdr[25];
      break;
    case 0x21:  /* TCPv6 */
      sin6->sin6_family= AF_INET6;
      memcpy(&(sin6->sin6_addr), hdr + 16, 16);
      peer_info->port= (hdr[48] << 8) + hdr[49];
      break;
    case 0x31: /* AF_UNIX, stream */
      peer_info->peer_addr.ss_family= AF_UNIX;
      break;
    default:
      return -1;
  }
  return 0;
}


bool has_proxy_protocol_header(NET *net)
{
  compile_time_assert(NET_HEADER_SIZE < sizeof(PROXY_PROTOCOL_V1_SIGNATURE));
  compile_time_assert(NET_HEADER_SIZE < sizeof(PROXY_PROTOCOL_V2_SIGNATURE));

  const uchar *preread_bytes= net->buff + net->where_b;
  return !memcmp(preread_bytes, PROXY_PROTOCOL_V1_SIGNATURE, NET_HEADER_SIZE)||
      !memcmp(preread_bytes, PROXY_PROTOCOL_V2_SIGNATURE, NET_HEADER_SIZE);
}


/**
  Try to parse proxy header.
  https://www.haproxy.org/download/1.8/doc/proxy-protocol.txt

  Whenever this function is called, client is connecting, and
  we have have pre-read 4 bytes (NET_HEADER_SIZE)  from the network already.
  These 4 bytes did not match MySQL packet header, and (unless the client
  is buggy), those bytes must be proxy header.

   @param[in]  net - vio and already preread bytes from the header
   @param[out] peer_info - parsed proxy header with client host and port
   @return 0 in case of success, -1 if error.
*/
int parse_proxy_protocol_header(NET *net, proxy_peer_info *peer_info)
{
  uchar hdr[MAX_PROXY_HEADER_LEN];
  size_t pos= 0;

  DBUG_ASSERT(!net->compress);
  const uchar *preread_bytes= net->buff + net->where_b;
  bool have_v1_header= !memcmp(preread_bytes, PROXY_PROTOCOL_V1_SIGNATURE, NET_HEADER_SIZE);
  bool have_v2_header=
    !have_v1_header && !memcmp(preread_bytes, PROXY_PROTOCOL_V2_SIGNATURE, NET_HEADER_SIZE);
  if (!have_v1_header && !have_v2_header)
  { 
    // not a proxy protocol header
    return -1;
  }
  memcpy(hdr, preread_bytes, NET_HEADER_SIZE);
  pos= NET_HEADER_SIZE;
  Vio *vio= net->vio;
  memset(peer_info, 0, sizeof (*peer_info));

  if (have_v1_header)
  {
    /* Read until end of header (newline character)*/
    while(pos < sizeof(hdr))
    {
      long len= (long)vio_read(vio, hdr + pos, 1);
      if (len < 0)
        return -1;
      pos++;
      if (hdr[pos-1] == '\n')
        break;
    }
    hdr[pos]= 0;

    if (parse_v1_header((char *)hdr, pos, peer_info))
      return -1;
  }
  else // if (have_v2_header)
  {
#define PROXY_V2_HEADER_LEN 16
    /* read off 16 bytes of the header.*/
    long len= vio_read(vio, hdr + pos, PROXY_V2_HEADER_LEN - pos);
    if (len < 0)
      return -1;
    // 2 last bytes are the length in network byte order of the part following header
    ushort trail_len= ((ushort)hdr[PROXY_V2_HEADER_LEN-2] >> 8) + hdr[PROXY_V2_HEADER_LEN-1];
    if (trail_len > sizeof(hdr) - PROXY_V2_HEADER_LEN)
      return -1;
    len= vio_read(vio,  hdr + PROXY_V2_HEADER_LEN, trail_len);
    pos= PROXY_V2_HEADER_LEN + trail_len;
    if (parse_v2_header(hdr, pos, peer_info))
      return -1;
  }

  if (peer_info->peer_addr.ss_family == AF_INET6)
  {
    /*
      Normalize IPv4 compatible or mapped IPv6 addresses.
      They will be treated as IPv4.
    */
    sockaddr_storage tmp;
    int dst_len;
    memset(&tmp, 0, sizeof(tmp));
    vio_get_normalized_ip((const struct sockaddr *)&peer_info->peer_addr,
      sizeof(sockaddr_storage), (struct sockaddr *)&tmp, &dst_len);
    memcpy(&peer_info->peer_addr, &tmp, (size_t)dst_len);
  }
  return 0;
}


/**
 CIDR address matching etc (for the proxy_protocol_networks parameter)
*/

/**
  Subnetwork address in CIDR format, e.g
  192.168.1.0/24 or 2001:db8::/32
*/
struct subnet
{
  char addr[16]; /* Binary representation of the address, big endian*/
  unsigned short family; /* Address family, AF_INET or AF_INET6 */
  unsigned short bits; /* subnetwork size */
};

static  subnet* proxy_protocol_subnets;
size_t  proxy_protocol_subnet_count;

#define MAX_MASK_BITS(family) (family == AF_INET ? 32 : 128)


/** Convert IPv4 that are compat or mapped IPv4 to "normal" IPv4 */
static int normalize_subnet(struct subnet *subnet)
{
  unsigned char *addr= (unsigned char*)subnet->addr;
  if (subnet->family == AF_INET6)
  {
    const struct in6_addr *src_ip6=(in6_addr *)addr;
    if (IN6_IS_ADDR_V4MAPPED(src_ip6) || IN6_IS_ADDR_V4COMPAT(src_ip6))
    {
      /* Copy the actual IPv4 address (4 last bytes) */
      if (subnet->bits < 96)
        return -1;
      subnet->family= AF_INET;
      memcpy(addr, addr+12, 4);
      subnet->bits -= 96;
    }
  }
  return 0;
}

/**
  Convert string representation of a subnet to subnet struct.
*/
static int parse_subnet(char *addr_str, struct subnet *subnet)
{
  if (strchr(addr_str, ':'))
    subnet->family= AF_INET6;
  else if (strchr(addr_str, '.'))
    subnet->family= AF_INET;
  else if (!strcmp(addr_str, "localhost"))
  {
    subnet->family= AF_UNIX;
    subnet->bits= 0;
    return 0;
  }

  char *pmask= strchr(addr_str, '/');
  if (!pmask)
  {
    subnet->bits= MAX_MASK_BITS(subnet->family);
  }
  else
  {
    *pmask= 0;
    pmask++;
    int b= 0;

    do
    {
      if (*pmask < '0' || *pmask > '9')
        return -1;
      b= 10 * b + *pmask - '0';
      if (b > MAX_MASK_BITS(subnet->family))
        return -1;
      pmask++;
    }
    while (*pmask);

    subnet->bits= (unsigned short)b;
  }

  if (!inet_pton(subnet->family, addr_str, subnet->addr))
    return -1;

  if (normalize_subnet(subnet))
    return -1;

  return 0;
}

/**
  Parse comma separated string subnet list into subnets array,
  which is stored in 'proxy_protocol_subnets' variable

  @param[in] subnets_str : networks in CIDR format,
    separated by comma and/or space

  @return 0 if success, otherwise -1
*/
int set_proxy_protocol_networks(const char *subnets_str)
{
  if (!subnets_str || !*subnets_str)
    return 0;

  size_t max_subnets= MY_MAX(3,strlen(subnets_str)/2);
  proxy_protocol_subnets= (subnet *)my_malloc(max_subnets * sizeof(subnet),MY_ZEROFILL);

  /* Check for special case '*'. */
  if (strcmp(subnets_str, "*") == 0)
  {

    proxy_protocol_subnets[0].family= AF_INET;
    proxy_protocol_subnets[1].family= AF_INET6;
    proxy_protocol_subnets[2].family= AF_UNIX;
    proxy_protocol_subnet_count= 3;
    return 0;
  }

  char token[256];
  const char *p= subnets_str;
  for(proxy_protocol_subnet_count= 0;; proxy_protocol_subnet_count++)
  {
    while(*p && (*p ==',' || *p == ' '))
      p++;
    if (!*p)
      break;

    size_t cnt= 0;
    while(*p && *p != ',' && *p != ' ' && cnt < sizeof(token)-1)
      token[cnt++]= *p++;

    token[cnt++]=0;
    if (cnt ==  sizeof(token))
      return -1;

    if (parse_subnet(token, &proxy_protocol_subnets[proxy_protocol_subnet_count]))
    {
      sql_print_error("Error parsing proxy_protocol_networks parameter, near '%s'",token);
      return -1;
    }
  }
  return 0;
}

/**
   Compare memory areas, in memcmp().similar fashion.
   The difference to memcmp() is that size parameter is the
   bit count, not byte count.
*/
static int compare_bits(const void *s1, const void *s2, int bit_count)
{
  int result= 0;
  int byte_count= bit_count / 8;
  if (byte_count && (result= memcmp(s1, s2, byte_count)))
    return result;
  int rem= byte_count % 8;
  if (rem)
  {
    // compare remaining bits i.e partial bytes.
    unsigned char s1_bits= (((char *)s1)[byte_count]) >> (8 - rem);
    unsigned char s2_bits= (((char *)s2)[byte_count]) >> (8 - rem);
    if (s1_bits > s2_bits)
      return 1;
    if (s1_bits < s2_bits)
      return -1;
  }
  return 0;
}

/**
  Check whether networks address matches network.
*/
bool addr_matches_subnet(const sockaddr *sock_addr, const subnet *subnet)
{
  DBUG_ASSERT(subnet->family == AF_UNIX ||
    subnet->family == AF_INET ||
    subnet->family == AF_INET6);

  if (sock_addr->sa_family != subnet->family)
    return false;

  if (subnet->family == AF_UNIX)
    return true;

  void *addr= (subnet->family == AF_INET) ?
    (void *)&((struct sockaddr_in *)sock_addr)->sin_addr :
    (void *)&((struct sockaddr_in6 *)sock_addr)->sin6_addr;

  return (compare_bits(subnet->addr, addr, subnet->bits) == 0);
}


/**
  Check whether proxy header from client is allowed, as per
  specification in 'proxy_protocol_networks' server variable.

  The non-TCP "localhost" clients (unix socket, shared memory, pipes)
  are accepted whenever 127.0.0.1 accepted  in 'proxy_protocol_networks'
*/
bool is_proxy_protocol_allowed(const sockaddr *addr)
{
  if (proxy_protocol_subnet_count == 0)
    return false;

  sockaddr_storage addr_storage;
  struct sockaddr *normalized_addr= (struct sockaddr *)&addr_storage;

  /*
   Non-TCP addresses (unix domain socket, windows pipe and shared memory
   gets tranlated to TCP4 localhost address.

   Note, that vio remote addresses are initialized with binary zeros
   for these protocols (which is AF_UNSPEC everywhere).
  */
  switch(addr->sa_family)
  {
    case AF_UNSPEC:
    case AF_UNIX:
      normalized_addr->sa_family= AF_UNIX;
      break;
    case AF_INET:
    case AF_INET6:
      {
      int len=
        (addr->sa_family == AF_INET)?sizeof(sockaddr_in):sizeof (sockaddr_in6);
      int dst_len;
      vio_get_normalized_ip(addr, len,normalized_addr, &dst_len);
      }
      break;
    default:
      DBUG_ASSERT(0);
   }

  for (size_t i= 0; i < proxy_protocol_subnet_count; i++)
    if (addr_matches_subnet(normalized_addr, &proxy_protocol_subnets[i]))
      return true;

  return false;
}


void cleanup_proxy_protocol_networks()
{
  my_free(proxy_protocol_subnets);
  proxy_protocol_subnets= 0;
  proxy_protocol_subnet_count= 0;
}

