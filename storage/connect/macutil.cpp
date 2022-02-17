/***********************************************************************/
/*  MACUTIL: Author Olivier Bertrand --  2008-2012                     */
/*  From the article and sample code by Khalid Shaikh.                 */
/***********************************************************************/
#if defined(_WIN32)
#include "my_global.h"
#else   // !_WIN32
#error This is WINDOWS only DLL
#endif  // !_WIN32
#include "global.h"
#include "plgdbsem.h"
#include "macutil.h"

#if 0   // This is placed here just to know what are the actual values
#define MAX_ADAPTER_DESCRIPTION_LENGTH  128
#define MAX_ADAPTER_NAME_LENGTH         256
#define MAX_ADAPTER_ADDRESS_LENGTH      8
#define DEFAULT_MINIMUM_ENTITIES        32
#define MAX_HOSTNAME_LEN                128
#define MAX_DOMAIN_NAME_LEN             128
#define MAX_SCOPE_ID_LEN                256

#define BROADCAST_NODETYPE              1
#define PEER_TO_PEER_NODETYPE           2
#define MIXED_NODETYPE                  4
#define HYBRID_NODETYPE                 8

#define IP_ADAPTER_DDNS_ENABLED               0x01
#define IP_ADAPTER_REGISTER_ADAPTER_SUFFIX    0x02
#define IP_ADAPTER_DHCP_ENABLED               0x04
#define IP_ADAPTER_RECEIVE_ONLY               0x08
#define IP_ADAPTER_NO_MULTICAST               0x10
#define IP_ADAPTER_IPV6_OTHER_STATEFUL_CONFIG 0x20
#endif // 0

/***********************************************************************/
/*  Implementation of the MACINFO class.                               */
/***********************************************************************/
MACINFO::MACINFO(bool adap, bool fix)
  {
  Fip = NULL;
  Piaf = NULL;
  Curp = NULL;
  Buflen = 0;
  Fix = fix;
  Adap = adap;
  N = -1;
  } // end of MACINFO constructor

/***********************************************************************/
/*  MACINFO: Return an error message.                                  */
/***********************************************************************/
void MACINFO::MakeErrorMsg(PGLOBAL g, DWORD drc)
  {
  if (drc == ERROR_BUFFER_OVERFLOW)
    sprintf(g->Message,
      "GetAdaptersInfo: Buffer Overflow buflen=%d nbofadap=%d",
      Buflen, N);
  else if (drc == ERROR_INVALID_PARAMETER)
    strcpy(g->Message, "GetAdaptersInfo: Invalid parameters");
  else if (drc == ERROR_NO_DATA)
    strcpy(g->Message,
           "No adapter information exists for the local computer");
  else if (drc == ERROR_NOT_SUPPORTED)
    strcpy(g->Message, "GetAdaptersInfo is not supported");
  else
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, GetLastError(),
                  0, g->Message, sizeof(g->Message), NULL);

  } // end of MakeErrorMsg

/***********************************************************************/
/*  MAC: Get the number of found adapters.                             */
/***********************************************************************/
int MACINFO::GetNadap(PGLOBAL g)
  {
  if (N < 0) {
    // Best method
    if (Adap) {
      DWORD drc = GetAdaptersInfo(NULL, &Buflen);

      if (drc == ERROR_SUCCESS)
        N = (Fix) ? 1 : 0;
      else if (drc == ERROR_BUFFER_OVERFLOW)
        N = Buflen / sizeof(IP_ADAPTER_INFO);
      else
        MakeErrorMsg(g, drc);

    } else
      N = (Fix) ? 1 : 0;

#if 0
    // This method returns too many adapters
    DWORD dw, drc = GetNumberOfInterfaces((PDWORD)&dw);

    if (drc == NO_ERROR) {
      N = (int)dw;
      Buflen = N * sizeof(IP_ADAPTER_INFO);
    } else
      MakeErrorMsg(g, 0);
#endif
    } // endif MaxSize

  return N;
  } // end of GetNadap

/***********************************************************************/
/*  GetMacInfo: Get info for all found adapters.                       */
/***********************************************************************/
bool MACINFO::GetMacInfo(PGLOBAL g)
  {
  DWORD drc;

  if (GetNadap(g) < 0)
    return true;
  else if (N == 0)
    return false;

  Piaf = (PIP_ADAPTER_INFO)PlugSubAlloc(g, NULL, Buflen);
  drc = GetAdaptersInfo(Piaf, &Buflen);

  if (drc == ERROR_SUCCESS) {
    Curp = Piaf;               // Curp is the first one
    return false;              // Success
    } // endif drc

  MakeErrorMsg(g, drc);
  return true;
  } // end of GetMacInfo

/***********************************************************************/
/*  GetMacInfo: Get info for all found adapters.                       */
/***********************************************************************/
bool MACINFO::GetFixedInfo(PGLOBAL g)
  {
  ULONG len = (uint)sizeof(FIXED_INFO);
  DWORD drc;

  Fip = (FIXED_INFO*)PlugSubAlloc(g, NULL, len);
  drc = GetNetworkParams(Fip, &len);

  if (drc == ERROR_BUFFER_OVERFLOW) {
    Fip = (FIXED_INFO*)PlugSubAlloc(g, NULL, len);
    drc = GetNetworkParams(Fip, &len);
    } // endif drc

  if (drc != ERROR_SUCCESS) {
    sprintf(g->Message, "GetNetworkParams failed. Rc=%08x\n", drc);
    return true;
    } // endif drc

  return false;
  } // end of GetFip

