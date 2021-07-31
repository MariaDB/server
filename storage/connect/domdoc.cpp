/******************************************************************/
/*  Implementation of XML document processing using MS DOM        */
/*  Author: Olivier Bertrand                2007 - 2013           */          
/******************************************************************/
#include "my_global.h"
#include <stdio.h>
#if defined(_WIN32)
//#include <windows.h>
#if   defined(MSX2)
#import "msxml2.dll"  //Does not exist on Vista
#elif defined(MSX3)
#import "msxml3.dll"  //Causes error C2872: DOMNodeType: ambiguous symbol  ??
#elif defined(MSX4)
#import "msxml4.dll"  //Causes error C2872: DOMNodeType: ambiguous symbol  ??
#elif defined(MSX6)
#pragma warning(suppress : 4192)
#import "msxml6.dll"  //Causes error C2872: DOMNodeType: ambiguous symbol  ??
#else       // MSX4
#error MSX? is not defined
#endif     // MSX
using namespace MSXML2;
#else
#error This is a Windows implementation only
#endif

#define NODE_TYPE_LIST

#include "global.h"
#include "plgdbsem.h"
#include "xobject.h"
#include "domdoc.h"

inline bool TestHr(PGLOBAL g, HRESULT hr)
  {
  if FAILED(hr) {
    sprintf(g->Message, "%s, hr=%d", MSG(COM_ERROR), hr);
    return true;
  } else
    return false;

  } // end of TestHr

/******************************************************************/
/*  Return a DOMDOC as a XMLDOC.                                  */
/******************************************************************/
PXDOC GetDomDoc(PGLOBAL g, char *nsl, char *nsdf, 
                                      char *enc, PFBLOCK fp)
  {
  return (PXDOC) new(g) DOMDOC(nsl, nsdf, enc, fp);
  }  // end of GetDomDoc

/***********************************************************************/
/*  Close a loaded DOM XML file.                                       */
/***********************************************************************/
void CloseXMLFile(PGLOBAL g, PFBLOCK fp, bool all)
  {
  PXBLOCK xp = (PXBLOCK)fp;

  if (xp && xp->Count > 1 && !all) {
    xp->Count--;
  } else if (xp && xp->Count > 0) {
    try {
      if (xp->Docp)
        xp->Docp->Release();

    } catch(_com_error e)  {
			char *p = _com_util::ConvertBSTRToString(e.Description());
      sprintf(g->Message, "%s %s", MSG(COM_ERROR), p);
			delete[] p;
    } catch(...) {}

    CoUninitialize();
    xp->Count = 0;
  }  // endif

  } // end of CloseXMLFile

/* ------------------------ class DOMDOC ------------------------ */

/******************************************************************/
/*  DOMDOC constructor.                                           */
/******************************************************************/
DOMDOC::DOMDOC(char *nsl, char *nsdf, char *enc, PFBLOCK fp)
      : XMLDOCUMENT(nsl, nsdf, enc)
  {
  assert (!fp || fp->Type == TYPE_FB_XML);
  Docp = (fp) ? ((PXBLOCK)fp)->Docp : (MSXML2::IXMLDOMDocumentPtr)NULL;
  Nlist = NULL;
  Hr = 0;
  } // end of DOMDOC constructor

/******************************************************************/
/*  Initialize XML parser and check library compatibility.        */
/******************************************************************/
bool DOMDOC::Initialize(PGLOBAL g, PCSZ entry, bool zipped)
{
	if (zipped && InitZip(g, entry))
		return true;

	if (TestHr(g, CoInitialize(NULL)))
    return true;

  if (TestHr(g, Docp.CreateInstance("msxml2.domdocument")))
    return true;

  return MakeNSlist(g);
} // end of Initialize

