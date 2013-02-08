/******************************************************************/
/*  Declaration of XML document processing using libxml2          */
/*  Author: Olivier Bertrand                2007-2012             */
/******************************************************************/
#include "plgxml.h"

typedef class LIBXMLDOC    *PXDOC2;
typedef class XML2NODE     *PNODE2;
typedef class XML2ATTR     *PATTR2;
typedef class XML2NODELIST *PLIST2;

/******************************************************************/
/* XML2 block. Must have the same layout than FBLOCK up to Type.  */
/******************************************************************/
typedef struct _x2block {         /* Loaded XML file block        */
  struct _x2block   *Next;
  LPCSTR             Fname;       /* Point on file name           */
  size_t             Length;      /* Used to tell if read mode    */
  short              Count;       /* Nb of times file is used     */
  short              Type;        /* TYPE_FB_XML                  */
  int                Retcode;     /* Return code from Load        */
  xmlDocPtr          Docp;        /* Document interface pointer   */
//  xmlXPathContextPtr Ctxp;
//  xmlXPathObjectPtr  Xop;
  } X2BLOCK, *PX2BLOCK;

/******************************************************************/
/*  Declaration of libxml2 document.                              */
/******************************************************************/
class LIBXMLDOC : public XMLDOCUMENT {
  friend class XML2NODE;
  friend class XML2ATTR;
 public:
  // Constructor
  LIBXMLDOC(char *nsl, char *nsdf, char *enc, PFBLOCK fp);

  // Properties
  virtual short  GetDocType(void) {return TYPE_FB_XML2;}
  virtual void  *GetDocPtr(void) {return Docp;}

  // Methods
  virtual bool    Initialize(PGLOBAL g);
  virtual bool    ParseFile(char *fn);
  virtual bool    NewDoc(PGLOBAL g, char *ver);
  virtual void    AddComment(PGLOBAL g, char *com);
  virtual PXNODE  GetRoot(PGLOBAL g);
  virtual PXNODE  NewRoot(PGLOBAL g, char *name);
  virtual PXNODE  NewPnode(PGLOBAL g, char *name);
  virtual PXATTR  NewPattr(PGLOBAL g);
  virtual PXLIST  NewPlist(PGLOBAL g);
  virtual int     DumpDoc(PGLOBAL g, char *ofn);
  virtual void    CloseDoc(PGLOBAL g, PFBLOCK xp);
  virtual PFBLOCK LinkXblock(PGLOBAL g, MODE m, int rc, char *fn);

 protected:
          bool     CheckDocument(FILE *of, xmlNodePtr np);
          xmlNodeSetPtr GetNodeList(PGLOBAL g, xmlNodePtr np, char *xp);
          int      Decode(xmlChar *cnt, char *buf, int n);
          xmlChar *Encode(PGLOBAL g, char *txt);

  // Members
  xmlDocPtr          Docp;
  xmlNodeSetPtr      Nlist;
  xmlXPathContextPtr Ctxp;
  xmlXPathObjectPtr  Xop;
  char              *Buf;                  // Temporary
}; // end of class LIBXMLDOC

/******************************************************************/
/*  Declaration of libxml2 node.                                  */
/******************************************************************/
class XML2NODE : public XMLNODE {
  friend class LIBXMLDOC;
  friend class XML2NODELIST;
 public:
  // Properties
  virtual char  *GetName(PGLOBAL g) {return (char*)Nodep->name;}
  virtual int    GetType(void);
  virtual PXNODE GetNext(PGLOBAL g);
  virtual PXNODE GetChild(PGLOBAL g);

  // Methods
  virtual char  *GetText(char *buf, int len);
  virtual bool   SetContent(PGLOBAL g, char *txtp, int len);
  virtual PXNODE Clone(PGLOBAL g, PXNODE np);
  virtual PXLIST GetChildElements(PGLOBAL g, char *xp, PXLIST lp);
  virtual PXLIST SelectNodes(PGLOBAL g, char *xp, PXLIST lp);
  virtual PXNODE SelectSingleNode(PGLOBAL g, char *xp, PXNODE np);
  virtual PXATTR GetAttribute(PGLOBAL g, char *name, PXATTR ap);
  virtual PXNODE AddChildNode(PGLOBAL g, char *name, PXNODE np);
  virtual PXATTR AddProperty(PGLOBAL g, char *name, PXATTR ap);
  virtual void   AddText(PGLOBAL g, char *txtp);
  virtual void   DeleteChild(PGLOBAL g, PXNODE dnp);

 protected:
  // Constructor
  XML2NODE(PXDOC dp, xmlNodePtr np);

  // Members
  xmlDocPtr  Docp;
  xmlChar   *Content;
  xmlNodePtr Nodep;
}; // end of class XML2NODE

/******************************************************************/
/*  Declaration of libxml2 node list.                             */
/******************************************************************/
class XML2NODELIST : public XMLNODELIST {
  friend class LIBXMLDOC;
  friend class XML2NODE;
 public:
  // Methods
  virtual int    GetLength(void);
  virtual PXNODE GetItem(PGLOBAL g, int n, PXNODE np);

 protected:
  // Constructor
  XML2NODELIST(PXDOC dp, xmlNodeSetPtr lp);

  // Members
  xmlNodeSetPtr Listp;
}; // end of class XML2NODELIST

/******************************************************************/
/*  Declaration of libxml2 attribute.                             */
/******************************************************************/
class XML2ATTR : public XMLATTRIBUTE {
  friend class LIBXMLDOC;
  friend class XML2NODE;
 public:
  // Properties
//virtual char *GetText(void);

  // Methods
  virtual bool  SetText(PGLOBAL g, char *txtp, int len);

 protected:
  // Constructor
  XML2ATTR(PXDOC dp, xmlAttrPtr ap, xmlNodePtr np);

  // Members
  xmlAttrPtr Atrp;
  xmlNodePtr Parent;
}; // end of class XML2ATTR
