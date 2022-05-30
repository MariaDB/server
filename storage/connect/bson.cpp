/*************** bson CPP Declares Source Code File (.H) ***************/
/*  Name: bson.cpp   Version 1.0                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2020         */
/*                                                                     */
/*  This file contains the BJSON classes functions.                    */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  bson.h      is header containing the BSON classes declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "bson.h"

/***********************************************************************/
/*  Check macro.                                                       */
/***********************************************************************/
#if defined(_DEBUG)
#define CheckType(X,Y) if (!X || X ->Type != Y) throw MSG(VALTYPE_NOMATCH);
#else
#define CheckType(X,Y)
#endif

#if defined(_WIN32)
#define EL  "\r\n"
#else
#define EL  "\n"
#undef     SE_CATCH                  // Does not work for Linux
#endif

int GetJsonDefPrec(void);

#if defined(SE_CATCH)
/**************************************************************************/
/*  This is the support of catching C interrupts to prevent crashes.      */
/**************************************************************************/
#include <eh.h>

class SE_Exception {
public:
  SE_Exception(unsigned int n, PEXCEPTION_RECORD p) : nSE(n), eRec(p) {}
  ~SE_Exception() {}

  unsigned int      nSE;
  PEXCEPTION_RECORD eRec;
};  // end of class SE_Exception

void trans_func(unsigned int u, _EXCEPTION_POINTERS* pExp) {
  throw SE_Exception(u, pExp->ExceptionRecord);
} // end of trans_func

char* GetExceptionDesc(PGLOBAL g, unsigned int e);
#endif   // SE_CATCH

/* --------------------------- Class BDOC ---------------------------- */

/***********************************************************************/
/*  BDOC constructor.                                                  */
/***********************************************************************/
BDOC::BDOC(PGLOBAL G) : BJSON(G, NULL)
{ 
  jp = NULL;
  s = NULL;
  len = 0;
  pretty = 3;
  pty[0] = pty[1] = pty[2] = true;
  comma = false;
} // end of BDOC constructor

/***********************************************************************/
/* Parse a json string.                                                */
/* Note: when pretty is not known, the caller set pretty to 3.         */
/***********************************************************************/
PBVAL BDOC::ParseJson(PGLOBAL g, char* js, size_t lng)
{
  size_t i;
  bool  b = false;
  PBVAL bvp = NULL;

  s = js;
  len = lng;
  xtrc(1, "BDOC::ParseJson: s=%.10s len=%zd\n", s, len);

  if (!s || !len) {
    strcpy(g->Message, "Void JSON object");
    return NULL;
  } // endif s

  // Trying to guess the pretty format
  if (s[0] == '[' && (s[1] == '\n' || (s[1] == '\r' && s[2] == '\n')))
    pty[0] = false;

  try {
    bvp = NewVal();
    bvp->Type = TYPE_UNKNOWN;

    for (i = 0; i < len; i++)
      switch (s[i]) {
      case '[':
        if (bvp->Type != TYPE_UNKNOWN)
          bvp->To_Val = ParseAsArray(i);
        else
          bvp->To_Val = ParseArray(++i);

        bvp->Type = TYPE_JAR;
        break;
      case '{':
        if (bvp->Type != TYPE_UNKNOWN) {
          bvp->To_Val = ParseAsArray(i);
          bvp->Type = TYPE_JAR;
        } else {
          bvp->To_Val = ParseObject(++i);
          bvp->Type = TYPE_JOB;
        } // endif Type

        break;
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        break;
      case ',':
        if (bvp->Type != TYPE_UNKNOWN && (pretty == 1 || pretty == 3)) {
          comma = true;
          pty[0] = pty[2] = false;
          break;
        } // endif pretty

        sprintf(g->Message, "Unexpected ',' (pretty=%d)", pretty);
        throw 3;
      case '(':
        b = true;
        break;
      case ')':
        if (b) {
          b = false;
          break;
        } // endif b
        /* fall through */
      default:
        if (bvp->Type != TYPE_UNKNOWN) {
          bvp->To_Val = ParseAsArray(i);
          bvp->Type = TYPE_JAR;
        } else if ((bvp->To_Val = MOF(ParseValue(i, NewVal()))))
          bvp->Type = TYPE_JVAL;
        else
          throw 4;

        break;
      }; // endswitch s[i]

    if (bvp->Type == TYPE_UNKNOWN)
      sprintf(g->Message, "Invalid Json string '%.*s'", MY_MIN((int)len, 50), s);
    else if (pretty == 3) {
      for (i = 0; i < 3; i++)
        if (pty[i]) {
          pretty = i;
          break;
        } // endif pty

    } // endif ptyp

  } catch (int n) {
    if (trace(1))
      htrc("Exception %d: %s\n", n, G->Message);
    GetMsg(g);
    bvp = NULL;
  } catch (const char* msg) {
    strcpy(g->Message, msg);
    bvp = NULL;
  } // end catch

  return bvp;
} // end of ParseJson

/***********************************************************************/
/* Parse several items as being in an array.                           */
/***********************************************************************/
OFFSET BDOC::ParseAsArray(size_t& i) {
  if (pty[0] && (!pretty || pretty > 2)) {
    OFFSET jsp;

    if ((jsp = ParseArray((i = 0))) && pretty == 3)
      pretty = (pty[0]) ? 0 : 3;

    return jsp;
  } else
    strcpy(G->Message, "More than one item in file");

  return 0;
} // end of ParseAsArray

/***********************************************************************/
/* Parse a JSON Array.                                                 */
/***********************************************************************/
OFFSET BDOC::ParseArray(size_t& i)
{
  int   level = 0;
  bool  b = (!i);
  PBVAL vlp, firstvlp, lastvlp;

  vlp = firstvlp = lastvlp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
    case ',':
      if (level < 2) {
        sprintf(G->Message, "Unexpected ',' near %.*s", (int) ARGS);
        throw 1;
      } else
        level = 1;

      break;
    case ']':
      if (level == 1) {
        sprintf(G->Message, "Unexpected ',]' near %.*s", (int) ARGS);
        throw 1;
      } // endif level

      return MOF(firstvlp);
    case '\n':
      if (!b)
        pty[0] = pty[1] = false;
    case '\r':
    case ' ':
    case '\t':
      break;
    default:
      if (level == 2) {
        sprintf(G->Message, "Unexpected value near %.*s", (int) ARGS);
        throw 1;
      } else if (lastvlp) {
        vlp = ParseValue(i, NewVal());
        lastvlp->Next = MOF(vlp);
        lastvlp = vlp;
      } else
        firstvlp = lastvlp = ParseValue(i, NewVal());

      level = (b) ? 1 : 2;
      break;
    }; // endswitch s[i]

  if (b) {
    // Case of Pretty == 0
    return MOF(firstvlp);
  } // endif b

  throw ("Unexpected EOF in array");
} // end of ParseArray