/******************************************************************/
/* Parse the XML file and construct node tree in memory.          */
/******************************************************************/
bool DOMDOC::ParseFile(PGLOBAL g, char *fn)
{
	bool b;

  Docp->async = false;

	if (zip) {
		// Parse an in memory document
		char *xdoc = GetMemDoc(g, fn);

		// This is not equivalent to load for UTF8 characters
		// It is why get node content is not the same
  	b = (xdoc) ? (bool)Docp->loadXML((_bstr_t)xdoc) : false;
	} else
		// Load the document
		b = (bool)Docp->load((_bstr_t)fn);

	if (!b)
    return true;

  return false;
} // end of ParseFile

/******************************************************************/
/* Create or reuse an Xblock for this document.                   */
/******************************************************************/
PFBLOCK DOMDOC::LinkXblock(PGLOBAL g, MODE m, int rc, char *fn)
  {
  PDBUSER dup = (PDBUSER)g->Activityp->Aptr;
  PXBLOCK xp = (PXBLOCK)PlugSubAlloc(g, NULL, sizeof(XBLOCK));

  memset(xp, 0, sizeof(XBLOCK));
  xp->Next = (PXBLOCK)dup->Openlist;
  dup->Openlist = (PFBLOCK)xp;
  xp->Type = TYPE_FB_XML;
  xp->Fname = (LPCSTR)PlugSubAlloc(g, NULL, strlen(fn) + 1);
  strcpy((char*)xp->Fname, fn);
  xp->Count = 1;
  xp->Length = (m == MODE_READ) ? 1 : 0;
  xp->Docp = Docp;
  xp->Retcode = rc;

  // Return xp as a fp 
  return (PFBLOCK)xp;
  } // end of LinkXblock

/******************************************************************/
/* Create the XML node.                                           */
/******************************************************************/
bool DOMDOC::NewDoc(PGLOBAL g, PCSZ ver)
  {
  char buf[64];
  MSXML2::IXMLDOMProcessingInstructionPtr pip;

  sprintf(buf, "version=\"%s\" encoding=\"%s\"", ver, Encoding);
  pip = Docp->createProcessingInstruction("xml", buf);
  return(TestHr(g, Docp->appendChild(pip)));
  } // end of NewDoc

/******************************************************************/
/* Add a comment to the document node.                            */
/******************************************************************/
void DOMDOC::AddComment(PGLOBAL g, char *com)
  {
  TestHr(g, Docp->appendChild(Docp->createComment(com)));
  } // end of AddComment

/******************************************************************/
/* Return the node class of the root of the document.             */
/******************************************************************/
PXNODE DOMDOC::GetRoot(PGLOBAL g)
  {
  MSXML2::IXMLDOMElementPtr root = Docp->documentElement;

  if (root == NULL)
    return NULL;

  return new(g) DOMNODE(this, root); 
  } // end of GetRoot

/******************************************************************/
/* Create a new root element and return its class node.           */
/******************************************************************/
PXNODE DOMDOC::NewRoot(PGLOBAL g, char *name)
  {
  MSXML2::IXMLDOMElementPtr ep = Docp->createElement(name);

  if (ep == NULL || TestHr(g, Docp->appendChild(ep)))
    return NULL;

  return new(g) DOMNODE(this, ep); 
  } // end of NewRoot

/******************************************************************/
/* Return a void DOMNODE node class.                              */
/******************************************************************/
PXNODE DOMDOC::NewPnode(PGLOBAL g, char *name)
  {
  MSXML2::IXMLDOMElementPtr root = NULL;

  if (name)
    if ((root = Docp->createElement(name)) == NULL)
      return NULL;

  return new(g) DOMNODE(this, root);
  } // end of NewPnode

/******************************************************************/
/* Return a void DOMATTR node class.                              */
/******************************************************************/
PXATTR DOMDOC::NewPattr(PGLOBAL g)
  {
  return new(g) DOMATTR(this, NULL);
  } // end of NewPattr

/******************************************************************/
/* Return a void DOMATTR node class.                              */
/******************************************************************/
PXLIST DOMDOC::NewPlist(PGLOBAL g)
  {
  return new(g) DOMNODELIST(this, NULL);
  } // end of NewPlist

