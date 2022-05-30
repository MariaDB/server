// TABMAC.H     Olivier Bertrand    2011-2012
// MAC: virtual table to Get Mac Addresses via GetAdaptersInfo
#if defined(_WIN32)
#include <windows.h>
#include <iphlpapi.h>
#else   // !_WIN32
#error This is a WINDOWS only table TYPE
#endif  // !_WIN32

/***********************************************************************/
/*  Definitions.                                                       */
/***********************************************************************/
typedef class MACDEF *PMACDEF;
typedef class TDBMAC *PTDBMAC;
typedef class MACCOL *PMACCOL;

/* -------------------------- MAC classes ---------------------------- */

/***********************************************************************/
/*  MAC: virtual table to get the list of MAC addresses.               */
/***********************************************************************/
class DllExport MACDEF : public TABDEF {  /* Logical table description */
  friend class TDBMAC;
 public:
  // Constructor
  MACDEF(void) {Pseudo = 3;}

  // Implementation
  virtual const char *GetType(void) {return "MAC";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);
//virtual bool DeleteTableFile(PGLOBAL g) {return true;}

 protected:
  // Members
  }; // end of MACDEF

/***********************************************************************/
/*  This is the class declaration for the MAC table.                   */
/***********************************************************************/
class TDBMAC : public TDBASE {
  friend class MACCOL;
 public:
  // Constructor
  TDBMAC(PMACDEF tdp);
//TDBMAC(PGLOBAL g, PTDBMAC tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_MAC;}
//virtual PTDB Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBMAC(g, this);}

  // Methods
//virtual PTDB Clone(PTABS t);
  virtual int GetRecpos(void) {return N;}
  virtual int RowNumber(PGLOBAL g, bool b = false) {return N;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  Cardinality(PGLOBAL g) {return GetMaxSize(g);}
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g) {}

 protected:
  // Specific routines
  bool GetMacInfo(PGLOBAL g);
  bool GetFixedInfo(PGLOBAL g);
  void MakeErrorMsg(PGLOBAL g, DWORD drc);

  // Members
  FIXED_INFO      *FixedInfo;     // Points to fixed info structure
  PIP_ADAPTER_INFO Piaf;         // Points on Adapter info array
  PIP_ADAPTER_INFO Curp;         // Points on current Adapt info
  PIP_ADAPTER_INFO Next;         // Points on next Adapt info
  ULONG            Buflen;       // Buffer length
  bool             Fix;           // true if FixedInfo is needed
  bool             Adap;         // true if Piaf is needed
  int              N;             // Row number
  }; // end of class TDBMAC

/***********************************************************************/
/*  Class MACCOL: MAC Address column.                                  */
/***********************************************************************/
class MACCOL : public COLBLK {
  friend class TDBMAC;
 public:
  // Constructors
  MACCOL(PCOLDEF cdp, PTDB tdbp, int n);
//MACCOL(MACCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_MAC;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  MACCOL(void) {}              // Default constructor not to be used

  // Members
  PTDBMAC Tdbp;                // Points to MAC table block
  int     Flag;               // Indicates what to display
  }; // end of class MACCOL