/***********************************************************************/
/* Parse a JSON Object.                                                */
/***********************************************************************/
OFFSET BDOC::ParseObject(size_t& i)
{
  OFFSET key;
  int    level = 0;
  PBPR   bpp, firstbpp, lastbpp;

  bpp = firstbpp = lastbpp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
    case '"':
      if (level < 2) {
        key = ParseString(++i);
        bpp = NewPair(key);

        if (lastbpp) {
          lastbpp->Vlp.Next = MOF(bpp);
          lastbpp = bpp;
        } else 
          firstbpp = lastbpp = bpp;

        level = 2;
      } else {
        sprintf(G->Message, "misplaced string near %.*s", (int) ARGS);
        throw 2;
      } // endif level

      break;
    case ':':
      if (level == 2) {
        ParseValue(++i, GetVlp(lastbpp));
        level = 3;
      } else {
        sprintf(G->Message, "Unexpected ':' near %.*s", (int) ARGS);
        throw 2;
      } // endif level

      break;
    case ',':
      if (level < 3) {
        sprintf(G->Message, "Unexpected ',' near %.*s", (int) ARGS);
        throw 2;
      } else
        level = 1;

      break;
    case '}':
      if (!(level == 0 || level == 3)) {
        sprintf(G->Message, "Unexpected '}' near %.*s", (int) ARGS);
        throw 2;
      } // endif level

      return MOF(firstbpp);
    case '\n':
      pty[0] = pty[1] = false;
    case '\r':
    case ' ':
    case '\t':
      break;
    default:
      sprintf(G->Message, "Unexpected character '%c' near %.*s",
        s[i], (int) ARGS);
      throw 2;
    }; // endswitch s[i]

  strcpy(G->Message, "Unexpected EOF in Object");
  throw 2;
} // end of ParseObject

/***********************************************************************/
/* Parse a JSON Value.                                                 */
/***********************************************************************/
PBVAL BDOC::ParseValue(size_t& i, PBVAL bvp)
{
  for (; i < len; i++)
    switch (s[i]) {
    case '\n':
      pty[0] = pty[1] = false;
    case '\r':
    case ' ':
    case '\t':
      break;
    default:
      goto suite;
    } // endswitch

suite:
  switch (s[i]) {
  case '[':
    bvp->To_Val = ParseArray(++i);
    bvp->Type = TYPE_JAR;
    break;
  case '{':
    bvp->To_Val = ParseObject(++i);
    bvp->Type = TYPE_JOB;
    break;
  case '"':
    bvp->To_Val = ParseString(++i);
    bvp->Type = TYPE_STRG;
    break;
  case 't':
    if (!strncmp(s + i, "true", 4)) {
      bvp->B = true;
      bvp->Type = TYPE_BOOL;
      i += 3;
    } else
      goto err;

    break;
  case 'f':
    if (!strncmp(s + i, "false", 5)) {
      bvp->B = false;
      bvp->Type = TYPE_BOOL;
      i += 4;
    } else
      goto err;

    break;
  case 'n':
    if (!strncmp(s + i, "null", 4)) {
      bvp->Type = TYPE_NULL;
      i += 3;
    } else
      goto err;

    break;
  case '-':
  default:
    if (s[i] == '-' || isdigit(s[i]))
      ParseNumeric(i, bvp);
    else
      goto err;

  }; // endswitch s[i]

  return bvp;

err:
  sprintf(G->Message, "Unexpected character '%c' near %.*s", s[i], (int) ARGS);
  throw 3;
} // end of ParseValue

/***********************************************************************/
/*  Unescape and parse a JSON string.                                  */
/***********************************************************************/
OFFSET BDOC::ParseString(size_t& i)
{
  uchar* p;
  int    n = 0;

  // Be sure of memory availability
  if (((size_t)len + 1 - i) > ((PPOOLHEADER)G->Sarea)->FreeBlk)
    throw("ParseString: Out of memory");

  // The size to allocate is not known yet
  p = (uchar*)BsonSubAlloc(0);

  for (; i < len; i++)
    switch (s[i]) {
    case '"':
      p[n++] = 0;
      BsonSubAlloc(n);
      return MOF(p);
    case '\\':
      if (++i < len) {
        if (s[i] == 'u') {
          if (len - i > 5) {
            //            if (charset == utf8) {
            char xs[5];
            uint hex;

            xs[0] = s[++i];
            xs[1] = s[++i];
            xs[2] = s[++i];
            xs[3] = s[++i];
            xs[4] = 0;
            hex = strtoul(xs, NULL, 16);

            if (hex < 0x80) {
              p[n] = (uchar)hex;
            } else if (hex < 0x800) {
              p[n++] = (uchar)(0xC0 | (hex >> 6));
              p[n] = (uchar)(0x80 | (hex & 0x3F));
            } else if (hex < 0x10000) {
              p[n++] = (uchar)(0xE0 | (hex >> 12));
              p[n++] = (uchar)(0x80 | ((hex >> 6) & 0x3f));
              p[n] = (uchar)(0x80 | (hex & 0x3f));
            } else
              p[n] = '?';

#if 0
          } else {
            char xs[3];
            UINT hex;

            i += 2;
            xs[0] = s[++i];
            xs[1] = s[++i];
            xs[2] = 0;
            hex = strtoul(xs, NULL, 16);
            p[n] = (char)hex;
          } // endif charset
#endif // 0
        } else
          goto err;

      } else switch (s[i]) {
      case 't': p[n] = '\t'; break;
      case 'n': p[n] = '\n'; break;
      case 'r': p[n] = '\r'; break;
      case 'b': p[n] = '\b'; break;
      case 'f': p[n] = '\f'; break;
      default:  p[n] = s[i]; break;
      } // endswitch

      n++;
    } else
      goto err;

    break;
      default:
        p[n++] = s[i];
        break;
}; // endswitch s[i]

err:
throw("Unexpected EOF in String");
} // end of ParseString

