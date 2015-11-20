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
#include "filamtxt.h"
#include "tabdos.h"

//typedef struct _tabdesc  *PTABD;        // For friend setting
typedef struct _xinfo        *PXF;
typedef struct _create_xinfo *PCXF;
typedef class ha_connect     *PHC;
typedef class TDBDOX         *PTDBDOX;

/****************************************************************************/
/*  CONNECT functions called externally.                                    */
/****************************************************************************/
bool  CntCheckDB(PGLOBAL g, PHC handler, const char *pathname);
PTDB  CntGetTDB(PGLOBAL g, const char *name, MODE xmod, PHC);
bool  CntOpenTable(PGLOBAL g, PTDB tdbp, MODE, char *, char *, bool, PHC);
bool  CntRewindTable(PGLOBAL g, PTDB tdbp);
int   CntCloseTable(PGLOBAL g, PTDB tdbp, bool nox, bool abort);
int   CntIndexInit(PGLOBAL g, PTDB tdbp, int id, bool sorted);
RCODE CntReadNext(PGLOBAL g, PTDB tdbp);
RCODE CntIndexRead(PGLOBAL g, PTDB, OPVAL op, const key_range *kr, bool mrr); 
RCODE CntWriteRow(PGLOBAL g, PTDB tdbp);
RCODE CntUpdateRow(PGLOBAL g, PTDB tdbp);
RCODE CntDeleteRow(PGLOBAL g, PTDB tdbp, bool all);
bool  CntInfo(PGLOBAL g, PTDB tdbp, PXF info);
int   CntIndexRange(PGLOBAL g, PTDB ptdb, const uchar* *key, uint *len,
                    bool *incl, key_part_map *kmap);
PGLOBAL CntExit(PGLOBAL g);

/***********************************************************************/
/*  Definition of classes XKPDEF, DOXDEF, TDBDOX                       */
/*  These classes purpose is chiefly to access protected items!        */
/***********************************************************************/
class DOXDEF: public DOSDEF {
  friend int CntIndexInit(PGLOBAL, PTDB, int, bool);
  }; // end of class DOXDEF

/***********************************************************************/
/*  This is the DOS/UNIX Access Method base class declaration.         */
/***********************************************************************/
class TDBDOX: public TDBDOS {
  friend int   MakeIndex(PGLOBAL, PTDB, PIXDEF);
  friend int   CntCloseTable(PGLOBAL, PTDB, bool, bool);
  friend int   CntIndexInit(PGLOBAL, PTDB, int, bool);
  friend RCODE CntIndexRead(PGLOBAL, PTDB, OPVAL, const key_range*, bool);
  friend RCODE CntDeleteRow(PGLOBAL, PTDB, bool);
  friend int   CntIndexRange(PGLOBAL, PTDB, const uchar**, uint*,
                             bool*, key_part_map*);
  friend class ha_connect;
  }; // end of class TDBDOX

class XKPDEF: public KPARTDEF {
  friend class TDBDOX;
  friend class ha_connect;
  friend int CntIndexInit(PGLOBAL, PTDB, int, bool);
 public:
  XKPDEF(const char *name, int n) : KPARTDEF((PSZ)name, n) {}
  }; // end of class XKPDEF
