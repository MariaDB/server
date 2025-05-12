/***************** FilAmDbf H Declares Source Code File (.H) ****************/
/*  Name: filamdbf.h    Version 1.4                                         */
/*                                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2014         */
/*                                                                          */
/*  This file contains the DBF file access method classes declares.         */
/****************************************************************************/

#ifndef __FILAMDBF_H
#define __FILAMDBF_H

#include "filamfix.h"
#include "filamap.h"

typedef class DBFBASE *PDBF;
typedef class DBFFAM  *PDBFFAM;
typedef class DBMFAM  *PDBMFAM;

/****************************************************************************/
/*  Functions used externally.                                              */
/****************************************************************************/
PQRYRES DBFColumns(PGLOBAL g, PCSZ dp, PCSZ fn, PTOS tiop, bool info);

/****************************************************************************/
/*  This is the base class for dBASE file access methods.                   */
/****************************************************************************/
class DllExport DBFBASE {
 public:
  // Constructors
  DBFBASE(PDOSDEF tdp);
  DBFBASE(PDBF txfp);

  // Implementation
  int  ScanHeader(PGLOBAL g, PCSZ fname, int lrecl, int *rlen, PCSZ defpath);

 protected:
  // Default constructor, not to be used
  DBFBASE(void) = default;

  // Members
  int  Records;                     /*  records in the file                 */
  bool Accept;                      /*  true if bad lines are accepted      */
  int  Nerr;                        /*  Number of bad records               */
  int  Maxerr;                      /*  Maximum number of bad records       */
  int  ReadMode;                    /*  1: ALL 2: DEL 0: NOT DEL            */
  }; // end of class DBFBASE

/****************************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for DBase files.   */
/****************************************************************************/
class DllExport DBFFAM : public FIXFAM, public DBFBASE {
 public:
  // Constructors
  DBFFAM(PDOSDEF tdp) : FIXFAM(tdp), DBFBASE(tdp) {}
  DBFFAM(PDBFFAM txfp) : FIXFAM(txfp), DBFBASE((PDBF)txfp) {}

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_DBF;}
  PTXF Duplicate(PGLOBAL g) override
                {return (PTXF)new(g) DBFFAM(this);}

  // Methods
  int  GetNerr(void) override {return Nerr;}
  int  Cardinality(PGLOBAL g) override;
  bool OpenTableFile(PGLOBAL g) override;
  bool AllocateBuffer(PGLOBAL g) override;
  void ResetBuffer(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  DeleteRecords(PGLOBAL g, int irc) override;
  void CloseTableFile(PGLOBAL g, bool abort) override;
  void Rewind(void) override;

 protected:
  bool CopyHeader(PGLOBAL g) override;
//virtual int  InitDelete(PGLOBAL g, int fpos, int spos);

  // Members
  }; // end of class DBFFAM

/****************************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for DBase files    */
/*  using file mapping to access the file.                                  */
/****************************************************************************/
class DllExport DBMFAM : public MPXFAM, public DBFBASE {
 public:
  // Constructors
  DBMFAM(PDOSDEF tdp) : MPXFAM(tdp), DBFBASE(tdp) {}
  DBMFAM(PDBMFAM txfp) : MPXFAM(txfp), DBFBASE((PDBF)txfp) {}

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_DBF;}
  PTXF Duplicate(PGLOBAL g) override
                {return (PTXF)new(g) DBMFAM(this);}
   int  GetDelRows(void) override;

  // Methods
  int  GetNerr(void) override {return Nerr;}
  int  Cardinality(PGLOBAL g) override;
  bool AllocateBuffer(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  DeleteRecords(PGLOBAL g, int irc) override;
  void Rewind(void) override;

 protected:
  // Members
  }; // end of class DBFFAM

#endif // __FILAMDBF_H