/***********************************************************************/
/* Parse a JSON numeric value.                                         */
/***********************************************************************/
void BDOC::ParseNumeric(size_t& i, PBVAL vlp)
{
  char  buf[50];
  int   n = 0;
  short nd = 0;
  bool  has_dot = false;
  bool  has_e = false;
  bool  found_digit = false;

  for (; i < len; i++) {
    switch (s[i]) {
    case '.':
      if (!found_digit || has_dot || has_e)
        goto err;

      has_dot = true;
      break;
    case 'e':
    case 'E':
      if (!found_digit || has_e)
        goto err;

      has_e = true;
      found_digit = false;
      break;
    case '+':
      if (!has_e)
        goto err;

      // fall through
    case '-':
      if (found_digit)
        goto err;

      break;
    default:
      if (isdigit(s[i])) {
        if (has_dot && !has_e)
          nd++;       // Number of decimals

        found_digit = true;
      } else
        goto fin;

    }; // endswitch s[i]

    buf[n++] = s[i];
  } // endfor i

fin:
  if (found_digit) {
    buf[n] = 0;

    if (has_dot || has_e) {
      double dv = atof(buf);

      if (nd >= 6 || dv > FLT_MAX || dv < FLT_MIN) {
        double* dvp = (double*)PlugSubAlloc(G, NULL, sizeof(double));

        *dvp = dv;
        vlp->To_Val = MOF(dvp);
        vlp->Type = TYPE_DBL;
      } else {
        vlp->F = (float)dv;
        vlp->Type = TYPE_FLOAT;
      } // endif nd

      vlp->Nd = MY_MIN(nd, 16);
    } else {
      longlong iv = strtoll(buf, NULL, 10);

      if (iv > INT_MAX32 || iv < INT_MIN32) {
        longlong *llp = (longlong*)PlugSubAlloc(G, NULL, sizeof(longlong));

        *llp = iv;
        vlp->To_Val = MOF(llp);
        vlp->Type = TYPE_BINT;
      } else {
        vlp->N = (int)iv;
        vlp->Type = TYPE_INTG;
      } // endif iv

    } // endif has

    i--;  // Unstack  following character
    return;
  } else
    throw("No digit found");

err:
  throw("Unexpected EOF in number");
} // end of ParseNumeric

/***********************************************************************/
/* Serialize a BJSON document tree:                                    */
/***********************************************************************/
PSZ BDOC::Serialize(PGLOBAL g, PBVAL bvp, char* fn, int pretty)
{
  PSZ   str = NULL;
  bool  b = false, err = true;
  FILE* fs = NULL;

  G->Message[0] = 0;

  try {
    if (!bvp) {
      strcpy(g->Message, "Null json tree");
      throw 1;
    } else if (!fn) {
      // Serialize to a string
      jp = new(g) JOUTSTR(g);
      b = pretty == 1;
    } else {
      if (!(fs = fopen(fn, "wb"))) {
        sprintf(g->Message, MSG(OPEN_MODE_ERROR),
          "w", (int)errno, fn);
        strcat(strcat(g->Message, ": "), strerror(errno));
        throw 2;
      } else if (pretty >= 2) {
        // Serialize to a pretty file
        jp = new(g)JOUTPRT(g, fs);
      } else {
        // Serialize to a flat file
        b = true;
        jp = new(g)JOUTFILE(g, fs, pretty);
      } // endif's

    } // endif's

    switch (bvp->Type) {
    case TYPE_JAR:
      err = SerializeArray(bvp->To_Val, b);
      break;
    case TYPE_JOB:
      err = ((b && jp->Prty()) && jp->WriteChr('\t'));
      err |= SerializeObject(bvp->To_Val);
      break;
    case TYPE_JVAL:
      err = SerializeValue(MVP(bvp->To_Val));
      break;
    default:
      err = SerializeValue(bvp, true);
    } // endswitch Type

    if (fs) {
      fputs(EL, fs);
      fclose(fs);
      str = (err) ? NULL : strcpy(g->Message, "Ok");
    } else if (!err) {
      str = ((JOUTSTR*)jp)->Strp;
      jp->WriteChr('\0');
      PlugSubAlloc(g, NULL, ((JOUTSTR*)jp)->N);
    } else if (G->Message[0])
        strcpy(g->Message, "Error in Serialize");
      else
        GetMsg(g);

  } catch (int n) {
    if (trace(1))
      htrc("Exception %d: %s\n", n, G->Message);
    GetMsg(g);
    str = NULL;
  } catch (const char* msg) {
    strcpy(g->Message, msg);
    str = NULL;
  } // end catch

  return str;
} // end of Serialize


/***********************************************************************/
/* Serialize a JSON Array.                                             */
/***********************************************************************/
bool BDOC::SerializeArray(OFFSET arp, bool b)
{
  bool  first = true;
  PBVAL vp = MVP(arp);

  if (b) {
    if (jp->Prty()) {
      if (jp->WriteChr('['))
        return true;
      else if (jp->Prty() == 1 && (jp->WriteStr(EL) || jp->WriteChr('\t')))
        return true;

    } // endif Prty

  } else if (jp->WriteChr('['))
    return true;

  for (; vp; vp = MVP(vp->Next)) {
    if (first)
      first = false;
    else if ((!b || jp->Prty()) && jp->WriteChr(','))
      return true;
    else if (b) {
      if (jp->Prty() < 2 && jp->WriteStr(EL))
        return true;
      else if (jp->Prty() == 1 && jp->WriteChr('\t'))
        return true;

    } // endif b

    if (SerializeValue(vp))
      return true;

  } // endfor vp

  if (b && jp->Prty() == 1 && jp->WriteStr(EL))
    return true;

  return ((!b || jp->Prty()) && jp->WriteChr(']'));
} // end of SerializeArray

/***********************************************************************/
/* Serialize a JSON Object.                                            */
/***********************************************************************/
bool BDOC::SerializeObject(OFFSET obp)
{
  bool first = true;
  PBPR prp = MPP(obp);

  if (jp->WriteChr('{'))
    return true;

  for (; prp; prp = GetNext(prp)) {
    if (first)
      first = false;
    else if (jp->WriteChr(','))
      return true;

    if (jp->WriteChr('"') ||
      jp->WriteStr(MZP(prp->Key)) ||
      jp->WriteChr('"') ||
      jp->WriteChr(':') ||
      SerializeValue(GetVlp(prp)))
      return true;

  } // endfor i

  return jp->WriteChr('}');
} // end of SerializeObject

