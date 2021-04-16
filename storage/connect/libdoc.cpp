/******************************************************************/
/*  Implementation of XML document processing using libxml2       */
/*  Author: Olivier Bertrand                2007-2016             */
/******************************************************************/
#include "my_global.h"
#include <string.h>
#include <stdio.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/catalog.h>
#include <libxml/xmlschemastypes.h>
#include <libxml/relaxng.h>

#if !defined(LIBXML_TREE_ENABLED) || !defined(LIBXML_OUTPUT_ENABLED)
#error "tree support not compiled in"
#endif

#if !defined(LIBXML_XPATH_ENABLED) || !defined(LIBXML_SAX1_ENABLED)
#error "XPath not supported"
#endif

#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"
#include "libdoc.h"

#include "sql_string.h"

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
  virtual void   SetNofree(bool b) {Nofreelist = b;}

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
//        bool     CheckDocument(FILE *of, xmlNodePtr np);
          xmlNodeSetPtr GetNodeList(PGLOBAL g, xmlNodePtr np, char *xp);
          int      Decode(xmlChar *cnt, char *buf, int n);
          xmlChar *Encode(PGLOBAL g, char *txt);

  // Members
  xmlDocPtr          Docp;
  xmlNodeSetPtr      Nlist;
  xmlXPathContextPtr Ctxp;
  xmlXPathObjectPtr  Xop;
  xmlXPathObjectPtr  NlXop;
  xmlErrorPtr        Xerr;
  char              *Buf;                  // Temporary
  bool               Nofreelist;
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
  virtual bool   DropItem(PGLOBAL g, int n);

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
  virtual char  *GetName(PGLOBAL g) {return (char*)Atrp->name;}
  virtual PXATTR GetNext(PGLOBAL g);

  // Methods
  virtual RCODE  GetText(PGLOBAL g, char *bufp, int len);
  virtual bool   SetText(PGLOBAL g, char *txtp, int len);

 protected:
  // Constructor
  XML2ATTR(PXDOC dp, xmlAttrPtr ap, xmlNodePtr np);

  // Members
  xmlAttrPtr Atrp;
  xmlNodePtr Parent;
}; // end of class XML2ATTR



extern "C" {
extern char version[];
} //   "C"

#if defined(MEMORY_TRACE)
static int  m = 0;
static char s[500];
/**************************************************************************/
/*  Tracing output function.                                              */
/**************************************************************************/
void xtrc(char const *fmt, ...)
  {
  va_list ap;
  va_start (ap, fmt);
																							;
//vfprintf(stderr, fmt, ap);
  vsprintf(s, fmt, ap);
  if (s[strlen(s)-1] == '\n')
      s[strlen(s)-1] = 0;
  va_end (ap);
  } // end of htrc

static xmlFreeFunc Free; 
static xmlMallocFunc Malloc;
static xmlMallocFunc MallocA; 
static xmlReallocFunc Realloc; 
static xmlStrdupFunc Strdup;

void xmlMyFree(void *mem)
{
  if (trace(1)) { 
    htrc("%.4d Freeing          at %p   %-.256s\n", ++m, mem, s);
    *s = 0;
    } // endif trace
  Free(mem);
} // end of xmlMyFree

void *xmlMyMalloc(size_t size)
{
  void *p = Malloc(size);
  if (trace(1)) { 
    htrc("%.4d Allocating %.5d at %p   %-.256s\n", ++m, size, p, s);
    *s = 0;
    } // endif trace
  return p;
} // end of xmlMyMalloc

void *xmlMyMallocAtomic(size_t size)
{
  void *p = MallocA(size);
  if (trace(1)) { 
    htrc("%.4d Atom alloc %.5d at %p   %-.256s\n", ++m, size, p, s);
    *s = 0;
    } // endif trace
  return p;
} // end of xmlMyMallocAtomic

void *xmlMyRealloc(void *mem, size_t size)
{
  void *p = Realloc(mem, size);
  if (trace(1)) { 
    htrc("%.4d ReAlloc    %.5d to %p from %p   %-.256s\n", ++m, size, p, mem, s);
    *s = 0;
    } // endif trace
  return p;
} // end of xmlMyRealloc

char *xmlMyStrdup(const char *str)
{
  char *p = Strdup(str);
  if (trace(1)) { 
    htrc("%.4d Duplicating      to %p from %p %-.256s   %-.256s\n", ++m, p, str, str, s);
    *s = 0;
    } // endif trace
  return p;
} // end of xmlMyStrdup
#define htrc xtrc
#endif   // MEMORY_TRACE

