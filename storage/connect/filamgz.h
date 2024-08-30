/*************** FilAmGz H Declares Source Code File (.H) **************/
/*  Name: FILAMGZ.H    Version 1.3                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2016    */
/*                                                                     */
/*  This file contains the GZIP access method classes declares.        */
/***********************************************************************/
#ifndef __FILAMGZ_H
#define __FILAMGZ_H

#include "zlib.h"

typedef class GZFAM *PGZFAM;
typedef class ZBKFAM *PZBKFAM;
typedef class GZXFAM *PZIXFAM;
typedef class ZLBFAM *PZLBFAM;

/***********************************************************************/
/*  This is the access method class declaration for not optimized      */
/*  variable record length files compressed using the gzip library     */
/*  functions. File is accessed record by record (row).                */
/***********************************************************************/
class DllExport GZFAM : public TXTFAM {
//  friend class DOSCOL;
 public:
  // Constructor
  GZFAM(PDOSDEF tdp) : TXTFAM(tdp) {Zfile = NULL; Zpos = 0;}
  GZFAM(PGZFAM txfp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_GZ;}
  int  GetPos(void) override;
  int  GetNextPos(void) override;
  PTXF Duplicate(PGLOBAL g) override
                {return (PTXF)new(g) GZFAM(this);}

  // Methods
  void Reset(void) override;
  int  GetFileLength(PGLOBAL g) override;
  int  Cardinality(PGLOBAL g) override {return (g) ? -1 : 0;}
  int  MaxBlkSize(PGLOBAL g, int s) override {return s;}
  bool AllocateBuffer(PGLOBAL g) override;
  int  GetRowID(void) override;
  bool RecordPos(PGLOBAL g) override;
  bool SetPos(PGLOBAL g, int recpos) override;
  int  SkipRecord(PGLOBAL g, bool header) override;
  bool OpenTableFile(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  WriteBuffer(PGLOBAL g) override;
  int  DeleteRecords(PGLOBAL g, int irc) override;
  void CloseTableFile(PGLOBAL g, bool abort) override;
  void Rewind(void) override;

 protected:
          int  Zerror(PGLOBAL g);    // GZ error function

  // Members
  gzFile  Zfile;              // Points to GZ file structure
  z_off_t Zpos;               // Uncompressed file position
  }; // end of class GZFAM

/***********************************************************************/
/*  This is the access method class declaration for optimized variable */
/*  record length files compressed using the gzip library functions.   */
/*  The File is accessed by block (requires an opt file).              */
/***********************************************************************/
class DllExport ZBKFAM : public GZFAM {
 public:
  // Constructor
  ZBKFAM(PDOSDEF tdp);
  ZBKFAM(PZBKFAM txfp);

  // Implementation
  int  GetPos(void) override;
  int  GetNextPos(void) override {return 0;}
  PTXF Duplicate(PGLOBAL g) override
                {return (PTXF)new(g) ZBKFAM(this);}

  // Methods
  int  Cardinality(PGLOBAL g) override;
  int  MaxBlkSize(PGLOBAL g, int s) override;
  bool AllocateBuffer(PGLOBAL g) override;
  int  GetRowID(void) override;
  bool RecordPos(PGLOBAL g) override;
  int  SkipRecord(PGLOBAL g, bool header) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  WriteBuffer(PGLOBAL g) override;
  int  DeleteRecords(PGLOBAL g, int irc) override;
  void CloseTableFile(PGLOBAL g, bool abort) override;
  void Rewind(void) override;

 protected:
  // Members
  char *CurLine;              // Position of current line in buffer
  char *NxtLine;              // Position of Next    line in buffer
  bool  Closing;              // True when closing on Insert
  }; // end of class ZBKFAM

/***********************************************************************/
/*  This is the access method class declaration for fixed record       */
/*  length files compressed using the gzip library functions.          */
/*  The file is always accessed by block.                              */
/***********************************************************************/
class DllExport GZXFAM : public ZBKFAM {
 public:
  // Constructor
  GZXFAM(PDOSDEF tdp);
  GZXFAM(PZIXFAM txfp) : ZBKFAM(txfp) {}

  // Implementation
  int  GetNextPos(void) override {return 0;}
  PTXF Duplicate(PGLOBAL g) override
                {return (PTXF)new(g) GZXFAM(this);}

  // Methods
  int  Cardinality(PGLOBAL g) override;
  bool AllocateBuffer(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  WriteBuffer(PGLOBAL g) override;

 protected:
  // No additional Members
  }; // end of class GZXFAM

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for PlugDB    */
/*  fixed/variable files compressed using the zlib library functions.  */
/*  Physically these are written and read using the same technique     */
/*  than blocked variable files, only the contain of each block is     */
/*  compressed using the deflate zlib function. The purpose of this    */
/*  specific format is to have a fast mechanism for direct access of   */
/*  records so blocked optimization is fast and direct access (joins)  */
/*  is allowed. Note that the block length is written ahead of each    */
/*  block to enable reading when optimization file is not available.   */
/***********************************************************************/
class DllExport ZLBFAM : public BLKFAM {
 public:
  // Constructor
  ZLBFAM(PDOSDEF tdp);
  ZLBFAM(PZLBFAM txfp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_ZLIB;}
  int  GetPos(void) override;
  int  GetNextPos(void) override;
  PTXF Duplicate(PGLOBAL g) override
                  {return (PTXF)new(g) ZLBFAM(this);}
  inline  void SetOptimized(bool b) {Optimized = b;}

  // Methods
  int  GetFileLength(PGLOBAL g) override;
  bool SetPos(PGLOBAL g, int recpos) override;
  bool AllocateBuffer(PGLOBAL g) override;
  int  ReadBuffer(PGLOBAL g) override;
  int  WriteBuffer(PGLOBAL g) override;
  void CloseTableFile(PGLOBAL g, bool abort) override;
  void Rewind(void) override;

 protected:
          bool WriteCompressedBuffer(PGLOBAL g);
          int  ReadCompressedBuffer(PGLOBAL g, void *rdbuf);

  // Members
  z_streamp Zstream;          // Compression/decompression stream
  Byte     *Zbuffer;          // Compressed block buffer
  int      *Zlenp;            // Pointer to block length
  bool      Optimized;        // true when opt file is available
  }; // end of class ZLBFAM

#endif // __FILAMGZ_H