/***********************************************************************/
/* Serialize a JSON Value.                                             */
/***********************************************************************/
bool BDOC::SerializeValue(PBVAL jvp, bool b)
{
  char buf[64];

  if (jvp) switch (jvp->Type) {
  case TYPE_JAR:
    return SerializeArray(jvp->To_Val, false);
  case TYPE_JOB:
    return SerializeObject(jvp->To_Val);
  case TYPE_BOOL:
    return jp->WriteStr(jvp->B ? "true" : "false");
  case TYPE_STRG:
  case TYPE_DTM:
    if (b) {
      return jp->WriteStr(MZP(jvp->To_Val));
    } else
      return jp->Escape(MZP(jvp->To_Val));

  case TYPE_INTG:
    sprintf(buf, "%d", jvp->N);
    return jp->WriteStr(buf);
  case TYPE_BINT:
    sprintf(buf, "%lld", *(longlong*)MakePtr(Base, jvp->To_Val));
    return jp->WriteStr(buf);
  case TYPE_FLOAT:
    sprintf(buf, "%.*f", jvp->Nd, jvp->F);
    return jp->WriteStr(buf);
  case TYPE_DBL:
    sprintf(buf, "%.*lf", jvp->Nd, *(double*)MakePtr(Base, jvp->To_Val));
    return jp->WriteStr(buf);
  case TYPE_NULL:
    return jp->WriteStr("null");
  case TYPE_JVAL:
    return SerializeValue(MVP(jvp->To_Val));
  default:
    return jp->WriteStr("???");   // TODO
  } // endswitch Type

  return  jp->WriteStr("null");
} // end of SerializeValue

/* --------------------------- Class BJSON --------------------------- */

/***********************************************************************/
/*  Program for sub-allocating Bjson structures.                       */
/***********************************************************************/
void* BJSON::BsonSubAlloc(size_t size)
{
  PPOOLHEADER pph;                           /* Points on area header. */
  void* memp = G->Sarea;

  size = ((size + 3) / 4) * 4;       /* Round up size to multiple of 4 */
  pph = (PPOOLHEADER)memp;

  xtrc(16, "SubAlloc in %p size=%zd used=%zd free=%zd\n",
    memp, size, pph->To_Free, pph->FreeBlk);

  if (size > pph->FreeBlk) {   /* Not enough memory left in pool */
    sprintf(G->Message,
      "Not enough memory for request of %zd (used=%zd free=%zd)",
      size, pph->To_Free, pph->FreeBlk);
    xtrc(1, "BsonSubAlloc: %s\n", G->Message);

    if (Throw)
      throw(1234);
    else
      return NULL;

  } /* endif size OS32 code */

  // Do the suballocation the simplest way
  memp = MakePtr(memp, pph->To_Free); /* Points to suballocated block  */
  pph->To_Free += size;               /* New offset of pool free block */
  pph->FreeBlk -= size;               /* New size   of pool free block */
  xtrc(16, "Done memp=%p used=%zd free=%zd\n",
    memp, pph->To_Free, pph->FreeBlk);
  return memp;
} // end of BsonSubAlloc

/*********************************************************************************/
/*  Program for SubSet re-initialization of the memory pool.                     */
/*********************************************************************************/
PSZ BJSON::NewStr(PSZ str)
{
  if (str) {
    PSZ sm = (PSZ)BsonSubAlloc(strlen(str) + 1);

    strcpy(sm, str);
    return sm;
  } else
    return NULL;

} // end of NewStr

/*********************************************************************************/
/*  Program for SubSet re-initialization of the memory pool.                     */
/*********************************************************************************/
void BJSON::SubSet(bool b)
{
  PPOOLHEADER pph = (PPOOLHEADER)G->Sarea;

  pph->To_Free = (G->Saved_Size) ? G->Saved_Size : sizeof(POOLHEADER);
  pph->FreeBlk = G->Sarea_Size - pph->To_Free;

  if (b)
    G->Saved_Size = 0;

} // end of SubSet

/*********************************************************************************/
/*  Set the beginning of suballocations.                                         */
/*********************************************************************************/
void BJSON::MemSet(size_t size)
{
  PPOOLHEADER pph = (PPOOLHEADER)G->Sarea;

  pph->To_Free = size + sizeof(POOLHEADER);
  pph->FreeBlk = G->Sarea_Size - pph->To_Free;
} // end of MemSet

  /* ------------------------ Bobject functions ------------------------ */

/***********************************************************************/
/* Set a pair vlp to some PVAL values.                                 */
/***********************************************************************/
void BJSON::SetPairValue(PBPR brp, PBVAL bvp)
{
  if (bvp) {
    brp->Vlp.To_Val = bvp->To_Val;
    brp->Vlp.Nd = bvp->Nd;
    brp->Vlp.Type = bvp->Type;
  } else {
    brp->Vlp.To_Val = 0;
    brp->Vlp.Nd = 0;
    brp->Vlp.Type = TYPE_NULL;
  } // endif bvp

} // end of SetPairValue

  /***********************************************************************/
/* Sub-allocate and initialize a BPAIR.                                */
/***********************************************************************/
PBPR BJSON::NewPair(OFFSET key, int type)
{
  PBPR bpp = (PBPR)BsonSubAlloc(sizeof(BPAIR));

  bpp->Key = key;
  bpp->Vlp.Type = type;
  bpp->Vlp.To_Val = 0;
  bpp->Vlp.Nd = 0;
  bpp->Vlp.Next = 0;
  return bpp;
} // end of SubAllocPair

/***********************************************************************/
/* Return the number of pairs in this object.                          */
/***********************************************************************/
int BJSON::GetObjectSize(PBVAL bop, bool b)
{
  CheckType(bop, TYPE_JOB);
  int n = 0;

  for (PBPR brp = GetObject(bop); brp; brp = GetNext(brp))
    // If b return only non null pairs
    if (!b || (brp->Vlp.To_Val && brp->Vlp.Type != TYPE_NULL))
      n++;

  return n;
} // end of GetObjectSize