/******************************************************************/
/*  Return a LIBXMLDOC as a XMLDOC.                               */
/******************************************************************/
PXDOC GetLibxmlDoc(PGLOBAL g, char *nsl, char *nsdf, 
                                         char *enc, PFBLOCK fp)
  {
  return (PXDOC) new(g) LIBXMLDOC(nsl, nsdf, enc, fp);
  } // end of GetLibxmlDoc

/******************************************************************/
/*  XML library initialization function.                          */
/******************************************************************/
void XmlInitParserLib(void)
  {
#if defined(MEMORY_TRACE)
int	rc = xmlGcMemGet(&Free, &Malloc, &MallocA, &Realloc, &Strdup);

if (!rc)
  rc = xmlGcMemSetup(xmlMyFree, 
                     xmlMyMalloc, 
                     xmlMyMallocAtomic,
                     xmlMyRealloc,
                     xmlMyStrdup); 

#endif   // MEMORY_TRACE
  xmlInitParser();
  } // end of XmlInitParserLib

/******************************************************************/
/*  XML library cleanup function.                                 */
/******************************************************************/
/*
  This is a copy of xmlCleanupParser() from the libxml2 sources
  with xmlResetLastError() commented.

  xmlResetLastError() called from the original xmlCleanupParser() causes
  valgrind to report memory leaks. This happens because
  ha_initialize_handlerton() is called from the main thread in mysqld.cc,
  while ha_finalize_handlerton() is called from a non-main thread.
  libxml2 gets confused because of xmlInitParser() and xmlCleanupParser()
  being called from the different threads.

  Perhaps the code in mysqld.cc should eventually be modified
  to shutdown plugins from the main thread.
*/
static void
xmlCleanupParser_replacement(void)
  {
  xmlCleanupCharEncodingHandlers();
#ifdef LIBXML_CATALOG_ENABLED
  xmlCatalogCleanup();
#endif
  xmlDictCleanup();
  xmlCleanupInputCallbacks();
#ifdef LIBXML_OUTPUT_ENABLED   
  xmlCleanupOutputCallbacks();
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
  xmlSchemaCleanupTypes(); 
  xmlRelaxNGCleanupTypes();
#endif
  //xmlResetLastError();
  xmlCleanupGlobals();
  xmlCleanupThreads(); /* must be last if called not from the main thread */
  xmlCleanupMemory();
  }


void XmlCleanupParserLib(void)
  {
  xmlCleanupParser_replacement();
  } // end of XmlCleanupParserLib

/******************************************************************/
/*  Close a loaded libxml2 XML file.                              */
/******************************************************************/
void CloseXML2File(PGLOBAL g, PFBLOCK fp, bool all)
  {
  PX2BLOCK xp = (PX2BLOCK)fp;

  if (trace(1))
    htrc("CloseXML2File: xp=%p count=%d\n", xp, (xp) ? xp->Count : 0);

  if (xp && xp->Count > 1 && !all) {
    xp->Count--;
  } else if (xp && xp->Count > 0) {
    xmlFreeDoc(xp->Docp);
    xp->Count = 0;
  } // endif

  } // end of CloseXML2File

/* ---------------------- class LIBXMLDOC ----------------------- */

/******************************************************************/
/*  LIBXMLDOC constructor.                                        */
/******************************************************************/
LIBXMLDOC::LIBXMLDOC(char *nsl, char *nsdf, char *enc, PFBLOCK fp)
         : XMLDOCUMENT(nsl, nsdf, enc)
  {
  assert (!fp || fp->Type == TYPE_FB_XML2);
  Docp = (fp) ? ((PX2BLOCK)fp)->Docp : NULL;
  Nlist = NULL;
  Ctxp = NULL;
  Xop = NULL;
  NlXop = NULL;
  Xerr = NULL;
  Buf = NULL;
  Nofreelist = false;
  } // end of LIBXMLDOC constructor

/******************************************************************/
/*  Initialize XML parser and check library compatibility.        */
/******************************************************************/
bool LIBXMLDOC::Initialize(PGLOBAL g, PCSZ entry, bool zipped)
{
	if (zipped && InitZip(g, entry))
		return true;

  xmlKeepBlanksDefault(1);
  return MakeNSlist(g);
} // end of Initialize

