// TABWMI.H     Olivier Bertrand    2012
// WMI: Virtual table to Get WMI information
#define _WIN32_DCOM
#include <wbemidl.h>
# pragma comment(lib, "wbemuuid.lib")
#include <iostream>
using namespace std;
#include <comdef.h>

/***********************************************************************/
/*  Definitions.                                                       */
/***********************************************************************/
typedef class WMIDEF *PWMIDEF;
typedef class TDBWMI *PTDBWMI;
typedef class WMICOL *PWMICOL;
typedef class TDBWCL *PTDBWCL;
typedef class WCLCOL *PWCLCOL;

/***********************************************************************/
/*  Structure used by WMI column info functions.                       */
/***********************************************************************/
typedef struct _WMIutil {
  IWbemServices    *Svc;
  IWbemClassObject *Cobj;
} WMIUTIL, *PWMIUT;

/***********************************************************************/
/*  Functions used externally.                                         */
/***********************************************************************/
PQRYRES WMIColumns(PGLOBAL g, char *nsp, char *classname, PWMIUT wp);

/* -------------------------- WMI classes ---------------------------- */

/***********************************************************************/
/*  WMI: Virtual table to get the WMI information.                     */
/***********************************************************************/
class WMIDEF : public TABDEF {            /* Logical table description */
  friend class TDBWMI;
  friend class TDBWCL;
 public:
  // Constructor
  WMIDEF(void) 
    {Pseudo = 3; Nspace = NULL; Wclass = NULL; Ems = 0; Info = false;}

  // Implementation
  virtual const char *GetType(void) {return "WMI";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);
  virtual bool DeleteTableFile(PGLOBAL g) {return true;}

 protected:
  // Members
  char   *Nspace;
  char   *Wclass;
  int     Ems;
  bool    Info;
  }; // end of WMIDEF

/***********************************************************************/
/*  This is the class declaration for the WMI table.                   */
/***********************************************************************/
class TDBWMI : public TDBASE {
  friend class WMICOL;
 public:
  // Constructor
  TDBWMI(PWMIDEF tdp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_WMI;}

  // Methods
  virtual int GetRecpos(void);
  virtual int GetProgCur(void) {return N;}
  virtual int RowNumber(PGLOBAL g, bool b = false) {return N + 1;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Specific routines
          bool  Initialize(PGLOBAL g);
          char *MakeWQL(PGLOBAL g);
          void  DoubleSlash(PGLOBAL g);
          bool  GetWMIInfo(PGLOBAL g);

  // Members
  IWbemServices        *Svc;      // IWbemServices pointer
  IEnumWbemClassObject *Enumerator;
  IWbemClassObject     *ClsObj;
  char                 *Nspace;    // Namespace
  char                 *Wclass;    // Class name
  char                 *ObjPath;  // Used for direct access
  char                 *Kvp;      // Itou
  int                    Ems;      // Estimated max size
  PCOL                  Kcol;     // Key column
  HRESULT               Res;
  PVBLK                 Vbp;
  bool                  Init;
  bool                  Done;
  ULONG                  Rc;
  int                   N;        // Row number
  }; // end of class TDBWMI

/***********************************************************************/
/*  Class WMICOL: WMI Address column.                                  */
/***********************************************************************/
class WMICOL : public COLBLK {
  friend class TDBWMI;
 public:
  // Constructors
  WMICOL(PCOLDEF cdp, PTDB tdbp, int n);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_WMI;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  WMICOL(void) {}              // Default constructor not to be used

  // Members
  PTDBWMI Tdbp;                // Points to WMI table block
  VARIANT Prop;               // Property value
  CIMTYPE Ctype;              // CIM Type
  HRESULT Res;
  }; // end of class WMICOL

/***********************************************************************/
/*  This is the class declaration for the WCL table.                   */
/***********************************************************************/
class TDBWCL : public TDBASE {
  friend class WCLCOL;
 public:
  // Constructor
  TDBWCL(PWMIDEF tdp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_WMI;}

  // Methods
  virtual int  GetRecpos(void) {return N;}
  virtual int  GetProgCur(void) {return N;}
  virtual int  RowNumber(PGLOBAL g, bool b = false) {return N + 1;}

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Specific routines
          bool Initialize(PGLOBAL g);

  // Members
  IWbemServices    *Svc;      // IWbemServices pointer
  IWbemClassObject *ClsObj;
  BSTR              Propname;
  char             *Nspace;    // Namespace
  char             *Wclass;    // Class name
  HRESULT           Res;
  bool              Init;
  bool              Done;
  int               N;        // Row number
  int                Lng;
  int                Typ;
  int                Prec;
  }; // end of class TDBWCL

/***********************************************************************/
/*  Class WMICOL: WMI Address column.                                  */
/***********************************************************************/
class WCLCOL : public COLBLK {
  friend class TDBWCL;
 public:
  // Constructors
  WCLCOL(PCOLDEF cdp, PTDB tdbp, int n);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_WMI;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  WCLCOL(void) {}              // Default constructor not to be used

  // Members
  PTDBWCL Tdbp;                // Points to WMI table block
  HRESULT Res;
  int     Flag;
  }; // end of class WCLCOL