/***********************************************************************/
/* Add a new pair to an Object and return it.                          */
/***********************************************************************/
PBVAL BJSON::AddPair(PBVAL bop, PSZ key, int type)
{
  CheckType(bop, TYPE_JOB);
  PBPR   brp;
  OFFSET nrp = NewPair(key, type);

  if (bop->To_Val) {
    for (brp = GetObject(bop); brp->Vlp.Next; brp = GetNext(brp));

    brp->Vlp.Next = nrp;
  } else
    bop->To_Val = nrp;

  bop->Nd++;
  return GetVlp(MPP(nrp));
} // end of AddPair

/***********************************************************************/
/* Return all object keys as an array.                                 */
/***********************************************************************/
PBVAL BJSON::GetKeyList(PBVAL bop)
{
  CheckType(bop, TYPE_JOB);
  PBVAL arp = NewVal(TYPE_JAR);

  for (PBPR brp = GetObject(bop); brp; brp = GetNext(brp))
    AddArrayValue(arp, MOF(SubAllocVal(brp->Key, TYPE_STRG)));

  return arp;
} // end of GetKeyList

/***********************************************************************/
/* Return all object values as an array.                               */
/***********************************************************************/
PBVAL BJSON::GetObjectValList(PBVAL bop)
{
  CheckType(bop, TYPE_JOB);
  PBVAL arp = NewVal(TYPE_JAR);

  for (PBPR brp = GetObject(bop); brp; brp = GetNext(brp))
    AddArrayValue(arp, DupVal(GetVlp(brp)));

  return arp;
} // end of GetObjectValList

/***********************************************************************/
/* Get the value corresponding to the given key.                       */
/***********************************************************************/
PBVAL BJSON::GetKeyValue(PBVAL bop, PSZ key)
{
  CheckType(bop, TYPE_JOB);

  for (PBPR brp = GetObject(bop); brp; brp = GetNext(brp))
    if (!strcmp(GetKey(brp), key))
      return GetVlp(brp);

  return NULL;
} // end of GetKeyValue;

/***********************************************************************/
/* Return the text corresponding to all keys (XML like).               */
/***********************************************************************/
PSZ BJSON::GetObjectText(PGLOBAL g, PBVAL bop, PSTRG text)
{
  CheckType(bop, TYPE_JOB);
  PBPR brp = GetObject(bop);

  if (brp) {
    bool b;

    if (!text) {
      text = new(g) STRING(g, 256);
      b = true;
    } else {
      if (text->GetLastChar() != ' ')
        text->Append(' ');

      b = false;
    }	// endif text

    if (b && !brp->Vlp.Next && !strcmp(MZP(brp->Key), "$date")) {
      int i;
      PSZ s;

      GetValueText(g, GetVlp(brp), text);
      s = text->GetStr();
      i = (s[1] == '-' ? 2 : 1);

      if (IsNum(s + i)) {
        // Date is in milliseconds
        int j = text->GetLength();

        if (j >= 4 + i) {
          s[j - 3] = 0;        // Change it to seconds
          text->SetLength((uint)strlen(s));
        } else
          text->Set(" 0");

      } // endif text

    } else for (; brp; brp = GetNext(brp)) {
      GetValueText(g, GetVlp(brp), text);

      if (brp->Vlp.Next)
        text->Append(' ');

    }	// endfor brp

    if (b) {
      text->Trim();
      return text->GetStr();
    }	// endif b

  } // endif bop

  return NULL;
} // end of GetObjectText;

/***********************************************************************/
/* Set or add a value corresponding to the given key.                  */
/***********************************************************************/
void BJSON::SetKeyValue(PBVAL bop, OFFSET bvp, PSZ key)
{
  CheckType(bop, TYPE_JOB);
  PBPR brp, prp = NULL;

  if (bop->To_Val) {
    for (brp = GetObject(bop); brp; brp = GetNext(brp))
      if (!strcmp(GetKey(brp), key))
        break;
      else
        prp = brp;

    if (!brp)
      brp = MPP(prp->Vlp.Next = NewPair(key));

  } else
    brp = MPP(bop->To_Val = NewPair(key));

  SetPairValue(brp, MVP(bvp));
  bop->Nd++;
} // end of SetKeyValue

/***********************************************************************/
/* Merge two objects.                                                  */
/***********************************************************************/
PBVAL BJSON::MergeObject(PBVAL bop1, PBVAL bop2)
{
  CheckType(bop1, TYPE_JOB);
  CheckType(bop2, TYPE_JOB);

  if (bop1->To_Val)
    for (PBPR brp = GetObject(bop2); brp; brp = GetNext(brp))
      SetKeyValue(bop1, GetVlp(brp), GetKey(brp));

  else {
    bop1->To_Val = bop2->To_Val;
    bop1->Nd = bop2->Nd;
  } // endelse To_Val

  return bop1;
} // end of MergeObject;

/***********************************************************************/
/* Delete a value corresponding to the given key.                      */
/***********************************************************************/
bool BJSON::DeleteKey(PBVAL bop, PCSZ key)
{
  CheckType(bop, TYPE_JOB);
  PBPR brp, pbrp = NULL;

  for (brp = GetObject(bop); brp; brp = GetNext(brp))
    if (!strcmp(MZP(brp->Key), key)) {
      if (pbrp) {
        pbrp->Vlp.Next = brp->Vlp.Next;
      } else
        bop->To_Val = brp->Vlp.Next;

      bop->Nd--;
      return true;;
    } else
      pbrp = brp;

  return false;
} // end of DeleteKey

/***********************************************************************/
/* True if void or if all members are nulls.                           */
/***********************************************************************/
bool BJSON::IsObjectNull(PBVAL bop)
{
  CheckType(bop, TYPE_JOB);

  for (PBPR brp = GetObject(bop); brp; brp = GetNext(brp))
    if (brp->Vlp.To_Val && brp->Vlp.Type != TYPE_NULL)
      return false;

  return true;
} // end of IsObjectNull

/* ------------------------- Barray functions ------------------------ */

/***********************************************************************/
/* Return the number of values in this object.                         */
/***********************************************************************/
int BJSON::GetArraySize(PBVAL bap, bool b)
{
  CheckType(bap, TYPE_JAR);
  int n = 0;

  for (PBVAL bvp = GetArray(bap); bvp; bvp = GetNext(bvp))
    //  If b, return only non null values
    if (!b || bvp->Type != TYPE_NULL)
      n++;

  return n;
} // end of GetArraySize