/******************************************************************/
/* Parse the XML file and construct node tree in memory.          */
/******************************************************************/
bool LIBXMLDOC::ParseFile(PGLOBAL g, char *fn)
  {
  if (trace(1))
    htrc("ParseFile\n");

	if (zip) {
		// Parse an in memory document
		char *xdoc = GetMemDoc(g, fn);

		Docp = (xdoc) ? xmlParseDoc((const xmlChar *)xdoc) : NULL;
	} else
		Docp = xmlParseFile(fn);

	if (Docp) {
		if (Docp->encoding)
      Encoding = (char*)Docp->encoding;

    return false;
  } else if ((Xerr = xmlGetLastError()))
    xmlResetError(Xerr);

  return true;
  } // end of ParseFile

/******************************************************************/
/* Create or reuse an Xblock for this document.                   */
/******************************************************************/
PFBLOCK LIBXMLDOC::LinkXblock(PGLOBAL g, MODE m, int rc, char *fn)
  {
  PDBUSER  dup = (PDBUSER)g->Activityp->Aptr;
  PX2BLOCK xp = (PX2BLOCK)PlugSubAlloc(g, NULL, sizeof(X2BLOCK));

  memset(xp, 0, sizeof(X2BLOCK));
  xp->Next = (PX2BLOCK)dup->Openlist;
  dup->Openlist = (PFBLOCK)xp;
  xp->Type = TYPE_FB_XML2;
  xp->Fname = (LPCSTR)PlugDup(g, fn);
  xp->Count = 1;
  xp->Length = (m == MODE_READ) ? 1 : 0;
  xp->Retcode = rc;
  xp->Docp = Docp;

  // Return xp as a fp
  return (PFBLOCK)xp;
  } // end of LinkXblock

/******************************************************************/
/* Construct and add the XML processing instruction node.         */
/******************************************************************/
bool LIBXMLDOC::NewDoc(PGLOBAL g, PCSZ ver)
  {
  if (trace(1))
    htrc("NewDoc\n");

  return ((Docp = xmlNewDoc(BAD_CAST ver)) == NULL);
  } // end of NewDoc

/******************************************************************/
/*  Add a new comment node to the document.                       */
/******************************************************************/
void LIBXMLDOC::AddComment(PGLOBAL g, char *txtp)
  {
  if (trace(1))
    htrc("AddComment: %-.256s\n", txtp);

  xmlNodePtr cp = xmlNewDocComment(Docp, BAD_CAST txtp);
  xmlAddChild((xmlNodePtr)Docp, cp);
  } // end of AddText

/******************************************************************/
/* Return the node class of the root of the document.             */
/******************************************************************/
PXNODE LIBXMLDOC::GetRoot(PGLOBAL g)
  {
  if (trace(1))
    htrc("GetRoot\n");

  xmlNodePtr root = xmlDocGetRootElement(Docp);

  if (!root)
    return NULL;

  return new(g) XML2NODE(this, root);
  } // end of GetRoot

/******************************************************************/
/* Create a new root element and return its class node.           */
/******************************************************************/
PXNODE LIBXMLDOC::NewRoot(PGLOBAL g, char *name)
  {
  if (trace(1))
    htrc("NewRoot: %-.256s\n", name);

  xmlNodePtr root = xmlNewDocNode(Docp, NULL, BAD_CAST name, NULL);

  if (root) {
    xmlDocSetRootElement(Docp, root);
    return new(g) XML2NODE(this, root);
  } else
    return NULL;

  } // end of NewRoot

/******************************************************************/
/* Return a void XML2NODE node class.                             */
/******************************************************************/
PXNODE LIBXMLDOC::NewPnode(PGLOBAL g, char *name)
  {
  if (trace(1))
    htrc("NewNode: %-.256s\n", name);

  xmlNodePtr nop;

  if (name) {
    nop = xmlNewDocNode(Docp, NULL, BAD_CAST name, NULL);

    if (nop == NULL)
      return NULL;

  } else
    nop = NULL;

  return new(g) XML2NODE(this, nop);
  } // end of NewPnode

/******************************************************************/
/* Return a void XML2ATTR node class.                             */
/******************************************************************/
PXATTR LIBXMLDOC::NewPattr(PGLOBAL g)
  {
  return new(g) XML2ATTR(this, NULL, NULL);
  } // end of NewPattr

/******************************************************************/
/* Return a void XML2ATTR node class.                             */
/******************************************************************/
PXLIST LIBXMLDOC::NewPlist(PGLOBAL g)
  {
  return new(g) XML2NODELIST(this, NULL);
  } // end of NewPlist