/******************************************************************/
/* Dump the node tree to a new XML file.                          */
/******************************************************************/
int DOMDOC::DumpDoc(PGLOBAL g, char *ofn)
  {
  int rc = 0;

  try {
    Docp->save(ofn);
  } catch(_com_error e)  {
    sprintf(g->Message, "%s: %s", MSG(COM_ERROR), 
            _com_util::ConvertBSTRToString(e.Description()));
    rc = -1;
  }  catch(...) {}

  return rc;
  } // end of Dump

/******************************************************************/
/* Free the document, cleanup the XML library, and                */
/* debug memory for regression tests.                             */
/******************************************************************/
void DOMDOC::CloseDoc(PGLOBAL g, PFBLOCK xp)
  {
  CloseXMLFile(g, xp, false);
	CloseZip();
  } // end of Close

/* ----------------------- class DOMNODE ------------------------ */

/******************************************************************/
/*  DOMNODE constructor.                                          */
/******************************************************************/
DOMNODE::DOMNODE(PXDOC dp, MSXML2::IXMLDOMNodePtr np) : XMLNODE(dp)
  {
  Docp = ((PDOMDOC)dp)->Docp;
  Nodep = np;
  Ws = NULL;
  Len = 0;
	Zip = (bool)dp->zip;
  } // end of DOMNODE constructor

/******************************************************************/
/*  Return the node name.                                         */
/******************************************************************/
char *DOMNODE::GetName(PGLOBAL g)
  {
  if (!WideCharToMultiByte(CP_ACP, 0, Nodep->nodeName, -1,
                           Name, sizeof(Name), NULL, NULL)) {
    strcpy(g->Message, MSG(NAME_CONV_ERR));
    return NULL;
    } // endif

  return Name;
  }  // end of GetName

/******************************************************************/
/*  Return the node class of next sibling of the node.            */
/******************************************************************/
PXNODE DOMNODE::GetNext(PGLOBAL g)
  {
  if (Nodep->nextSibling == NULL)
    Next = NULL;
  else // if (!Next)
    Next = new(g) DOMNODE(Doc, Nodep->nextSibling);

  return Next;
  } // end of GetNext

/******************************************************************/
/*  Return the node class of first children of the node.          */
/******************************************************************/
PXNODE DOMNODE::GetChild(PGLOBAL g)
  {
  if (Nodep->firstChild == NULL)
    Children = NULL;
  else // if (!Children)
    Children = new(g) DOMNODE(Doc, Nodep->firstChild);

  return Children;
  } // end of GetChild

/******************************************************************/
/*  Return the content of a node and subnodes.                    */
/******************************************************************/
RCODE DOMNODE::GetContent(PGLOBAL g, char *buf, int len)
  {
  RCODE rc = RC_OK;

  // Nodep can be null for a missing HTML table column
  if (Nodep) {
		if (Zip) {
			strcpy(buf, Nodep->text);
		} else if (!WideCharToMultiByte(CP_UTF8, 0, Nodep->text, -1,
                             buf, len, NULL, NULL)) {
      DWORD lsr = GetLastError();

      switch (lsr) {
        case 0:
        case ERROR_INSUFFICIENT_BUFFER:      // 122L
          sprintf(g->Message, "Truncated %s content", GetName(g));
          rc = RC_INFO;
          break;
        case ERROR_NO_UNICODE_TRANSLATION:   // 1113L
          sprintf(g->Message, "Invalid character(s) in %s content",
                              GetName(g));
          rc = RC_INFO;
          break;
        default:
          sprintf(g->Message, "System error getting %s content",
                              GetName(g));
          rc = RC_FX;
          break;
        } // endswitch

      } // endif

  } else
    *buf = '\0';

  return rc;
  } // end of GetContent

/******************************************************************/
/*  Set the text content of an attribute.                         */
/******************************************************************/
bool DOMNODE::SetContent(PGLOBAL g, char *txtp, int len)
  {
  bool rc;
  BSTR val;

  if (len > Len || !Ws) {
    Ws = (WCHAR*)PlugSubAlloc(g, NULL, (len + 1) * 2);
    Len = len;
    } // endif len

  if (!MultiByteToWideChar(CP_UTF8, 0, txtp, strlen(txtp) + 1,
                                       Ws, Len + 1)) {
    sprintf(g->Message, MSG(WS_CONV_ERR), txtp);
    return true;
    } // endif

  val = SysAllocString(Ws);
  rc = TestHr(g, Nodep->put_text(val));
  SysFreeString(val);
  return rc;
  } // end of SetContent