/***********************************************************************/
/* Get the Nth value of an Array.                                      */
/***********************************************************************/
PBVAL BJSON::GetArrayValue(PBVAL bap, int n)
{
  CheckType(bap, TYPE_JAR);
  int i = 0;

  if (n < 0)
    n += GetArraySize(bap);

  for (PBVAL bvp = GetArray(bap); bvp; bvp = GetNext(bvp), i++)
    if (i == n)
      return bvp;

  return NULL;
} // end of GetArrayValue

/***********************************************************************/
/* Add a Value to the Array Value list.                                */
/***********************************************************************/
void BJSON::AddArrayValue(PBVAL bap, OFFSET nbv, int* x)
{
  CheckType(bap, TYPE_JAR);
  int   i = 0;
  PBVAL bvp, lbp = NULL;

  if (!nbv)
    nbv = MOF(NewVal());

  for (bvp = GetArray(bap); bvp; bvp = GetNext(bvp), i++)
    if (x && i == *x)
      break;
    else
      lbp = bvp;

  if (lbp) {
    MVP(nbv)->Next = lbp->Next;
    lbp->Next = nbv;
  } else {
    MVP(nbv)->Next = bap->To_Val;
    bap->To_Val = nbv;
  } // endif lbp

  bap->Nd++;
} // end of AddArrayValue

/***********************************************************************/
/* Merge two arrays.                                                   */
/***********************************************************************/
void BJSON::MergeArray(PBVAL bap1, PBVAL bap2)
{
  CheckType(bap1, TYPE_JAR);
  CheckType(bap2, TYPE_JAR);

  if (bap1->To_Val) {
    for (PBVAL bvp = GetArray(bap2); bvp; bvp = GetNext(bvp))
      AddArrayValue(bap1, MOF(DupVal(bvp)));

  } else {
    bap1->To_Val = bap2->To_Val;
    bap1->Nd = bap2->Nd;
  } // endif To_Val

} // end of MergeArray

/***********************************************************************/
/* Set the nth Value of the Array Value list or add it.                */
/***********************************************************************/
void BJSON::SetArrayValue(PBVAL bap, PBVAL nvp, int n)
{
  CheckType(bap, TYPE_JAR);
  int   i = 0;
  PBVAL bvp = NULL;

  for (bvp = GetArray(bap); i < n; i++, bvp = bvp ? GetNext(bvp) : NULL)
    if (!bvp)
      AddArrayValue(bap, NewVal());

  if (!bvp)
    AddArrayValue(bap, MOF(nvp));
  else
    SetValueVal(bvp, nvp);

} // end of SetValue

/***********************************************************************/
/* Return the text corresponding to all values.                        */
/***********************************************************************/
PSZ BJSON::GetArrayText(PGLOBAL g, PBVAL bap, PSTRG text)
{
  CheckType(bap, TYPE_JAR);

  if (bap->To_Val) {
    bool  b;

    if (!text) {
      text = new(g) STRING(g, 256);
      b = true;
    } else {
      if (text->GetLastChar() != ' ')
        text->Append(" (");
      else
        text->Append('(');

      b = false;
    } // endif text

    for (PBVAL bvp = GetArray(bap); bvp; bvp = GetNext(bvp)) {
      GetValueText(g, bvp, text);

      if (bvp->Next)
        text->Append(", ");
      else if (!b)
        text->Append(')');

    }	// endfor bvp

    if (b) {
      text->Trim();
      return text->GetStr();
    }	// endif b

  } // endif To_Val

  return NULL;
} // end of GetText;

/***********************************************************************/
/* Delete a Value from the Arrays Value list.                          */
/***********************************************************************/
bool BJSON::DeleteValue(PBVAL bap, int n)
{
  CheckType(bap, TYPE_JAR);
  int   i = 0;
  PBVAL bvp, pvp = NULL;

  for (bvp = GetArray(bap); bvp; i++, bvp = GetNext(bvp))
    if (i == n) {
      if (pvp)
        pvp->Next = bvp->Next;
      else
        bap->To_Val = bvp->Next;

      bap->Nd--;
      return true;;
    } else
      pvp = bvp;

  return false;
} // end of DeleteValue

/***********************************************************************/
/* True if void or if all members are nulls.                           */
/***********************************************************************/
bool BJSON::IsArrayNull(PBVAL bap)
{
  CheckType(bap, TYPE_JAR);

  for (PBVAL bvp = GetArray(bap); bvp; bvp = GetNext(bvp))
    if (bvp->Type != TYPE_NULL)
      return false;

  return true;
} // end of IsNull

/* ------------------------- Bvalue functions ------------------------ */

/***********************************************************************/
/* Sub-allocate and clear a BVAL.                                      */
/***********************************************************************/
PBVAL BJSON::NewVal(int type)
{
  PBVAL bvp = (PBVAL)BsonSubAlloc(sizeof(BVAL));

  bvp->To_Val = 0;
  bvp->Nd = 0;
  bvp->Type = type;
  bvp->Next = 0;
  return bvp;
} // end of SubAllocVal

/***********************************************************************/
/* Sub-allocate and initialize a BVAL as type.                         */
/***********************************************************************/
PBVAL BJSON::SubAllocVal(OFFSET toval, int type, short nd)
{
  PBVAL bvp = NewVal(type);

  bvp->To_Val = toval;
  bvp->Nd = nd;
  return bvp;
} // end of SubAllocVal

/***********************************************************************/
/* Sub-allocate and initialize a BVAL as string.                       */
/***********************************************************************/
PBVAL BJSON::SubAllocStr(OFFSET toval, short nd)
{
  PBVAL bvp = NewVal(TYPE_STRG);

  bvp->To_Val = toval;
  bvp->Nd = nd;
  return bvp;
} // end of SubAllocStr

/***********************************************************************/
/* Allocate a BVALUE with a given string or numeric value.             */
/***********************************************************************/
PBVAL BJSON::NewVal(PVAL valp)
{
  PBVAL vlp = NewVal();

  SetValue(vlp, valp);
  return vlp;
} // end of SubAllocVal

/***********************************************************************/
/* Sub-allocate and initialize a BVAL from another BVAL.               */
/***********************************************************************/
PBVAL BJSON::DupVal(PBVAL bvlp)
{
  if (bvlp) {
    PBVAL bvp = NewVal();

    *bvp = *bvlp;
    bvp->Next = 0;
    return bvp;
  } else
    return NULL;

} // end of DupVal