/******************************************************************/
/* Dump the node tree to a new XML file.                          */
/******************************************************************/
int LIBXMLDOC::DumpDoc(PGLOBAL g, char *ofn)
  {
  int   rc = 0;
  FILE *of;

  if (trace(1))
    htrc("DumpDoc: %-.256s\n", ofn);

  if (!(of= global_fopen(g, MSGID_CANNOT_OPEN, ofn, "w")))
    return -1;

#if 1
  // This function does not crash (
  if (xmlSaveFormatFileEnc((const char *)ofn, Docp, Encoding, 0) < 0) {
    xmlErrorPtr err = xmlGetLastError();
    strcpy(g->Message, (err) ? err->message : "Error saving XML doc");
    xmlResetError(Xerr);
    rc = -1;
    } // endif Save
//  rc = xmlDocDump(of, Docp);
#else // 0
  // Until this function is fixed, do the job ourself
  xmlNodePtr Rootp;

  // Save the modified document
  fprintf(of, "<?xml version=\"1.0\" encoding=\"%-.256s\"?>\n", Encoding);
  fprintf(of, "<!-- Created by CONNECT %-.256s -->\n", version);

  if (!(Rootp = xmlDocGetRootElement(Docp)))
    return 1;

  Buf = (char*)PlugSubAlloc(g, NULL, 1024);
  rc = iconv_close(Cd2);
  Cd2 = iconv_open(Encoding, "UTF-8");
  rc = CheckDocument(of, Rootp);
#endif // 0

  fclose(of);
  return rc;
  } // end of Dump

/******************************************************************/
/* Free the document, cleanup the XML library, and                */
/* debug memory for regression tests.                             */
/******************************************************************/
void LIBXMLDOC::CloseDoc(PGLOBAL g, PFBLOCK xp)
  {
  if (trace(1))
    htrc("CloseDoc: xp=%p count=%d\n", xp, (xp) ? xp->Count : 0);

//if (xp && xp->Count == 1) {
  if (xp) {
    if (Nlist) {
      xmlXPathFreeNodeSet(Nlist);

      if ((Xerr = xmlGetLastError()))
        xmlResetError(Xerr);

      Nlist = NULL;
      } // endif Nlist

    if (Xop) {
      xmlXPathFreeObject(Xop);

      if ((Xerr = xmlGetLastError()))
        xmlResetError(Xerr);

      Xop = NULL;
      } // endif Xop

    if (NlXop) {
      xmlXPathFreeObject(NlXop);

      if ((Xerr = xmlGetLastError()))
        xmlResetError(Xerr);

      NlXop = NULL;
      } // endif NlXop

    if (Ctxp) {
      xmlXPathFreeContext(Ctxp);

      if ((Xerr = xmlGetLastError()))
        xmlResetError(Xerr);

      Ctxp = NULL;
      } // endif Ctxp

    } // endif xp

  CloseXML2File(g, xp, false);
	CloseZip();
  } // end of Close

/******************************************************************/
/* Evaluate the passed Xpath from the passed context node.        */
/******************************************************************/
xmlNodeSetPtr LIBXMLDOC::GetNodeList(PGLOBAL g, xmlNodePtr np, char *xp)
  {
  xmlNodeSetPtr nl;

  if (trace(1))
    htrc("GetNodeList: %-.256s np=%p\n", xp, np);

  if (!Ctxp) {
    // Init Xpath
    if (trace(1))
      htrc("Calling xmlPathInit\n");

    xmlXPathInit();

    if (trace(1))
      htrc("Calling xmlXPathNewContext Docp=%p\n", Docp);

    // Create xpath evaluation context
    if (!(Ctxp = xmlXPathNewContext(Docp))) {
      strcpy(g->Message, MSG(XPATH_CNTX_ERR));

      if (trace(1))
        htrc("Context error: %-.256s\n", g->Message);

      return NULL;
      } // endif xpathCtx

    // Register namespaces from list (if any)
    for (PNS nsp = Namespaces; nsp; nsp = nsp->Next) {
      if (trace(1))
        htrc("Calling xmlXPathRegisterNs Prefix=%-.256s Uri=%-.512s\n", 
             nsp->Prefix, nsp->Uri);

      if (xmlXPathRegisterNs(Ctxp, BAD_CAST nsp->Prefix,
                                   BAD_CAST nsp->Uri)) {
        sprintf(g->Message, MSG(REGISTER_ERR), nsp->Prefix, nsp->Uri);

        if (trace(1))
          htrc("Ns error: %-.256s\n", g->Message);

        return NULL;
        } // endif Registering

      } // endfor nsp

    } // endif Ctxp

  if (Xop) {
    if (trace(1))
      htrc("Calling xmlXPathFreeNodeSetList Xop=%p NOFREE=%d\n", 
                                            Xop, Nofreelist);

    if (Nofreelist) {
      // Making Nlist that must not be freed yet
//    xmlXPathFreeNodeSetList(Xop);       // Caused memory leak
      assert(!NlXop);
      NlXop = Xop;                        // Freed on closing
      Nofreelist = false;
    } else
      xmlXPathFreeObject(Xop);            // Caused node not found

    if ((Xerr = xmlGetLastError())) {
      strcpy(g->Message, Xerr->message);
      xmlResetError(Xerr);
      return NULL;
      } // endif Xerr

    } // endif Xop

  // Set the context to the calling node
  Ctxp->node = np;

  if (trace(1))
    htrc("Calling xmlXPathEval %-.256s Ctxp=%p\n", xp, Ctxp);

  // Evaluate table xpath
  if (!(Xop = xmlXPathEval(BAD_CAST xp, Ctxp))) {
    sprintf(g->Message, MSG(XPATH_EVAL_ERR), xp);

    if (trace(1))
      htrc("Path error: %-.256s\n", g->Message);

    return NULL;
  } else
    nl = Xop->nodesetval;

  if (trace(1))
    htrc("GetNodeList nl=%p n=%p\n", nl, (nl) ? nl->nodeNr : 0);

  return nl;
  } // end of GetNodeList

