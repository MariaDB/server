/* Copyright (C) Olivier Bertrand 2004 - 2011

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

/**************** Cnt H Declares Source Code File (.H) *****************/
/*  Name: CONNECT.H    Version 2.4                                     */
/*  This file contains the some based classes declares.                */
/***********************************************************************/
//#include "xtable.h"                     // Base class declares
#include "filamtxt.h"
#include "tabdos.h"

//typedef struct _tabdesc  *PTABD;        // For friend setting
typedef struct _xinfo        *PXF;
typedef struct _create_xinfo *PCXF;
typedef class TDBDOX         *PTDBDOX;

/***********************************************************************/
/*  Definition of classes XCOLCRT, XIXDEF, XKPDEF, DOXDEF, TDBDOX      */
/*  These classes purpose is chiefly to access protected items!        */
/***********************************************************************/
class XCOLCRT: public COLCRT {
	friend class ha_connect;
	friend bool CntCreateTable(PGLOBAL, char *, PCXF);
 public:
	XCOLCRT(PSZ name) : COLCRT(name) {Nulls= -1;}        // Constructor
	bool HasNulls(void) {return (Nulls != 0);}

private:
	int Nulls;
	}; // end of class XCOLCRT

class DOXDEF: public DOSDEF {
//friend class TDBDOX;
//friend int MakeIndex(PGLOBAL, PTDB, PIXDEF);
	friend int CntIndexInit(PGLOBAL, PTDB, int);
	}; // end of class DOXDEF

/***********************************************************************/
/*  This is the DOS/UNIX Access Method base class declaration.         */
/***********************************************************************/
class TDBDOX: public TDBDOS {
	friend int   MakeIndex(PGLOBAL, PTDB, PIXDEF);
	friend int   CntCloseTable(PGLOBAL, PTDB);
	friend int   CntIndexInit(PGLOBAL, PTDB, int);
	friend RCODE CntIndexRead(PGLOBAL, PTDB, OPVAL, const void*, int);
	friend RCODE CntDeleteRow(PGLOBAL, PTDB, bool);
	friend int   CntIndexRange(PGLOBAL, PTDB, const uchar**, uint*,
														 bool*, key_part_map*);
	friend class ha_connect;
  }; // end of class TDBDOX

class XKPDEF: public KPARTDEF {
	friend class TDBDOX;
	friend class ha_connect;
//friend int CntMakeIndex(PGLOBAL, const char *, PIXDEF);
	friend int CntIndexInit(PGLOBAL, PTDB, int);
 public:
	XKPDEF(const char *name, int n) : KPARTDEF((PSZ)name, n) {HasNulls= false;}
	void   SetNulls(bool b) {HasNulls= b;}

 protected:
	bool   HasNulls;            /* Can have null values                  */
	}; // end of class XKPDEF

RCODE CheckRecord(PGLOBAL g, PTDB tdbp, char *oldbuf, char *newbuf);
