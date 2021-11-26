// MACUTIL.H     Olivier Bertrand    2008-2012
// Get Mac Addresses via GetAdaptersInfo
#if defined(_WIN32)
#include <iphlpapi.h>
#else   // !_WIN32
#error This is WINDOWS only
#endif  // !_WIN32
#include "block.h"

typedef class MACINFO *MACIP;

/***********************************************************************/
/*  This is the class declaration for MACINFO.                         */
/***********************************************************************/
class DllExport MACINFO : public BLOCK {
 public:
  // Constructor
  MACINFO(bool adap, bool fix);

  // Implementation
  int  GetNadap(PGLOBAL g);
  bool GetMacInfo(PGLOBAL g);
  bool GetFixedInfo(PGLOBAL g);
  void MakeErrorMsg(PGLOBAL g, DWORD drc);
  bool NextMac(void);
  bool GetOneInfo(PGLOBAL g, int flag, void *v, int lv);

  // Members
  FIXED_INFO      *Fip;          // Points to fixed info structure
  PIP_ADAPTER_INFO Piaf;         // Points on Adapter info array
  PIP_ADAPTER_INFO Curp;         // Points on current Adapt info
  ULONG            Buflen;       // Buffer length
  bool             Fix;           // true if FixedInfo is needed
  bool             Adap;         // true if Piaf is needed
  int              N;             // Number of adapters
  }; // end of class MACINFO
