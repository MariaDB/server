/***********************************************************************/
/*  TABMAC: Author Olivier Bertrand -- PlugDB -- 2008-2012             */
/*  From the article and sample code by Khalid Shaikh.                 */
/*  TABMAC: virtual table to get the list of MAC addresses.            */
/***********************************************************************/
#if defined(_WIN32)
#include "my_global.h"
//#include <iphlpapi.h>
#else   // !_WIN32
#error This is a WINDOWS only table type
#endif  // !_WIN32
#include "global.h"
#include "plgdbsem.h"
//#include "catalog.h"
//#include "reldef.h"
#include "xtable.h"
#include "colblk.h"
#include "tabmac.h"

#if 0    // This is placed here just to know what are the actual values
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

/* -------------- Implementation of the MAC classes  ------------------ */

/***********************************************************************/
/*  DefineAM: define specific AM block values from MAC file.           */
/***********************************************************************/
bool MACDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  return false;
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB MACDEF::GetTable(PGLOBAL g, MODE m)
  {
  return new(g) TDBMAC(this);
  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMAC class.                               */
/***********************************************************************/
TDBMAC::TDBMAC(PMACDEF tdp) : TDBASE(tdp)
  {
  FixedInfo = NULL;
  Piaf = NULL;
  Curp = NULL;
  Next = NULL;
  Buflen = 0;
  Fix = false;
  Adap = false;
  N = 0;
  } // end of TDBMAC constructor

/***********************************************************************/
/*  Allocate MAC column description block.                             */
/***********************************************************************/
PCOL TDBMAC::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PCOL colp;

  colp = new(g) MACCOL(cdp, this, n);

  if (cprec) {
    colp->SetNext(cprec->GetNext());
    cprec->SetNext(colp);
  } else {
    colp->SetNext(Columns);
    Columns = colp;
  } // endif cprec

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  MAC: Get the number of found adapters.                             */
/***********************************************************************/
void TDBMAC::MakeErrorMsg(PGLOBAL g, DWORD drc)
  {
  if (drc == ERROR_BUFFER_OVERFLOW)
    sprintf(g->Message, 
      "GetAdaptersInfo: Buffer Overflow buflen=%d maxsize=%d",
      Buflen, MaxSize);
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
/*  GetMacInfo: Get info for all found adapters.                       */
/***********************************************************************/
bool TDBMAC::GetMacInfo(PGLOBAL g)
  {
  DWORD drc;

  if (GetMaxSize(g) < 0)
    return true;
  else if (MaxSize == 0)
    return false;

  Piaf = (PIP_ADAPTER_INFO)PlugSubAlloc(g, NULL, Buflen); 
  drc = GetAdaptersInfo(Piaf, &Buflen);

  if (drc == ERROR_SUCCESS) {
    Next = Piaf;               // Next is the first one
    return false;               // Success
    } // endif drc

  MakeErrorMsg(g, drc);
  return true;
  } // end of GetMacInfo

/***********************************************************************/
/*  GetFixedInfo: Get info for network parameters.                     */
/***********************************************************************/
bool TDBMAC::GetFixedInfo(PGLOBAL g)
  {
  ULONG len;
  DWORD drc;

  FixedInfo = (FIXED_INFO*)PlugSubAlloc(g, NULL, sizeof(FIXED_INFO));
  len = sizeof(FIXED_INFO);
  drc = GetNetworkParams(FixedInfo, &len);
  
  if (drc == ERROR_BUFFER_OVERFLOW) {
    FixedInfo = (FIXED_INFO*)PlugSubAlloc(g, NULL, len);
    drc = GetNetworkParams(FixedInfo, &len);
    } // endif drc

  if (drc != ERROR_SUCCESS) {
    sprintf(g->Message,  "GetNetworkParams failed. Rc=%08x\n", drc);
    return true;
    } // endif drc

  return false;
  } // end of GetFixedInfo

/***********************************************************************/
/*  MAC: Get the number of found adapters.                             */
/***********************************************************************/
int TDBMAC::GetMaxSize(PGLOBAL g)
  {
  if (Use != USE_OPEN)
    // Called from info, Adap and Fix are not set yet
    return 1;

  if (MaxSize < 0) {
    // Best method
    if (Adap) {
      DWORD drc = GetAdaptersInfo(NULL, &(Buflen = 0));

      if (drc == ERROR_SUCCESS)
        MaxSize = (Fix) ? 1 : 0;
      else if (drc == ERROR_BUFFER_OVERFLOW) {
        // sizeof(IP_ADAPTER_INFO) was returning 640 but is now sometimes
        // returning 648 while the Buflen setting remains the same (n*640)
        // >> Of course, the code above contains a race condition.... 
        // if the size of the structure Windows wants to return grows after
        // the first call to GetAdaptersInfo() but before the second call 
        // to GetAdaptersInfo(), the second call to GetAdaptersInfo() will 
        // fail with ERROR_BUFFER_OVERFLOW as well, and your function won't 
        // work (by Jeremy Friesner on stackoverflow.com).
        // That's why we add something to it to be comfortable.
        MaxSize = (Buflen + 600) / sizeof(IP_ADAPTER_INFO);

        // Now Buflen must be updated if 648 is true.
        Buflen = MaxSize * sizeof(IP_ADAPTER_INFO);
      } else
        MakeErrorMsg(g, drc);

    } else
      MaxSize = (Fix) ? 1 : 0;

#if 0
    // This method returns too many adapters
    DWORD dw, drc = GetNumberOfInterfaces((PDWORD)&dw);

    if (drc == NO_ERROR) {
      MaxSize = (int)dw;
      Buflen = MaxSize * sizeof(IP_ADAPTER_INFO);
    } else
      MakeErrorMsg(g, 0);
#endif
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  MAC Access Method opening routine.                                 */
/***********************************************************************/
bool TDBMAC::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, this should not happen.                    */
    /*******************************************************************/
    strcpy(g->Message, "TDBMAC should not be reopened");
    return true;
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* MAC tables cannot be modified.                                  */
    /*******************************************************************/
    strcpy(g->Message, "MAC tables are read only");
    return true;
  } else
    Use = USE_OPEN;

  /*********************************************************************/
  /*  Get the adapters info.                                           */
  /*********************************************************************/
  if (Adap && GetMacInfo(g))
    return true;

  if (Fix && GetFixedInfo(g))
    return true;

  /*********************************************************************/
  /*  All is done.                                                     */
  /*********************************************************************/
  return false;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for MAC access method.                      */
/***********************************************************************/
int TDBMAC::ReadDB(PGLOBAL g)
  {
  Curp = Next;

  if (Curp)
    Next = Curp->Next;
  else if (N || !Fix)
    return RC_EF;
  
  N++;
  return RC_OK;
  } // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for MAC access methods.           */
/***********************************************************************/
int TDBMAC::WriteDB(PGLOBAL g)
  {
  strcpy(g->Message, "MAC tables are read only");
  return RC_FX;
  } // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for MAC access methods.              */
/***********************************************************************/
int TDBMAC::DeleteDB(PGLOBAL g, int irc)
  {
  strcpy(g->Message, "Delete not enabled for MAC tables");
  return RC_FX;
  } // end of DeleteDB

// ------------------------ MACCOL functions ----------------------------

/***********************************************************************/
/*  MACCOL public constructor.                                         */
/***********************************************************************/
MACCOL::MACCOL(PCOLDEF cdp, PTDB tdbp, int n)
      : COLBLK(cdp, tdbp, n)
  {
  Tdbp = (PTDBMAC)tdbp;
  Flag = cdp->GetOffset();

  if (Flag < 10)
    Tdbp->Fix = true;
  else
    Tdbp->Adap = true;

  } // end of MACCOL constructor

/***********************************************************************/
/*  Read the next MAC address elements.                                */
/***********************************************************************/
void MACCOL::ReadColumn(PGLOBAL g)
  {
  // Type conversion is handled by Value set routines
  char            *p = NULL, buf[260] = "";
  unsigned int     i;
  int             n = 0;
  PIP_ADAPTER_INFO adp = Tdbp->Curp;
  FIXED_INFO      *fip = Tdbp->FixedInfo;

  if (!adp && Flag >= 10) {
    // Fix info row, no adapter info available
    switch (Flag) {
      case 13:
      case 14:
      case 19:
      case 22:
      case 23:
        n = 0;
        break;
      default:
        p = PlugDup(g, "");
      } // endswitch Flag

  } else switch (Flag) {
    // FIXED INFO
    case 1:                      // Host Name
      p = fip->HostName;
      break;
    case 2:                      // Domain Name
      p = fip->DomainName;
      break;
    case 3:                      // DNS IPaddress
      p = (fip->CurrentDnsServer)
        ? (char*)&fip->CurrentDnsServer->IpAddress
        : (char*)&fip->DnsServerList.IpAddress;
      break;
    case 4:                      // Node Type
      n = (int)fip->NodeType;
      break;
    case 5:                      // Scope ID ???
      p = fip->ScopeId;
      break;
    case 6:                      // Routing enabled
      n = (int)fip->EnableRouting;
      break;
    case 7:                      // Proxy enabled
      n = (int)fip->EnableProxy;
      break;
    case 8:                      // DNS enabled
      n = (int)fip->EnableDns;
      break;
    // ADAPTERS INFO
    case 10:                    // Name
      p = adp->AdapterName;
      break;
    case 11:                    // Description
      if ((p = strstr(adp->Description, " - Packet Scheduler Miniport"))) {
        strncpy(buf, adp->Description, p - adp->Description);
        i = (int)(p - adp->Description);
        strncpy(buf, adp->Description, i);
        buf[i] = 0;
        p = buf;
      } else if ((p = strstr(adp->Description,
                  " - Miniport d'ordonnancement de paquets"))) {
        i = (int)(p - adp->Description);
        strncpy(buf, adp->Description, i);
        buf[i] = 0;
        p = buf;
      } else
        p = adp->Description;

      break;
    case 12:                    // MAC Address
      for (p = buf, i = 0; i < adp->AddressLength; i++) {
        if (i)
          strcat(p++, "-");
      
        p += sprintf(p, "%.2X", adp->Address[i]);
        } // endfor i

      p = buf;
      break;
    case 13:                    // Type
#if 0                        // This is not found in the SDK
      switch (adp->Type) {
        case IF_ETHERNET_ADAPTERTYPE:   p = "Ethernet Adapter";      break;
        case IF_TOKEN_RING_ADAPTERTYPE: p = "Token Ring Adapter";   break;
        case IF_FDDI_ADAPTERTYPE:       p = "FDDI Adapter";          break;
        case IF_PPP_ADAPTERTYPE:         p = "PPP Adapter";          break;
        case IF_LOOPBACK_ADAPTERTYPE:   p = "Loop Back Adapter";    break;
//      case IF_SLIP_ADAPTERTYPE:        p = "Generic Slip Adapter";  break;
        default:
          sprintf(buf, "Other Adapter, type=%d", adp->Type);
          p = buf;
        } // endswitch Type
#endif // 0
      n = (int)adp->Type;
      break;
    case 14:                    // DHCP enabled
      n = (int)adp->DhcpEnabled;
      break;
    case 15:                    // IP Address
      p = (adp->CurrentIpAddress)
        ? (char*)&adp->CurrentIpAddress->IpAddress
        : (char*)&adp->IpAddressList.IpAddress;
      break;
    case 16:                    // Subnet Mask
      p = (adp->CurrentIpAddress)
        ? (char*)&adp->CurrentIpAddress->IpMask
        : (char*)&adp->IpAddressList.IpMask;
      break;
    case 17:                    // Gateway
      p = (char*)&adp->GatewayList.IpAddress;
      break;
    case 18:                    // DHCP Server
      p = (char*)&adp->DhcpServer.IpAddress;
      break;
    case 19:                    // Have WINS
      n = (adp->HaveWins) ? 1 : 0;
      break;
    case 20:                    // Primary WINS
      p = (char*)&adp->PrimaryWinsServer.IpAddress;
      break;
    case 21:                    // Secondary WINS
      p = (char*)&adp->SecondaryWinsServer.IpAddress;
      break;
    case 22:                    // Lease obtained
      n = (int)adp->LeaseObtained;
      break;
    case 23:                    // Lease expires
      n = (int)adp->LeaseExpires;
      break;
    default:
      if (Buf_Type == TYPE_STRING) {
        sprintf(buf, "Invalid flag value %d", Flag);
        p = buf;
      } else
        n = 0;

    } // endswitch Flag

  if (p)
    Value->SetValue_psz(p);
  else
    Value->SetValue(n);

  } // end of ReadColumn
