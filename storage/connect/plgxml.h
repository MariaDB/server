#if defined(ZIP_SUPPORT)
#include "filamzip.h"
#endif   //	ZIP_SUPPORT

/******************************************************************/
/*  Dual XML implementation base classes defines.                 */
/******************************************************************/
#if !defined(BASE_BUFFER_SIZE)
enum ElementType {               // libxml2
     XML_ELEMENT_NODE       =  1,
     XML_ATTRIBUTE_NODE     =  2,
     XML_TEXT_NODE          =  3,
     XML_CDATA_SECTION_NODE =  4,
     XML_ENTITY_REF_NODE    =  5,
     XML_ENTITY_NODE        =  6,
     XML_PI_NODE            =  7,
     XML_COMMENT_NODE       =  8,
     XML_DOCUMENT_NODE      =  9,
     XML_DOCUMENT_TYPE_NODE = 10,
     XML_DOCUMENT_FRAG_NODE = 11,
     XML_NOTATION_NODE      = 12,
     XML_HTML_DOCUMENT_NODE = 13,
     XML_DTD_NODE           = 14,
     XML_ELEMENT_DECL       = 15,
     XML_ATTRIBUTE_DECL     = 16,
     XML_ENTITY_DECL        = 17,
     XML_NAMESPACE_DECL     = 18,
     XML_XINCLUDE_START     = 19,
     XML_XINCLUDE_END       = 20,
     XML_DOCB_DOCUMENT_NODE = 21};
#endif   // !BASE_BUFFER_SIZE

//#if !defined(NODE_TYPE_LIST)
#ifdef NOT_USED
enum NodeType {                   // MS DOM
     NODE_ELEMENT                 =  1,
     NODE_ATTRIBUTE               =  2,
     NODE_TEXT                    =  3,
     NODE_CDATA_SECTION           =  4,
     NODE_ENTITY_REFERENCE        =  5,
     NODE_ENTITY                  =  6,
     NODE_PROCESSING_INSTRUCTION  =  7,
     NODE_COMMENT                 =  8,
     NODE_DOCUMENT                =  9,
     NODE_DOCUMENT_TYPE           = 10,
     NODE_DOCUMENT_FRAGMENT       = 11,
     NODE_NOTATION                = 12};
#endif   // !NODE_TYPE_LIST

typedef class XMLDOCUMENT  *PXDOC;         // Document
typedef class XMLNODE      *PXNODE;        // Node (Element)
typedef class XMLNODELIST  *PXLIST;        // Node list
typedef class XMLATTRIBUTE *PXATTR;        // Attribute

typedef struct _ns {
  struct _ns *Next;
  char       *Prefix;
  char       *Uri;
  } NS, *PNS;

PXDOC GetLibxmlDoc(PGLOBAL g, char *nsl, char *nsdf, 
                              char *enc, PFBLOCK fp = NULL);
PXDOC GetDomDoc(PGLOBAL g, char *nsl, char *nsdf,
                           char *enc, PFBLOCK fp = NULL);

/******************************************************************/
/*  Declaration of XML document.                                  */
/******************************************************************/
class XMLDOCUMENT : public BLOCK {
  friend class XML2NODE;
  friend class DOMNODE;
 public:
  // Properties
  virtual short   GetDocType(void) = 0;
  virtual void   *GetDocPtr(void) = 0;
  virtual void    SetNofree(bool b) = 0;

  // Methods
	virtual bool    Initialize(PGLOBAL, PCSZ, bool) = 0;
  virtual bool    ParseFile(PGLOBAL, char *) = 0;
  virtual bool    NewDoc(PGLOBAL, PCSZ) = 0;
  virtual void    AddComment(PGLOBAL, char *) = 0;
  virtual PXNODE  GetRoot(PGLOBAL) = 0;
  virtual PXNODE  NewRoot(PGLOBAL, char *) = 0;
  virtual PXNODE  NewPnode(PGLOBAL, char * = NULL) = 0;
  virtual PXATTR  NewPattr(PGLOBAL) = 0;
  virtual PXLIST  NewPlist(PGLOBAL) = 0;
  virtual int     DumpDoc(PGLOBAL, char *) = 0;
  virtual void    CloseDoc(PGLOBAL, PFBLOCK) = 0;
  virtual PFBLOCK LinkXblock(PGLOBAL, MODE, int, char *) = 0;