/******************************************************************/
/*  Return a clone of this node.                                  */
/******************************************************************/
PXNODE DOMNODE::Clone(PGLOBAL g, PXNODE np)
  {
  if (np) {
    ((PDOMNODE)np)->Nodep = Nodep;
    return np;
  } else
    return new(g) DOMNODE(Doc, Nodep);

  } // end of Clone

/******************************************************************/
/*  Return the list of all or matching children that are elements.*/
/******************************************************************/
PXLIST DOMNODE::GetChildElements(PGLOBAL g, char *xp, PXLIST lp)
  {
  MSXML2::IXMLDOMNodeListPtr dnlp;

  if (xp) {
    if (Nodep->nodeType == MSXML2::NODE_ELEMENT) {
      MSXML2::IXMLDOMElementPtr ep = Nodep;
      dnlp = ep->getElementsByTagName(xp);
    } else
      return NULL;

  } else
    dnlp = Nodep->childNodes;

  if (lp) {
    ((PDOMLIST)lp)->Listp = dnlp;
    return lp;
  } else
    return new(g) DOMNODELIST(Doc, dnlp);

  } // end of GetChildElements

/******************************************************************/
/*  Return the list of nodes verifying the passed Xapth.          */
/******************************************************************/
PXLIST DOMNODE::SelectNodes(PGLOBAL g, char *xp, PXLIST lp)
  {
  MSXML2::IXMLDOMNodeListPtr dnlp = Nodep->selectNodes(xp);

  if (lp) {
    ((PDOMLIST)lp)->Listp = dnlp;
    return lp;
  } else
    return new(g) DOMNODELIST(Doc, dnlp);

  } // end of SelectNodes

/******************************************************************/
/*  Return the first node verifying the passed Xapth.             */
/******************************************************************/
PXNODE DOMNODE::SelectSingleNode(PGLOBAL g, char *xp, PXNODE np)
  {
  try {
    MSXML2::IXMLDOMNodePtr dnp = Nodep->selectSingleNode(xp);

    if (dnp) {
      if (np) {
        ((PDOMNODE)np)->Nodep = dnp;
        return np;
      } else
        return new(g) DOMNODE(Doc, dnp);

      } // endif dnp

  } catch(_com_error e) {
    sprintf(g->Message, "%s: %s", MSG(COM_ERROR), 
            _com_util::ConvertBSTRToString(e.Description()));
  } catch(...) {}

  return NULL;
  } // end of SelectSingleNode

/******************************************************************/
/*  Return the node attribute with the specified name.            */
/******************************************************************/
PXATTR DOMNODE::GetAttribute(PGLOBAL g, char *name, PXATTR ap)
  {
  MSXML2::IXMLDOMElementPtr ep;
  MSXML2::IXMLDOMNamedNodeMapPtr nmp;
  MSXML2::IXMLDOMAttributePtr atp;

  if (name) {
    ep = Nodep;
    atp = ep->getAttributeNode(name);
    nmp = NULL;
  } else {
    nmp = Nodep->Getattributes();
    atp = nmp->Getitem(0);
  } // endif name

  if (atp) {
    if (ap) {
      ((PDOMATTR)ap)->Atrp = atp;
      ((PDOMATTR)ap)->Nmp = nmp;
      ((PDOMATTR)ap)->K = 0;
      return ap;
    } else
      return new(g) DOMATTR(Doc, atp, nmp);

  } else
    return NULL;

  } // end of GetAttribute

