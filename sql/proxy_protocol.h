#include "my_net.h"

struct proxy_peer_info
{
  struct sockaddr_storage peer_addr;
  int port;
  bool is_local_command;
};

extern bool has_proxy_protocol_header(NET *net);
extern int parse_proxy_protocol_header(NET *net, proxy_peer_info *peer_info);
extern bool is_proxy_protocol_allowed(const sockaddr *remote_addr);

extern int  set_proxy_protocol_networks(const char *spec);
extern void cleanup_proxy_protocol_networks();
