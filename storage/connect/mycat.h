/* Copyright (C) Olivier Bertrand 2004 - 2015

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/**************** MYCAT H Declares Source Code File (.H) ***************/
/*  Name: MYCAT.H  Version 2.3                                         */
/*                                                                     */
/*  This file contains the CONNECT plugin MYCAT class definitions.     */
/***********************************************************************/
#ifndef __MYCAT__H
#define __MYCAT__H

#include "block.h"
#include "catalog.h"

//typedef struct ha_table_option_struct TOS, *PTOS;

/**
  structure for CREATE TABLE options (table options)

  These can be specified in the CREATE TABLE:
  CREATE TABLE ( ... ) {...here...}
*/
struct ha_table_option_struct {
  const char *type;
  const char *filename;
  const char *optname;
  const char *tabname;
  const char *tablist;
  const char *dbname;
  const char *separator;
//const char *connect;
  const char *qchar;
  const char *module;
  const char *subtype;
  const char *catfunc;
  const char *srcdef;
  const char *colist;
  const char *oplist;
  const char *data_charset;
  ulonglong lrecl;
  ulonglong elements;
//ulonglong estimate;
  ulonglong multiple;
  ulonglong header;
  ulonglong quoted;
  ulonglong ending;
  ulonglong compressed;
  bool mapped;
  bool huge;
  bool split;
  bool readonly;
  bool sepindex;
  };

// Possible value for catalog functions
#define FNC_NO      (1 << 0)    // Not a catalog table         
#define FNC_COL     (1 << 1)    // Column catalog function     
#define FNC_TABLE   (1 << 2)    // Table catalog function      
#define FNC_DSN     (1 << 3)    // Data Source catalog function
#define FNC_DRIVER  (1 << 4)    // Column catalog function     
#define FNC_NIY     (1 << 5)    // Catalog function NIY        

typedef class ha_connect     *PHC;

char *GetPluginDir(void);
TABTYPE GetTypeID(const char *type);
bool    IsFileType(TABTYPE type);
bool    IsExactType(TABTYPE type);
bool    IsTypeNullable(TABTYPE type);
bool    IsTypeFixed(TABTYPE type);
bool    IsTypeIndexable(TABTYPE type);
int     GetIndexType(TABTYPE type);
uint    GetFuncID(const char *func);

/***********************************************************************/
/*  MYCAT: class for managing the CONNECT plugin DB items.             */
/***********************************************************************/
class MYCAT : public CATALOG {
 public:
  MYCAT(PHC hc);                       // Constructor

  // Implementation
  PHC     GetHandler(void) {return Hc;}
  void    SetHandler(PHC hc) {Hc= hc;}

  // Methods
  void    Reset(void);
//void    SetDataPath(PGLOBAL g, const char *path) 
//            {SetPath(g, &DataPath, path);}
  bool    StoreIndex(PGLOBAL, PTABDEF) {return false;}  // Temporary
//  PRELDEF GetTableDesc(PGLOBAL g, LPCSTR name,
	PRELDEF GetTableDesc(PGLOBAL g, PTABLE tablep,
		                   LPCSTR type, PRELDEF *prp = NULL);
  PTDB    GetTable(PGLOBAL g, PTABLE tablep, 
                              MODE mode = MODE_READ, LPCSTR type = NULL);
  void    ClearDB(PGLOBAL g);

 protected:
//  PRELDEF MakeTableDesc(PGLOBAL g, LPCSTR name, LPCSTR am);
	PRELDEF MakeTableDesc(PGLOBAL g, PTABLE tablep, LPCSTR am);
	//void    SetPath(PGLOBAL g, LPCSTR *datapath, const char *path);

  // Members
  ha_connect *Hc;                          // The Connect handler
  }; // end of class MYCAT

#endif /* __MYCAT__H */