#if 0                                // Not used anymore
/******************************************************************/
/*  CheckDocument: check if the document is ok to dump.           */
/*  Currently this does the dumping of the document.              */
/******************************************************************/
bool LIBXMLDOC::CheckDocument(FILE *of, xmlNodePtr np)
  {
  int  n;
  bool b;

  if (!np)
    return true;

  if (np->type == XML_ELEMENT_NODE) {
    n = fprintf(of, "<%s", np->name);
    b = CheckDocument(of, (xmlNodePtr)np->properties);

    if (np->children)
      n = fprintf(of, ">");
    else
      n = fprintf(of, "/>");

  } else if (np->type == XML_ATTRIBUTE_NODE)
    n = fprintf(of, " %s=\"", np->name);
  else if (np->type == XML_TEXT_NODE)
    n = fprintf(of, "%s", Encode(NULL, (char*)np->content));
  else if (np->type == XML_COMMENT_NODE)
    n = fprintf(of, "%s", Encode(NULL, (char*)np->content));

  b = CheckDocument(of, np->children);

  if (np->type == XML_ATTRIBUTE_NODE)
    n = fprintf(of, "\"");
  else if (!b && np->type == XML_ELEMENT_NODE)
    n = fprintf(of, "</%s>", np->name);

  b = CheckDocument(of, np->next);
  return false;
  } // end of CheckDocument

/******************************************************************/
/*  Convert node or attribute content to latin characters.        */
/******************************************************************/
int LIBXMLDOC::Decode(xmlChar *cnt, char *buf, int n)
  {
  const char *txt = (const char *)cnt;
  uint   dummy_errors;
  uint32 len= copy_and_convert(buf, n, &my_charset_utf8mb3_general_ci, txt,
                               strlen(txt), &my_charset_utf8mb3_general_ci,
                               &dummy_errors);
  buf[len]= '\0';
  return 0;
  } // end of Decode

/******************************************************************/
/*  Convert node or attribute content to latin characters.        */
/******************************************************************/
xmlChar *LIBXMLDOC::Encode(PGLOBAL g, char *txt)
  {
  const CHARSET_INFO *ics= &my_charset_utf8mb3_general_ci;
  const CHARSET_INFO *ocs= &my_charset_utf8mb3_general_ci;
  size_t      i = strlen(txt);
  size_t      o = i * ocs->mbmaxlen / ics->mbmaxlen + 1;
  char        *buf;
  if (g) {
    buf = (char*)PlugSubAlloc(g, NULL, o);
  } else {
    o = 1024;
    buf = Buf;
  } // endif g
  uint dummy_errors;
  uint32 len= copy_and_convert(buf, o, ocs,
                               txt, i, ics,
                               &dummy_errors);
  buf[len]= '\0';
  return BAD_CAST buf;
  } // end of Encode
#endif // 0

/* ---------------------- class XML2NODE ------------------------ */

/******************************************************************/
/*  XML2NODE constructor.                                         */
/******************************************************************/
XML2NODE::XML2NODE(PXDOC dp, xmlNodePtr np) : XMLNODE(dp)
  {
  Docp = ((PXDOC2)dp)->Docp;
  Content = NULL;
  Nodep = np;
  } // end of XML2NODE constructor

int XML2NODE::GetType(void)
  {
  if (trace(1))
    htrc("GetType type=%d\n", Nodep->type);

  return Nodep->type;
  }  // end of GetType