/***********************************************************************/
/* Return the size of value's value.                                   */
/***********************************************************************/
int BJSON::GetSize(PBVAL vlp, bool b)
{
  switch (vlp->Type) {
  case TYPE_JAR:
    return GetArraySize(vlp);
  case TYPE_JOB:
    return GetObjectSize(vlp);
  default:
    return 1;
  } // enswitch Type

} // end of GetSize

PBVAL BJSON::GetBson(PBVAL bvp)
{ 
  PBVAL bp = NULL;

  switch (bvp->Type) {
    case TYPE_JAR:
      bp = MVP(bvp->To_Val);
      break;
    case TYPE_JOB:
      bp = GetVlp(MPP(bvp->To_Val));
      break;
    default:
      bp = bvp;
      break;
  } // endswitch Type

  return bp;
} // end of GetBson

/***********************************************************************/
/* Return the Value's as a Value struct.                               */
/***********************************************************************/
PVAL BJSON::GetValue(PGLOBAL g, PBVAL vp)
{
  double d;
  PVAL   valp;
  PBVAL  vlp = vp->Type == TYPE_JVAL ? MVP(vp->To_Val) : vp;

  switch (vlp->Type) {
    case TYPE_STRG:
    case TYPE_DBL:
    case TYPE_BINT:
      valp = AllocateValue(g, MP(vlp->To_Val), vlp->Type, vlp->Nd);
      break;
    case TYPE_INTG:
    case TYPE_BOOL:
      valp = AllocateValue(g, vlp, vlp->Type);
      break;
    case TYPE_FLOAT:
      d = (double)vlp->F;
      valp = AllocateValue(g, &d, TYPE_DOUBLE, vlp->Nd);
      break;
    default:
      valp = NULL;
      break;
  } // endswitch Type

  return valp;
} // end of GetValue

/***********************************************************************/
/* Return the Value's Integer value.                                   */
/***********************************************************************/
int BJSON::GetInteger(PBVAL vp) {
  int   n;
  PBVAL vlp = (vp->Type == TYPE_JVAL) ? MVP(vp->To_Val) : vp;

  switch (vlp->Type) {
  case TYPE_INTG:
    n = vlp->N;
    break;
  case TYPE_FLOAT:
    n = (int)vlp->F;
    break;
  case TYPE_DTM:
  case TYPE_STRG:
    n = atoi(MZP(vlp->To_Val));
    break;
  case TYPE_BOOL:
    n = (vlp->B) ? 1 : 0;
    break;
  case TYPE_BINT: 
    n = (int)*(longlong*)MP(vlp->To_Val);
    break;
  case TYPE_DBL:
    n = (int)*(double*)MP(vlp->To_Val);
    break;
  default:
    n = 0;
  } // endswitch Type

  return n;
} // end of GetInteger

/***********************************************************************/
/* Return the Value's Big integer value.                               */
/***********************************************************************/
longlong BJSON::GetBigint(PBVAL vp) {
  longlong lln;
  PBVAL vlp = (vp->Type == TYPE_JVAL) ? MVP(vp->To_Val) : vp;

  switch (vlp->Type) {
  case TYPE_BINT: 
    lln = *(longlong*)MP(vlp->To_Val);
    break;
  case TYPE_INTG: 
    lln = (longlong)vlp->N;
    break;
  case TYPE_FLOAT: 
    lln = (longlong)vlp->F;
    break;
  case TYPE_DBL:
    lln = (longlong)*(double*)MP(vlp->To_Val);
    break;
  case TYPE_DTM:
  case TYPE_STRG: 
    lln = atoll(MZP(vlp->To_Val));
    break;
  case TYPE_BOOL:
    lln = (vlp->B) ? 1 : 0;
    break;
  default:
    lln = 0;
  } // endswitch Type

  return lln;
} // end of GetBigint

/***********************************************************************/
/* Return the Value's Double value.                                    */
/***********************************************************************/
double BJSON::GetDouble(PBVAL vp)
{
  double d;
  PBVAL vlp = (vp->Type == TYPE_JVAL) ? MVP(vp->To_Val) : vp;

  switch (vlp->Type) {
    case TYPE_DBL:
      d = *(double*)MP(vlp->To_Val);
      break;
    case TYPE_BINT:
      d = (double)*(longlong*)MP(vlp->To_Val);
      break;
    case TYPE_INTG:
      d = (double)vlp->N;
      break;
    case TYPE_FLOAT:
      d = (double)vlp->F;
      break;
    case TYPE_DTM:
    case TYPE_STRG:
      d = atof(MZP(vlp->To_Val));
      break;
    case TYPE_BOOL:
      d = (vlp->B) ? 1.0 : 0.0;
      break;
    default:
      d = 0.0;
  } // endswitch Type

  return d;
} // end of GetDouble

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ BJSON::GetString(PBVAL vp, char* buff)
{
  char  buf[32];
  char* p = (buff) ? buff : buf;
  PBVAL vlp = (vp->Type == TYPE_JVAL) ? MVP(vp->To_Val) : vp;

  switch (vlp->Type) {
  case TYPE_DTM:
  case TYPE_STRG:
    p = MZP(vlp->To_Val);
    break;
  case TYPE_INTG:
    sprintf(p, "%d", vlp->N);
    break;
  case TYPE_FLOAT:
    sprintf(p, "%.*f", vlp->Nd, vlp->F);
    break;
  case TYPE_BINT:
    sprintf(p, "%lld", *(longlong*)MP(vlp->To_Val));
    break;
  case TYPE_DBL:
    sprintf(p, "%.*lf", vlp->Nd, *(double*)MP(vlp->To_Val));
    break;
  case TYPE_BOOL:
    p = (PSZ)((vlp->B) ? "true" : "false");
    break;
  case TYPE_NULL:
    p = (PSZ)"null";
    break;
  default:
    p = NULL;
  } // endswitch Type

  return (p == buf) ? (PSZ)PlugDup(G, buf) : p;
} // end of GetString

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ BJSON::GetValueText(PGLOBAL g, PBVAL vlp, PSTRG text)
{
  if (vlp->Type == TYPE_JOB)
    return GetObjectText(g, vlp, text);
  else if (vlp->Type == TYPE_JAR)
    return GetArrayText(g, vlp, text);

  char buff[32];
  PSZ  s = (vlp->Type == TYPE_NULL) ? NULL : GetString(vlp, buff);

  if (s)
    text->Append(s);
  else if (GetJsonNull())
    text->Append(GetJsonNull());

  return NULL;
} // end of GetText

