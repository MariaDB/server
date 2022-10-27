/******************************************************************/
/*  Declaration of XML document processing using MS DOM           */
/*  Author: Olivier Bertrand                2007 - 2012           */
/******************************************************************/
#include "plgxml.h"

typedef class DOMDOC      *PDOMDOC;
typedef class DOMNODE     *PDOMNODE;
typedef class DOMATTR     *PDOMATTR;
typedef class DOMNODELIST *PDOMLIST;

/******************************************************************/
/*  XML block. Must have the same layout than FBLOCK up to Type.  */
/******************************************************************/
typedef struct _xblock {          /* Loaded XML file block        */
  struct _xblock    *Next;
  LPCSTR             Fname;       /* Point on file name           */
  size_t             Length;      /* Used to tell if read mode    */
  short              Count;       /* Nb of times file is used     */
  short              Type;        /* TYPE_FB_XML                  */
  int                Retcode;     /* Return code from Load        */
  MSXML2::IXMLDOMDocumentPtr Docp;/* Document interface pointer   */
  } XBLOCK, *PXBLOCK;

/******************************************************************/
/*  Declaration of DOM document.                                  */
/******************************************************************/
class DOMDOC : public XMLDOCUMENT {
  friend class DOMNODE;
 public:
  // Constructor
  DOMDOC(char *nsl, char *nsdf, char *enc, PFBLOCK fp);

  // Properties
  virtual short  GetDocType(void) {return TYPE_FB_XML;}
  virtual void  *GetDocPtr(void) {return Docp;}
  virtual void   SetNofree(bool b) {}   // Only libxml2

  // Methods
	virtual bool    Initialize(PGLOBAL g, PCSZ entry, bool zipped);
  virtual bool    ParseFile(PGLOBAL g, char *fn);
  virtual bool    NewDoc(PGLOBAL g, PCSZ ver);
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
  // Members
  MSXML2::IXMLDOMDocumentPtr Docp;
  MSXML2::IXMLDOMNodeListPtr Nlist;
  HRESULT            Hr;
}; // end of class DOMDOC

/******************************************************************/
/*  Declaration of DOM XML node.                                  */
/******************************************************************/
class DOMNODE : public XMLNODE {
  friend class DOMDOC;
  friend class DOMNODELIST;
 public:
  // Properties
  virtual char  *GetName(PGLOBAL g);
  virtual int    GetType(void) {return Nodep->nodeType;}
  virtual PXNODE GetNext(PGLOBAL g);
  virtual PXNODE GetChild(PGLOBAL g);

  // Methods
  virtual RCODE  GetContent(PGLOBAL g, char *buf, int len);
  virtual bool   SetContent(PGLOBAL g, char *txtp, int len);
  virtual PXNODE Clone(PGLOBAL g, PXNODE np);
  virtual PXLIST GetChildElements(PGLOBAL g, char *xp, PXLIST lp);
  virtual PXLIST SelectNodes(PGLOBAL g, char *xp, PXLIST lp);
  virtual PXNODE SelectSingleNode(PGLOBAL g, char *xp, PXNODE np);
  virtual PXATTR GetAttribute(PGLOBAL g, char *name, PXATTR ap);
  virtual PXNODE AddChildNode(PGLOBAL g, PCSZ name, PXNODE np);
  virtual PXATTR AddProperty(PGLOBAL g, char *name, PXATTR ap);
  virtual void   AddText(PGLOBAL g, PCSZ txtp);
  virtual void   DeleteChild(PGLOBAL g, PXNODE dnp);

 protected:
  // Constructor
  DOMNODE(PXDOC dp, MSXML2::IXMLDOMNodePtr np);

  // Members
  MSXML2::IXMLDOMDocumentPtr Docp;
  MSXML2::IXMLDOMNodePtr     Nodep;
  char               Name[64];
  WCHAR             *Ws;
  int                Len;
	bool               Zip;
}; // end of class DOMNODE

/******************************************************************/
/*  Declaration of DOM XML node list.                             */
/******************************************************************/
class DOMNODELIST : public XMLNODELIST {
  friend class DOMDOC;
  friend class DOMNODE;
 public:
  // Methods
  virtual int    GetLength(void) {return Listp->length;}
  virtual PXNODE GetItem(PGLOBAL g, int n, PXNODE np);
  virtual bool   DropItem(PGLOBAL g, int n);

 protected:
  // Constructor
  DOMNODELIST(PXDOC dp, MSXML2::IXMLDOMNodeListPtr lp);

  // Members
  MSXML2::IXMLDOMNodeListPtr Listp;
}; // end of class DOMNODELIST

/******************************************************************/
/*  Declaration of DOM XML attribute.                             */
/******************************************************************/
class DOMATTR : public XMLATTRIBUTE {
  friend class DOMDOC;
  friend class DOMNODE;
 public:
  // Properties
  virtual char  *GetName(PGLOBAL g);
  virtual PXATTR GetNext(PGLOBAL);

  // Methods
  virtual RCODE  GetText(PGLOBAL g, char *bufp, int len);
  virtual bool   SetText(PGLOBAL g, char *txtp, int len);

 protected:
  // Constructor
  DOMATTR(PXDOC dp, MSXML2::IXMLDOMAttributePtr ap,
                    MSXML2::IXMLDOMNamedNodeMapPtr nmp = NULL);

  // Members
  MSXML2::IXMLDOMAttributePtr    Atrp;
  MSXML2::IXMLDOMNamedNodeMapPtr Nmp;
  char               Name[64];
  WCHAR             *Ws;
  int                Len;
  long               K;
}; // end of class DOMATTR