/******************************************************************/
/*  Return the node class of next sibling of the node.            */
/******************************************************************/
PXNODE XML2NODE::GetNext(PGLOBAL g)
  {
  if (trace(1))
    htrc("GetNext\n");

  if (!Nodep->next)
    Next = NULL;
  else // if (!Next)
    Next = new(g) XML2NODE(Doc, Nodep->next);

  return Next;
  } // end of GetNext

/******************************************************************/
/*  Return the node class of first children of the node.          */
/******************************************************************/
PXNODE XML2NODE::GetChild(PGLOBAL g)
  {
  if (trace(1))
    htrc("GetChild\n");

  if (!Nodep->children)
    Children = NULL;
  else // if (!Children)
    Children = new(g) XML2NODE(Doc, Nodep->children);

  return Children;
  } // end of GetChild

/******************************************************************/
/*  Return the content of a node and subnodes.                    */
/******************************************************************/
RCODE XML2NODE::GetContent(PGLOBAL g, char *buf, int len)
  {
  RCODE rc = RC_OK;

  if (trace(1))
    htrc("GetContent\n");

  if (Content)
    xmlFree(Content);

  if ((Content = xmlNodeGetContent(Nodep))) {
    char *p1 = (char*)Content, *p2 = buf;
    bool  b = false;

    // Copy content eliminating extra characters
    for (; *p1; p1++)
      if ((p2 - buf) < len) {
        if (strchr(" \t\r\n", *p1)) {
          if (b) {
            // This to have one blank between sub-nodes
            *p2++ = ' ';
            b = false;
            }  // endif b
    
        } else {
          *p2++ = *p1;
          b = true;
        } // endif p1

      } else {
        sprintf(g->Message, "Truncated %-.256s content", Nodep->name);
        rc = RC_INFO;
      } // endif len

    *p2 = 0;

    if (trace(1))
      htrc("GetText buf='%-.256s' len=%d\n", buf, len);

    xmlFree(Content);
    Content = NULL;
  } else
    *buf = '\0';

  if (trace(1))
    htrc("GetContent: %-.256s\n", buf);

  return rc;
  } // end of GetContent

/******************************************************************/
/*  Set the content of a node.                                    */
/******************************************************************/
bool XML2NODE::SetContent(PGLOBAL g, char *txtp, int len)
  {
  if (trace(1))
    htrc("SetContent: %-.256s\n", txtp);

  xmlChar *buf = xmlEncodeEntitiesReentrant(Docp, BAD_CAST txtp);

  if (trace(1))
    htrc("SetContent: %-.256s -> %-.256s\n", txtp, buf);

  xmlNodeSetContent(Nodep, buf);
  xmlFree(buf);
  return false;
  } // end of SetContent

/******************************************************************/
/*  Return a clone of this node.                                  */
/******************************************************************/
PXNODE XML2NODE::Clone(PGLOBAL g, PXNODE np)
  {
  if (trace(1))
    htrc("Clone: np=%p\n", np);

  if (np) {
    ((PNODE2)np)->Nodep = Nodep;
    return np;
  } else
    return new(g) XML2NODE(Doc, Nodep);

  } // end of Clone

/******************************************************************/
/*  Return the list of all or matching children that are elements.*/
/******************************************************************/
PXLIST XML2NODE::GetChildElements(PGLOBAL g, char *xp, PXLIST lp)
  {
  if (trace(1))
    htrc("GetChildElements: %-.256s\n", xp);

  return SelectNodes(g, (xp) ? xp : (char*)"*", lp);
  } // end of GetChildElements

/******************************************************************/
/*  Return the list of nodes verifying the passed Xpath.          */
/******************************************************************/
PXLIST XML2NODE::SelectNodes(PGLOBAL g, char *xp, PXLIST lp)
  {
  if (trace(1))
    htrc("SelectNodes: %-.256s\n", xp);

  xmlNodeSetPtr nl = ((PXDOC2)Doc)->GetNodeList(g, Nodep, xp);

  if (lp) {
    ((PLIST2)lp)->Listp = nl;
    return lp;
  } else
    return new(g) XML2NODELIST(Doc, nl);

  } // end of SelectNodes