 protected:
  // Constructor
  XMLDOCUMENT(char *nsl, char *nsdf, char *enc);

  // Utility
  bool  MakeNSlist(PGLOBAL g);
	bool  InitZip(PGLOBAL g, PCSZ entry);
	char *GetMemDoc(PGLOBAL g, char *fn);
	void  CloseZip(void);

  // Members
#if defined(ZIP_SUPPORT)
	UNZIPUTL *zip;												 /* Used for zipped file  */
#else   // !ZIP_SUPPORT
	bool     zip;													 /* Always false          */
#endif  //	!ZIP_SUPPORT
  PNS   Namespaces;                      /* To the namespaces     */
  char *Encoding;                        /* The document encoding */
  char *Nslist;                          /* Namespace list        */
  char *DefNs;                           /* Default namespace     */
}; // end of class XMLDOCUMENT

/******************************************************************/
/*  Declaration of XML node.                                      */
/******************************************************************/
class XMLNODE : public BLOCK {
 public:
  // Properties
  virtual char  *GetName(PGLOBAL) = 0;
  virtual int    GetType(void) = 0;
  virtual PXNODE GetNext(PGLOBAL) = 0;
  virtual PXNODE GetChild(PGLOBAL) = 0;
  virtual int    GetLen(void) {return Len;}

  // Methods
  virtual RCODE  GetContent(PGLOBAL, char *, int) = 0;
  virtual bool   SetContent(PGLOBAL, char *, int) = 0;
  virtual PXNODE Clone(PGLOBAL, PXNODE) = 0;
  virtual PXLIST GetChildElements(PGLOBAL, char * = NULL, PXLIST = NULL) = 0;
  virtual PXLIST SelectNodes(PGLOBAL, char *, PXLIST = NULL) = 0;
  virtual PXNODE SelectSingleNode(PGLOBAL, char *, PXNODE = NULL) = 0;
  virtual PXATTR GetAttribute(PGLOBAL, char *, PXATTR = NULL) = 0;
  virtual PXNODE AddChildNode(PGLOBAL, PCSZ, PXNODE = NULL) = 0;
  virtual PXATTR AddProperty(PGLOBAL, char *, PXATTR = NULL) = 0;
  virtual void   AddText(PGLOBAL, PCSZ) = 0;
  virtual void   DeleteChild(PGLOBAL, PXNODE) = 0;

 protected:
          PXNODE NewChild(PXNODE ncp);
          void   Delete(PXNODE dnp);
          char  *BufAlloc(PGLOBAL g, const char *p, int n);

  // Constructor
  XMLNODE(PXDOC dp);

  // Members
  PXDOC  Doc;
  PXNODE Next;
  PXNODE Children;
  char  *Buf;
  int    Len; 
}; // end of class XMLNODE

/******************************************************************/
/*  Declaration of XML node list.                                 */
/******************************************************************/
class XMLNODELIST : public BLOCK {
 public:
  // Properties
  virtual int    GetLength(void) = 0;
  virtual PXNODE GetItem(PGLOBAL, int, PXNODE = NULL) = 0;
  virtual bool   DropItem(PGLOBAL, int) = 0;

 protected:
  // Constructor
  XMLNODELIST(PXDOC dp) {Doc = dp;}

  // Members
  PXDOC Doc;
}; // end of class XMLNODELIST

/******************************************************************/
/*  Declaration of XML attribute.                                 */
/******************************************************************/
class XMLATTRIBUTE : public BLOCK {
 public:
  // Properties
  virtual char  *GetName(PGLOBAL) = 0;
  virtual PXATTR GetNext(PGLOBAL) = 0;

  // Methods
  virtual RCODE  GetText(PGLOBAL, char *, int) = 0;
  virtual bool   SetText(PGLOBAL, char *, int) = 0;

 protected:
  // Constructor
  XMLATTRIBUTE(PXDOC dp) {Doc = dp;}

  // Members
  PXDOC Doc;
}; // end of class XMLATTRIBUTE