/******************************************************************/
/*  Add a new element child node to this node and return it.      */
/******************************************************************/
PXNODE DOMNODE::AddChildNode(PGLOBAL g, PCSZ name, PXNODE np)
  {
  const char *p, *pn;
//  char *p, *pn, *epf, *pf = NULL;
  MSXML2::IXMLDOMNodePtr ep;
//  _bstr_t   uri((wchar_t*)NULL);

#if 0
  // Is a prefix specified ?
  if ((p = strchr(name, ':'))) {
    pf = BufAlloc(g, name, p - name);

    // Is it the pseudo default prefix
    if (Doc->DefNs && !strcmp(pf, Doc->DefNs)) {
      name = p + 1;              // Suppress it from name
      pf = NULL;                // No real prefix
      } // endif DefNs

    } // endif p

  // Look for matching namespace URI in context
  for (ep = Nodep; ep; ep = ep->parentNode) {
    epf = (_bstr_t)ep->prefix;

    if ((!pf && !epf) || (pf && epf && !strcmp(pf, epf))) {
      uri = Nodep->namespaceURI;
      break;
      } // endif

    } // endfor ep

  if ((wchar_t*)uri == NULL) {
    if (!pf)
      pf = Doc->DefNs;

    // Look for the namespace URI corresponding to this node
    if (pf)
      for (PNS nsp = Doc->Namespaces; nsp; nsp = nsp->Next)
        if (!strcmp(pf, nsp->Prefix)) {
          uri = nsp->Uri;
          break;
          } // endfor nsp

    } // endif pns
#endif // 0

  // If name has the format m[n] only m is taken as node name
  if ((p = strchr(name, '[')))
    pn = BufAlloc(g, name, (int)(p - name));
  else
    pn = name;

  // Construct the element node with eventual namespace
//  ep = Docp->createNode(_variant_t("Element"), pn, uri);
  ep = Docp->createElement(pn);

  _bstr_t pfx = ep->prefix;
  _bstr_t uri = ep->namespaceURI;

  if (ep == NULL || TestHr(g, Nodep->appendChild(ep)))
    return NULL;

  if (np)
    ((PDOMNODE)np)->Nodep = ep;
  else
    np = new(g) DOMNODE(Doc, ep);

  return NewChild(np);
  } // end of AddChildNode

/******************************************************************/
/*  Add a new property to this node and return it.                */
/******************************************************************/
PXATTR DOMNODE::AddProperty(PGLOBAL g, char *name, PXATTR ap)
  {
  MSXML2::IXMLDOMAttributePtr atp = Docp->createAttribute(name);

  if (atp) {
    MSXML2::IXMLDOMElementPtr ep = Nodep;
    ep->setAttributeNode(atp);

    if (ap) {
      ((PDOMATTR)ap)->Atrp = atp;
      return ap;
    } else
      return new(g) DOMATTR(Doc, atp);

  } else
    return NULL;

  } // end of AddProperty

/******************************************************************/
/*  Add a new text node to this node.                             */
/******************************************************************/
void DOMNODE::AddText(PGLOBAL g, PCSZ txtp)
  {
  MSXML2::IXMLDOMTextPtr tp= Docp->createTextNode((_bstr_t)txtp);

  if (tp != NULL)
    TestHr(g, Nodep->appendChild(tp));

  } // end of AddText

/******************************************************************/
/*  Remove a child node from this node.                           */
/******************************************************************/
void DOMNODE::DeleteChild(PGLOBAL g, PXNODE dnp)
  {
  TestHr(g, Nodep->removeChild(((PDOMNODE)dnp)->Nodep));
//  ((PDOMNODE)dnp)->Nodep->Release();  bad idea, causes a crash
  Delete(dnp);
  } // end of DeleteChild

/* --------------------- class DOMNODELIST ---------------------- */

/******************************************************************/
/*  DOMNODELIST constructor.                                      */
/******************************************************************/
DOMNODELIST::DOMNODELIST(PXDOC dp, MSXML2::IXMLDOMNodeListPtr lp)
           : XMLNODELIST(dp)
  {
  Listp = lp;
  } // end of DOMNODELIST constructor