/******************************************************************/
/*  Return the first node verifying the passed Xapth.             */
/******************************************************************/
PXNODE XML2NODE::SelectSingleNode(PGLOBAL g, char *xp, PXNODE np)
  {
  if (trace(1))
    htrc("SelectSingleNode: %-.256s\n", xp);

  xmlNodeSetPtr nl = ((PXDOC2)Doc)->GetNodeList(g, Nodep, xp);

  if (nl && nl->nodeNr) {
    if (np) {
      ((PNODE2)np)->Nodep = nl->nodeTab[0];
      return np;
    } else
      return new(g) XML2NODE(Doc, nl->nodeTab[0]);

  } else
    return NULL;

  } // end of SelectSingleNode

/******************************************************************/
/*  Return the node attribute with the specified name.            */
/******************************************************************/
PXATTR XML2NODE::GetAttribute(PGLOBAL g, char *name, PXATTR ap)
  {
  xmlAttrPtr atp;

  if (trace(1))
    htrc("GetAttribute: %-.256s\n", SVP(name));

  if (name)
    atp = xmlHasProp(Nodep, BAD_CAST name);
  else
    atp = Nodep->properties;


  if (atp) {
    if (ap) {
      ((PATTR2)ap)->Atrp = atp;
      ((PATTR2)ap)->Parent = Nodep;
      return ap;
    } else
      return new(g) XML2ATTR(Doc, atp, Nodep);

  } else
    return NULL;

  } // end of GetAttribute

/******************************************************************/
/*  Add a new child node to this node and return it.              */
/******************************************************************/
PXNODE XML2NODE::AddChildNode(PGLOBAL g, PCSZ name, PXNODE np)
  {
  char *p, *pn, *pf = NULL, *nmp = PlugDup(g, name);

  if (trace(1))
    htrc("AddChildNode: %-.256s\n", name);

  // Is a prefix specified
  if ((pn = strchr(nmp, ':'))) {
    pf = nmp;
    *pn++ = '\0';                    // Separate name from prefix
  } else
    pn = nmp;

  // If name has the format m[n] only m is taken as node name
  if ((p = strchr(pn, '[')))
    p = BufAlloc(g, pn, int(p - pn));
  else
    p = pn;

  xmlNodePtr nop = xmlNewChild(Nodep, NULL, BAD_CAST p, NULL);

  if (!nop)
    return NULL;

  if (pf) {
    // Prefixed name, is it the default NS prefix?
    if (Doc->DefNs && !strcmp(pf, Doc->DefNs))
      pf = NULL;                        // Default namespace

    xmlNsPtr nsp = xmlSearchNs(Docp, nop, BAD_CAST pf);

    if (!nsp)
      nsp = xmlNewNs(nop, NULL, BAD_CAST pf);

    // Set node namespace
    nop->ns = nsp;
    *(--p) = ':';                       // Restore Xname
  } else if (Doc->DefNs && xmlSearchNs(Docp, nop, NULL))
    // Not in default namespace
    nop->ns = xmlNewNs(nop, BAD_CAST "", NULL);

  if (np)
    ((PNODE2)np)->Nodep = nop;
  else
    np = new(g) XML2NODE(Doc, nop);

  return NewChild(np);
  } // end of AddChildNode

/******************************************************************/
/*  Add a new property to this node and return it.                */
/******************************************************************/
PXATTR XML2NODE::AddProperty(PGLOBAL g, char *name, PXATTR ap)
  {
  if (trace(1))
    htrc("AddProperty: %-.256s\n", name);

  xmlAttrPtr atp = xmlNewProp(Nodep, BAD_CAST name, NULL);

  if (atp) {
    if (ap) {
      ((PATTR2)ap)->Atrp = atp;
      ((PATTR2)ap)->Parent = Nodep;
      return ap;
    } else
      return new(g) XML2ATTR(Doc, atp, Nodep);

  } else
    return NULL;

  } // end of AddProperty

/******************************************************************/
/*  Add a new text node to this node.                             */
/******************************************************************/
void XML2NODE::AddText(PGLOBAL g, PCSZ txtp)
  {
  if (trace(1))
    htrc("AddText: %-.256s\n", txtp);

  // This is to avoid a blank line when inserting a new line
  xmlNodePtr np = xmlGetLastChild(Nodep);

  if (np && np->type == XML_TEXT_NODE) {
    xmlUnlinkNode(np);
    xmlFreeNode(np);
    } // endif type

  // Add the new text
  xmlAddChild(Nodep, xmlNewText(BAD_CAST txtp));
  } // end of AddText