void BJSON::SetValueObj(PBVAL vlp, PBVAL bop)
{
  CheckType(bop, TYPE_JOB);
  vlp->To_Val = bop->To_Val;
  vlp->Nd = bop->Nd;
  vlp->Type = TYPE_JOB;
} // end of SetValueObj;

void BJSON::SetValueArr(PBVAL vlp, PBVAL bap)
{
  CheckType(bap, TYPE_JAR);
  vlp->To_Val = bap->To_Val;
  vlp->Nd = bap->Nd;
  vlp->Type = TYPE_JAR;
} // end of SetValue;

void BJSON::SetValueVal(PBVAL vlp, PBVAL vp)
{
  vlp->To_Val = vp->To_Val;
  vlp->Nd = vp->Nd;
  vlp->Type = vp->Type;
} // end of SetValue;

PBVAL BJSON::SetValue(PBVAL vlp, PVAL valp)
{
  if (!vlp)
    vlp = NewVal();

  if (!valp || valp->IsNull()) {
    vlp->Type = TYPE_NULL;
  } else switch (valp->GetType()) {
    case TYPE_DATE:
      if (((DTVAL*)valp)->IsFormatted())
        vlp->To_Val = DupStr(valp->GetCharValue());
      else {
        char buf[32];

        vlp->To_Val = DupStr(valp->GetCharString(buf));
      }	// endif Formatted

      vlp->Type = TYPE_DTM;
      break;
    case TYPE_STRING:
      vlp->To_Val = DupStr(valp->GetCharValue());
      vlp->Type = TYPE_STRG;
      break;
    case TYPE_DOUBLE:
    case TYPE_DECIM:
    { double d = valp->GetFloatValue();
      int    nd = (IsTypeNum(valp->GetType())) ? valp->GetValPrec() : 0;

      if (nd > 0 && nd <= 6 && d >= FLT_MIN && d <= FLT_MAX) {
        vlp->F = (float)valp->GetFloatValue();
        vlp->Type = TYPE_FLOAT;
      } else {
        double* dp = (double*)BsonSubAlloc(sizeof(double));

        *dp = d;
        vlp->To_Val = MOF(dp);
        vlp->Type = TYPE_DBL;
      } // endif Nd

      vlp->Nd = MY_MIN(nd, 16);
    } break;
    case TYPE_TINY:
      vlp->B = valp->GetTinyValue() != 0;
      vlp->Type = TYPE_BOOL;
      break;
    case TYPE_INT:
      vlp->N = valp->GetIntValue();
      vlp->Type = TYPE_INTG;
      break;
    case TYPE_BIGINT:
      if (valp->GetBigintValue() >= INT_MIN32 &&
        valp->GetBigintValue() <= INT_MAX32) {
        vlp->N = valp->GetIntValue();
        vlp->Type = TYPE_INTG;
      } else {
        longlong* llp = (longlong*)BsonSubAlloc(sizeof(longlong));

        *llp = valp->GetBigintValue();
        vlp->To_Val = MOF(llp);
        vlp->Type = TYPE_BINT;
      } // endif BigintValue

      break;
    default:
      sprintf(G->Message, "Unsupported typ %d\n", valp->GetType());
      throw(777);
  } // endswitch Type

  return vlp;
} // end of SetValue

/***********************************************************************/
/* Set the Value's value as the given integer.                         */
/***********************************************************************/
void BJSON::SetInteger(PBVAL vlp, int n)
{
  vlp->N = n;
  vlp->Type = TYPE_INTG;
} // end of SetInteger

/***********************************************************************/
/* Set the Value's Boolean value as a tiny integer.                    */
/***********************************************************************/
void BJSON::SetBool(PBVAL vlp, bool b)
{
  vlp->B = b;
  vlp->Type = TYPE_BOOL;
} // end of SetTiny

/***********************************************************************/
/* Set the Value's value as the given big integer.                     */
/***********************************************************************/
void BJSON::SetBigint(PBVAL vlp, longlong ll)
{
  if (ll >= INT_MIN32 && ll <= INT_MAX32) {
    vlp->N = (int)ll;
    vlp->Type = TYPE_INTG;
  } else {
    longlong* llp = (longlong*)PlugSubAlloc(G, NULL, sizeof(longlong));

    *llp = ll;
    vlp->To_Val = MOF(llp);
    vlp->Type = TYPE_BINT;
  } // endif ll

} // end of SetBigint

/***********************************************************************/
/* Set the Value's value as the given DOUBLE.                          */
/***********************************************************************/
void BJSON::SetFloat(PBVAL vlp, double d, int prec)
{
  int nd = MY_MIN((prec < 0) ? GetJsonDefPrec() : prec, 16);

  if (nd < 6 && d >= FLT_MIN && d <= FLT_MAX) {
    vlp->F = (float)d;
    vlp->Type = TYPE_FLOAT;
  } else {
    double* dp = (double*)BsonSubAlloc(sizeof(double));

    *dp = d;
    vlp->To_Val = MOF(dp);
    vlp->Type = TYPE_DBL;
  } // endif nd

  vlp->Nd = nd;
} // end of SetFloat

/***********************************************************************/
/* Set the Value's value as the given DOUBLE representation.                          */
/***********************************************************************/
void BJSON::SetFloat(PBVAL vlp, PSZ s)
{
  char  *p = strchr(s, '.');
  int    nd = 0;
  double d = atof(s);

  if (p) {
    for (++p; isdigit(*p); nd++, p++);
    for (--p; *p == '0'; nd--, p--);
  } // endif p

  SetFloat(vlp, d, nd);
} // end of SetFloat

 /***********************************************************************/
/* Set the Value's value as the given string.                          */
/***********************************************************************/
void BJSON::SetString(PBVAL vlp, PSZ s, int ci)
{
  vlp->To_Val = MOF(s);
  vlp->Nd = ci;
  vlp->Type = TYPE_STRG;
} // end of SetString

/***********************************************************************/
/* True when its JSON or normal value is null.                         */
/***********************************************************************/
bool BJSON::IsValueNull(PBVAL vlp)
{
  bool b;

  switch (vlp->Type) {
  case TYPE_NULL:
    b = true;
    break;
  case TYPE_JOB:
    b = IsObjectNull(vlp);
    break;
  case TYPE_JAR:
    b = IsArrayNull(vlp);
    break;
  default:
    b = false;
  } // endswitch Type

  return b;
  } // end of IsNull