/******************************************************************/
/*  Return the nth element of the list.                           */
/******************************************************************/
PXNODE DOMNODELIST::GetItem(PGLOBAL g, int n, PXNODE np)
  {
  if (Listp == NULL || Listp->length <= n)
    return NULL;

  if (np) {
    ((PDOMNODE)np)->Nodep = Listp->item[n];
    return np;
  } else
    return new(g) DOMNODE(Doc, Listp->item[n]);

  }  // end of GetItem

/******************************************************************/
/*  Reset the pointer on the deleted item.                        */
/******************************************************************/
bool DOMNODELIST::DropItem(PGLOBAL g, int n)
{
	if (Listp == NULL || Listp->length < n)
		return true;

//Listp->item[n] = NULL;  La propriété n'a pas de méthode 'set'
  return false;
}  // end of DeleteItem

/* ----------------------- class DOMATTR ------------------------ */

/******************************************************************/
/*  DOMATTR constructor.                                          */
/******************************************************************/
DOMATTR::DOMATTR(PXDOC dp, MSXML2::IXMLDOMAttributePtr ap,
                           MSXML2::IXMLDOMNamedNodeMapPtr nmp)
        : XMLATTRIBUTE(dp)
  {
  Atrp = ap;
  Nmp = nmp;
  Ws = NULL;
  Len = 0;
  K = 0;
  }  // end of DOMATTR constructor

/******************************************************************/
/*  Return the attribute name.                                    */
/******************************************************************/
char *DOMATTR::GetName(PGLOBAL g)
  {
  if (!WideCharToMultiByte(CP_ACP, 0, Atrp->nodeName, -1,
                           Name, sizeof(Name), NULL, NULL)) {
    strcpy(g->Message, MSG(NAME_CONV_ERR));
    return NULL;
    } // endif

  return Name;
  }  // end of GetName

/******************************************************************/
/*  Return the next attribute node.                               */
/*  This funtion is implemented as needed by XMLColumns.          */
/******************************************************************/
PXATTR DOMATTR::GetNext(PGLOBAL g)
  {
  if (!Nmp)
    return NULL;

  if (++K >= Nmp->Getlength()) {
    Nmp->reset();
    Nmp = NULL;
    K = 0;
    return NULL;
    } // endif K

  Atrp = Nmp->Getitem(K);
  return this;
  } // end of GetNext

/******************************************************************/
/*  Return the content of a node and subnodes.                    */
/******************************************************************/
RCODE DOMATTR::GetText(PGLOBAL g, char *buf, int len)
  {
  RCODE rc = RC_OK;

  if (!WideCharToMultiByte(CP_UTF8, 0, Atrp->text, -1,
                           buf, len, NULL, NULL)) {
    DWORD lsr = GetLastError();

    switch (lsr) {
      case 0:
      case ERROR_INSUFFICIENT_BUFFER:      // 122L
        sprintf(g->Message, "Truncated %s content", GetName(g));
        rc = RC_INFO;
        break;
      case ERROR_NO_UNICODE_TRANSLATION:   // 1113L
        sprintf(g->Message, "Invalid character(s) in %s content",
                            GetName(g));
        rc = RC_INFO;
        break;
      default:
        sprintf(g->Message, "System error getting %s content",
                            GetName(g));
        rc = RC_FX;
        break;
      } // endswitch

    } // endif

  return rc;
  } // end of GetText

/******************************************************************/
/*  Set the text content of an attribute.                         */
/******************************************************************/
bool DOMATTR::SetText(PGLOBAL g, char *txtp, int len)
  {
  bool rc;
  BSTR val;

  if (len > Len || !Ws) {
    Ws = (WCHAR*)PlugSubAlloc(g, NULL, (len + 1) * 2);
    Len = len;
    } // endif len

  if (!MultiByteToWideChar(CP_UTF8, 0, txtp, strlen(txtp) + 1,
                                       Ws, Len + 1)) {
    sprintf(g->Message, MSG(WS_CONV_ERR), txtp);
    return true;
    } // endif

  val = SysAllocString(Ws);
  rc = TestHr(g, Atrp->put_text(val));
  SysFreeString(val);
  return rc;
  } // end of SetText