/******************************************************************/
/*  Remove a child node from this node.                           */
/******************************************************************/
void XML2NODE::DeleteChild(PGLOBAL g, PXNODE dnp)
  {
  xmlErrorPtr xerr;

  if (trace(1))
    htrc("DeleteChild: node=%p\n", dnp);

  xmlNodePtr np = ((PNODE2)dnp)->Nodep;
  xmlNodePtr text = np->next;

  // This is specific to row nodes
  if (text && text->type == XML_TEXT_NODE) {
    xmlUnlinkNode(text);

    if ((xerr = xmlGetLastError()))
      goto err;

    xmlFreeNode(text);

    if ((xerr = xmlGetLastError()))
      goto err;

    } // endif type

  xmlUnlinkNode(np);

  if ((xerr = xmlGetLastError()))
    goto err;

  xmlFreeNode(np);

  if ((xerr = xmlGetLastError()))
    goto err;

  Delete(dnp);

  if ((xerr = xmlGetLastError()))
    goto err;

  return;

err:
  if (trace(1))
    htrc("DeleteChild: errmsg=%-.256s\n", xerr->message);

  xmlResetError(xerr);
  } // end of DeleteChild

/* -------------------- class XML2NODELIST ---------------------- */

/******************************************************************/
/*  XML2NODELIST constructor.                                     */
/******************************************************************/
XML2NODELIST::XML2NODELIST(PXDOC dp, xmlNodeSetPtr lp)
            : XMLNODELIST(dp)
  {
  Listp = lp;
  } // end of XML2NODELIST constructor

/******************************************************************/
/*  Return the length of the list.                                */
/******************************************************************/
int XML2NODELIST::GetLength(void)
  {
  return (Listp) ? Listp->nodeNr : 0;
  } // end of GetLength

/******************************************************************/
/*  Return the nth element of the list.                           */
/******************************************************************/
PXNODE XML2NODELIST::GetItem(PGLOBAL g, int n, PXNODE np)
  {
  if (trace(1))
    htrc("GetItem: %d\n", n);

  if (!Listp || Listp->nodeNr <= n)
    return NULL;

  if (np) {
    ((PNODE2)np)->Nodep = Listp->nodeTab[n];
    return np;
  } else
    return new(g) XML2NODE(Doc, Listp->nodeTab[n]);

  } // end of GetItem

/******************************************************************/
/*  Reset the pointer on the deleted item.                        */
/******************************************************************/
bool XML2NODELIST::DropItem(PGLOBAL g, int n)
  {
  if (trace(1))
    htrc("DropItem: n=%d\n", n);

  // We should do something here
  if (!Listp || Listp->nodeNr <= n)
    return true;

  Listp->nodeTab[n] = NULL;   // This was causing Valgrind warning
  return false;
  } // end of DropItem

/* ---------------------- class XML2ATTR ------------------------ */

/******************************************************************/
/*  XML2ATTR constructor.                                         */
/******************************************************************/
XML2ATTR::XML2ATTR(PXDOC dp, xmlAttrPtr ap, xmlNodePtr np)
        : XMLATTRIBUTE(dp)
  {
  Atrp = ap;
  Parent = np;
  } // end of XML2ATTR constructor

/******************************************************************/
/*  Return the next sibling of the attribute.                     */
/******************************************************************/
PXATTR XML2ATTR::GetNext(PGLOBAL g)
  {
  if (trace(1))
    htrc("Attr GetNext\n");

  if (!Atrp->next)
    return NULL;
  else
    return new(g) XML2ATTR(Doc, Atrp->next, Atrp->parent);

  } // end of GetNext

/******************************************************************/
/*  Return the text of an attribute.                              */
/******************************************************************/
RCODE XML2ATTR::GetText(PGLOBAL g, char *buf, int len)
  {
  RCODE    rc = RC_OK;
  xmlChar *txt;

  if (trace(1))
    htrc("GetText\n");

  if ((txt = xmlGetProp(Atrp->parent, Atrp->name))) {
    // Copy the text to the buffer
    if (strlen((char*)txt) >= (unsigned)len) {
      memcpy(buf, txt, len - 1);
      buf[len - 1] = 0;
      sprintf(g->Message, "Truncated %-.256s content", Atrp->name);
      rc = RC_INFO;
    } else
      strcpy(buf, (const char*)txt);

    xmlFree(txt);
  } else
    *buf = '\0';

  if (trace(1))
    htrc("GetText: %-.256s\n", buf);

  return rc;
  } // end of GetText

/******************************************************************/
/*  Set the content of an attribute.                              */
/******************************************************************/
bool XML2ATTR::SetText(PGLOBAL g, char *txtp, int len)
  {
  if (trace(1))
    htrc("SetText: %-.256s %d\n", txtp, len);

  xmlSetProp(Parent, Atrp->name, BAD_CAST txtp);
  return false;
  } // end of SetText