#if 0
#define IF_OTHER_ADAPTERTYPE 0
#define IF_ETHERNET_ADAPTERTYPE 1
#define IF_TOKEN_RING_ADAPTERTYPE 2
#define IF_FDDI_ADAPTERTYPE 3
#define IF_PPP_ADAPTERTYPE 4
#define IF_LOOPBACK_ADAPTERTYPE 5
#endif // 0

/***********************************************************************/
/*  Get next MAC info.                                                 */
/***********************************************************************/
bool MACINFO::NextMac(void)
  {
  if (Curp)
    Curp = Curp->Next;

  return Curp != NULL;
  } // end of NextMac

/***********************************************************************/
/*  Get the next MAC address elements.                                 */
/***********************************************************************/
bool MACINFO::GetOneInfo(PGLOBAL g, int flag, void *v, int lv)
  {
  char        *p = NULL, buf[260] = "";
  unsigned int i;
  int         n = 0;

  if (!Curp && flag >= 10) {
    // Fix info row, no adapter info available
    switch (flag) {
      case 13:
      case 14:
      case 19:
      case 22:
      case 23:
        break;
      default:
        p = PlugDup(g, "");
      } // endswitch flag

  } else switch (flag) {
    // FIXED INFO
    case 1:                     // Host Name
      p = Fip->HostName;
      break;
    case 2:                     // Domain Name
      p = Fip->DomainName;
      break;
    case 3:                     // DNS IPaddress
      p = (Fip->CurrentDnsServer)
        ? (char*)&Fip->CurrentDnsServer->IpAddress
        : (char*)&Fip->DnsServerList.IpAddress;
      break;
    case 4:                     // Node Type
      n = (int)Fip->NodeType;
      break;
    case 5:                     // Scope ID ???
      p = Fip->ScopeId;
      break;
    case 6:                     // Routing enabled
      n = (int)Fip->EnableRouting;
      break;
    case 7:                     // Proxy enabled
      n = (int)Fip->EnableProxy;
      break;
    case 8:                     // DNS enabled
      n = (int)Fip->EnableDns;
      break;
    // ADAPTERS INFO
    case 10:                    // Name
      p = Curp->AdapterName;
      break;
    case 11:                    // Description
      if ((p = strstr(Curp->Description, " - Packet Scheduler Miniport"))) {
        strncpy(buf, Curp->Description, p - Curp->Description);
        i = (int)(p - Curp->Description);
        strncpy(buf, Curp->Description, i);
        buf[i] = 0;
        p = buf;
      } else if ((p = strstr(Curp->Description,
                  " - Miniport d'ordonnancement de paquets"))) {
        i = (int)(p - Curp->Description);
        strncpy(buf, Curp->Description, i);
        buf[i] = 0;
        p = buf;
      } else
        p = Curp->Description;

      break;
    case 12:                    // MAC Address
      for (p = buf, i = 0; i < Curp->AddressLength; i++) {
        if (i)
          strcat(p++, "-");

        p += sprintf(p, "%.2X", Curp->Address[i]);
        } // endfor i

      p = buf;
      break;
    case 13:                    // Type
#if 0                       // This is not found in the SDK
      switch (Curp->Type) {
        case IF_ETHERNET_ADAPTERTYPE:   p = "Ethernet Adapter";     break;
        case IF_TOKEN_RING_ADAPTERTYPE: p = "Token Ring Adapter";   break;
        case IF_FDDI_ADAPTERTYPE:       p = "FDDI Adapter";         break;
        case IF_PPP_ADAPTERTYPE:        p = "PPP Adapter";          break;
        case IF_LOOPBACK_ADAPTERTYPE:   p = "Loop Back Adapter";    break;
//      case IF_SLIP_ADAPTERTYPE:       p = "Generic Slip Adapter"; break;
        default:
          sprintf(buf, "Other Adapter, type=%d", Curp->Type);
          p = buf;
        } // endswitch Type
#endif // 0
      n = (int)Curp->Type;
      break;
    case 14:                    // DHCP enabled
      n = (int)Curp->DhcpEnabled;
      break;
    case 15:                    // IP Address
      p = (Curp->CurrentIpAddress)
        ? (char*)&Curp->CurrentIpAddress->IpAddress
        : (char*)&Curp->IpAddressList.IpAddress;
      break;
    case 16:                    // Subnet Mask
      p = (Curp->CurrentIpAddress)
        ? (char*)&Curp->CurrentIpAddress->IpMask
        : (char*)&Curp->IpAddressList.IpMask;
      break;
    case 17:                    // Gateway
      p = (char*)&Curp->GatewayList.IpAddress;
      break;
    case 18:                    // DHCP Server
      p = (char*)&Curp->DhcpServer.IpAddress;
      break;
    case 19:                    // Have WINS
      n = (Curp->HaveWins) ? 1 : 0;
      break;
    case 20:                    // Primary WINS
      p = (char*)&Curp->PrimaryWinsServer.IpAddress;
      break;
    case 21:                    // Secondary WINS
      p = (char*)&Curp->SecondaryWinsServer.IpAddress;
      break;
    case 22:                    // Lease obtained
      n = (int)Curp->LeaseObtained;
      break;
    case 23:                    // Lease expires
      n = (int)Curp->LeaseExpires;
      break;
    default:
      sprintf(g->Message, "Invalid flag value %d", flag);
      return true;
    } // endswitch flag

  if (p)
    strncpy((char*)v, p, lv);
  else
    *((int*)v) = n;

  return false;
  } // end of GetOneInfo
