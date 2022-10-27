/******************************************************************/
/*  Implementation of XML document processing using PdbXML.       */
/*  Author: Olivier Bertrand                2007-2017             */
/******************************************************************/
#include "my_global.h"
#include "global.h"
#include "plgdbsem.h"
#include "block.h"
#include "plgxml.h"

#if !defined(DOMDOC_SUPPORT)
PXDOC GetDomDoc(PGLOBAL g, char *nsl, char *nsdf, 
                                      char *enc, PFBLOCK fp)
  {
  strcpy(g->Message, MSG(DOM_NOT_SUPP));
  return NULL;
  } // end of GetDomDoc
#endif   // !DOMDOC_SUPPORT

#ifndef LIBXML2_SUPPORT
PXDOC GetLibxmlDoc(PGLOBAL g, char *nsl, char *nsdf, 
                              char *enc, PFBLOCK fp)
  {
  strcpy(g->Message, "libxml2 not supported");
  return NULL;
  } // end of GetLibxmlDoc
#endif   // LIBXML2_SUPPORT

/******************************************************************/
/*  XMLDOCUMENT constructor.                                      */
/******************************************************************/
XMLDOCUMENT::XMLDOCUMENT(char *nsl, char *nsdf, char *enc)
{
#if defined(ZIP_SUPPORT)
	zip = NULL;
#else   // !ZIP_SUPPORT
	zip = false;
#endif  // !ZIP_SUPPORT
	Namespaces = NULL;
  Encoding = enc;
  Nslist = nsl;
  DefNs = nsdf;
} // end of XMLDOCUMENT constructor

/******************************************************************/
/*  Initialize zipped file processing.                            */
/******************************************************************/
bool XMLDOCUMENT::InitZip(PGLOBAL g, PCSZ entry)
{
#if defined(ZIP_SUPPORT)
	bool mul = (entry) ? strchr(entry, '*') || strchr(entry, '?') : false;
	zip = new(g) UNZIPUTL(entry, NULL, mul);
	return zip == NULL;
#else   // !ZIP_SUPPORT
	sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
	return true;
#endif  // !ZIP_SUPPORT
} // end of InitZip

/******************************************************************/
/*  Make the namespace structure list.                            */
/******************************************************************/
char* XMLDOCUMENT::GetMemDoc(PGLOBAL g, char *fn)
{
#if defined(ZIP_SUPPORT)
	return (zip->OpenTable(g, MODE_ANY, fn)) ? NULL : zip->memory;
#else   // !ZIP_SUPPORT
	return NULL;
#endif  // !ZIP_SUPPORT
} // end of GetMemDoc

/******************************************************************/
/*  Make the namespace structure list.                            */
/******************************************************************/
bool XMLDOCUMENT::MakeNSlist(PGLOBAL g)
{
	char *prefix, *href, *next = Nslist;
  PNS   nsp, *ppns = &Namespaces;

  while (next) {
    // Skip spaces
    while ((*next) == ' ')
      next++;

    if ((*next) == '\0')
      break;

    // Find prefix
    prefix = next;
    next = strchr(next, '=');

    if (next == NULL) {
      strcpy(g->Message, MSG(BAS_NS_LIST));
      return true;
      } // endif next

    *(next++) = '\0';

    // Find href
    href = next;
    next = strchr(next, ' ');

    if (next != NULL)
      *(next++) = '\0';

    // Allocate and link NS structure
    nsp = (PNS)PlugSubAlloc(g, NULL, sizeof(NS));
    nsp->Next = NULL;
    nsp->Prefix = prefix;
    nsp->Uri = href;
    *ppns = nsp;
    ppns = &nsp->Next;
    } // endwhile next

  return false;
  } // end of MakeNSlist

/******************************************************************/
/*  Close ZIP file.                                               */
/******************************************************************/
void XMLDOCUMENT::CloseZip(void)
{
#if defined(ZIP_SUPPORT)
	if (zip) {
		zip->close();
		zip = NULL;
	}	// endif zip
#endif   //	ZIP_SUPPORT
} // end of CloseZip

/******************************************************************/
/*  XMLNODE constructor.                                          */
/******************************************************************/
XMLNODE::XMLNODE(PXDOC dp)
  {
  Doc = dp;
  Next = NULL;
  Children = NULL;
  Buf = NULL;
  Len = -1;
  } // end of XMLNODE constructor

/******************************************************************/
/*  Attach new node at the end of this node children list.        */
/******************************************************************/
PXNODE XMLNODE::NewChild(PXNODE ncp)
{
  PXNODE np, *pnp = &Children;

  for (np = *pnp; np; np = np->Next)
    pnp = &np->Next;

  *pnp = np;
  return ncp;
} // end of NewChild

/******************************************************************/
/*  Delete a node from this node children list.                   */
/******************************************************************/
void XMLNODE::Delete(PXNODE dnp)
  {
  PXNODE *pnp = &Children;

  for (PXNODE np = *pnp; np; np = np->Next)
    if (np == dnp) {
      *pnp = dnp->Next;
      break;
    } else
      pnp = &np->Next;

  } // end of Delete

/******************************************************************/
/*  Store a string in Buf, enventually reallocating it.           */
/******************************************************************/
char *XMLNODE::BufAlloc(PGLOBAL g, const char *p, int n)
  {
  if (Len < n) {
    Len = n;
    Buf = (char*)PlugSubAlloc(g, NULL, n + 1);
    } // endif Len

  *Buf = '\0';
  return strncat(Buf, p, n);
  } // end of BufAlloc
