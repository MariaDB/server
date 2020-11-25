/*************** json CPP Declares Source Code File (.H) ***************/
/*  Name: json.cpp   Version 1.5                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2020  */
/*                                                                     */
/*  This file contains the JSON classes functions.                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  xjson.h     is header containing the JSON classes declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "bson.h"

#if defined(__WIN__)
#define EL  "\r\n"
#else
#define EL  "\n"
#undef     SE_CATCH                  // Does not work for Linux
#endif

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

#if 0
char* GetJsonNull(void);

/***********************************************************************/
/* IsNum: check whether this string is all digits.                     */
/***********************************************************************/
bool IsNum(PSZ s) {
  for (char* p = s; *p; p++)
    if (*p == ']')
      break;
    else if (!isdigit(*p) || *p == '-')
      return false;

  return true;
} // end of IsNum

/***********************************************************************/
/* NextChr: return the first found '[' or Sep pointer.                 */
/***********************************************************************/
char* NextChr(PSZ s, char sep) {
  char* p1 = strchr(s, '[');
  char* p2 = strchr(s, sep);

  if (!p2)
    return p1;
  else if (p1)
    return MY_MIN(p1, p2);

  return p2;
} // end of NextChr
#endif // 0

/* --------------------------- Class BDOC ---------------------------- */

/***********************************************************************/
/*  BDOC constructor.                                                  */
/***********************************************************************/
BDOC::BDOC(void *base) : BJSON(base, NULL)
{ 
  jp = NULL;
  s = NULL;
  len = 0;
  pty[0] = pty[1] = pty[2] = true;
} // end of BDOC constructor

/***********************************************************************/
/* Parse a json string.                                                */
/* Note: when pretty is not known, the caller set pretty to 3.         */
/***********************************************************************/
PBVAL BDOC::ParseJson(PGLOBAL g, char* js, size_t lng, int* ptyp, bool* comma) {
  int   i, pretty = (ptyp) ? *ptyp : 3;
  bool  b = false;

  s = js;
  len = lng;
  xtrc(1, "BDOC::ParseJson: s=%.10s len=%zd\n", s, len);

  if (!s || !len) {
    strcpy(g->Message, "Void JSON object");
    return NULL;
  } else if (comma)
    *comma = false;

  // Trying to guess the pretty format
  if (s[0] == '[' && (s[1] == '\n' || (s[1] == '\r' && s[2] == '\n')))
    pty[0] = false;

  try {
    Bvp = SubAllocVal(g);
    Bvp->Type = TYPE_UNKNOWN;

    for (i = 0; i < len; i++)
      switch (s[i]) {
      case '[':
        if (Bvp->Type != TYPE_UNKNOWN)
          Bvp->To_Val = ParseAsArray(g, i, pretty, ptyp);
        else
          Bvp->To_Val = ParseArray(g, ++i);

        Bvp->Type = TYPE_JAR;
        break;
      case '{':
        if (Bvp->Type != TYPE_UNKNOWN) {
          Bvp->To_Val = ParseAsArray(g, i, pretty, ptyp);
          Bvp->Type = TYPE_JAR;
        } else if ((Bvp->To_Val = ParseObject(g, ++i)))
          Bvp->Type = TYPE_JOB;
        else
          throw 2;

        break;
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        break;
      case ',':
        if (Bvp->Type != TYPE_UNKNOWN && (pretty == 1 || pretty == 3)) {
          if (comma)
            *comma = true;

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

      default:
        if (Bvp->Type != TYPE_UNKNOWN) {
          Bvp->To_Val = ParseAsArray(g, i, pretty, ptyp);
          Bvp->Type = TYPE_JAR;
        } else if ((Bvp->To_Val = MOF(ParseValue(g, i))))
          Bvp->Type = TYPE_JVAL;
        else
          throw 4;

        break;
      }; // endswitch s[i]

    if (Bvp->Type == TYPE_UNKNOWN)
      sprintf(g->Message, "Invalid Json string '%.*s'", MY_MIN((int)len, 50), s);
    else if (ptyp && pretty == 3) {
      *ptyp = 3;     // Not recognized pretty

      for (i = 0; i < 3; i++)
        if (pty[i]) {
          *ptyp = i;
          break;
        } // endif pty

    } // endif ptyp

  } catch (int n) {
    if (trace(1))
      htrc("Exception %d: %s\n", n, g->Message);
    Bvp = NULL;
  } catch (const char* msg) {
    strcpy(g->Message, msg);
    Bvp = NULL;
  } // end catch

  return Bvp;
} // end of ParseJson

/***********************************************************************/
/* Parse several items as being in an array.                           */
/***********************************************************************/
OFFSET BDOC::ParseAsArray(PGLOBAL g, int& i, int pretty, int* ptyp) {
  if (pty[0] && (!pretty || pretty > 2)) {
    OFFSET jsp;

    if ((jsp = ParseArray(g, (i = 0))) && ptyp && pretty == 3)
      *ptyp = (pty[0]) ? 0 : 3;

    return jsp;
  } else
    strcpy(g->Message, "More than one item in file");

  return 0;
} // end of ParseAsArray

/***********************************************************************/
/* Parse a JSON Array.                                                 */
/***********************************************************************/
OFFSET BDOC::ParseArray(PGLOBAL g, int& i) {
  int   level = 0;
  bool  b = (!i);
  PBVAL vlp, firstvlp, lastvlp;

  vlp = firstvlp = lastvlp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
    case ',':
      if (level < 2) {
        sprintf(g->Message, "Unexpected ',' near %.*s", ARGS);
        throw 1;
      } else
        level = 1;

      break;
    case ']':
      if (level == 1) {
        sprintf(g->Message, "Unexpected ',]' near %.*s", ARGS);
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
        sprintf(g->Message, "Unexpected value near %.*s", ARGS);
        throw 1;
      } else if (lastvlp) {
        vlp = ParseValue(g, i);
        lastvlp->Next = MOF(vlp);
        lastvlp = vlp;
      } else
        firstvlp = lastvlp = ParseValue(g, i);

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
OFFSET BDOC::ParseObject(PGLOBAL g, int& i) {
  OFFSET key;
  int    level = 0;
  PBPR   bpp, firstbpp, lastbpp;

  bpp = firstbpp = lastbpp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
    case '"':
      if (level < 2) {
        key = ParseString(g, ++i);
        bpp = SubAllocPair(g, key);

        if (lastbpp) {
          lastbpp->Next = MOF(bpp);
          lastbpp = bpp;
        } else 
          firstbpp = lastbpp = bpp;

        level = 1;
      } else {
        sprintf(g->Message, "misplaced string near %.*s", ARGS);
        throw 2;
      } // endif level

      break;
    case ':':
      if (level == 1) {
        lastbpp->Vlp = MOF(ParseValue(g, ++i));
        level = 2;
      } else {
        sprintf(g->Message, "Unexpected ':' near %.*s", ARGS);
        throw 2;
      } // endif level

      break;
    case ',':
      if (level < 2) {
        sprintf(g->Message, "Unexpected ',' near %.*s", ARGS);
        throw 2;
      } else
        level = 0;

      break;
    case '}':
      if (level < 2) {
        sprintf(g->Message, "Unexpected '}' near %.*s", ARGS);
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
      sprintf(g->Message, "Unexpected character '%c' near %.*s",
        s[i], ARGS);
      throw 2;
    }; // endswitch s[i]

  strcpy(g->Message, "Unexpected EOF in Object");
  throw 2;
} // end of ParseObject

/***********************************************************************/
/* Parse a JSON Value.                                                 */
/***********************************************************************/
PBVAL BDOC::ParseValue(PGLOBAL g, int& i) {
  PBVAL bvp = SubAllocVal(g);

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
    bvp->To_Val = ParseArray(g, ++i);
    bvp->Type = TYPE_JAR;
    break;
  case '{':
    bvp->To_Val = ParseObject(g, ++i);
    bvp->Type = TYPE_JOB;
    break;
  case '"':
    //    jvp->Val = AllocVal(g, TYPE_STRG);
    bvp->To_Val = ParseString(g, ++i);
    bvp->Type = TYPE_STRG;
    break;
  case 't':
    if (!strncmp(s + i, "true", 4)) {
      //      jvp->Val = AllocVal(g, TYPE_BOOL);
      bvp->B = true;
      bvp->Type = TYPE_BOOL;
      i += 3;
    } else
      goto err;

    break;
  case 'f':
    if (!strncmp(s + i, "false", 5)) {
      //      jvp->Val = AllocVal(g, TYPE_BOOL);
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
      ParseNumeric(g, i, bvp);
    else
      goto err;

  }; // endswitch s[i]

  return bvp;

err:
  sprintf(g->Message, "Unexpected character '%c' near %.*s", s[i], ARGS);
  throw 3;
} // end of ParseValue

/***********************************************************************/
/*  Unescape and parse a JSON string.                                  */
/***********************************************************************/
OFFSET BDOC::ParseString(PGLOBAL g, int& i) {
  uchar* p;
  int    n = 0;

  // Be sure of memory availability
  if (((size_t)len + 1 - i) > ((PPOOLHEADER)g->Sarea)->FreeBlk)
    throw("ParseString: Out of memory");

  // The size to allocate is not known yet
  p = (uchar*)PlugSubAlloc(g, NULL, 0);

  for (; i < len; i++)
    switch (s[i]) {
    case '"':
      p[n++] = 0;
      PlugSubAlloc(g, NULL, n);
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
void BDOC::ParseNumeric(PGLOBAL g, int& i, PBVAL vlp) {
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
      double dv = strtod(buf, NULL);

      if (nd > 6) {
        double* dvp = (double*)PlugSubAlloc(g, NULL, sizeof(double));

        *dvp = dv;
        vlp->To_Val = MOF(dvp);
        vlp->Type = TYPE_DBL;
      } else {
        vlp->F = (float)dv;
        vlp->Type = TYPE_FLOAT;
      } // endif nd

      vlp->Nd = nd;
    } else {
      longlong iv = strtoll(buf, NULL, 10);

      if (iv > INT_MAX32 || iv < INT_MIN32) {
        longlong *llp = (longlong*)PlugSubAlloc(g, NULL, sizeof(longlong));

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
PSZ BDOC::Serialize(PGLOBAL g, PBVAL bvp, char* fn, int pretty) {
  PSZ   str = NULL;
  bool  b = false, err = true;
  FILE* fs = NULL;

  g->Message[0] = 0;

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
      strcpy(g->Message, "Invalid json tree");
    } // endswitch Type

    if (fs) {
      fputs(EL, fs);
      fclose(fs);
      str = (err) ? NULL : strcpy(g->Message, "Ok");
    } else if (!err) {
      str = ((JOUTSTR*)jp)->Strp;
      jp->WriteChr('\0');
      PlugSubAlloc(g, NULL, ((JOUTSTR*)jp)->N);
    } else {
      if (!g->Message[0])
        strcpy(g->Message, "Error in Serialize");

    } // endif's

  } catch (int n) {
    if (trace(1))
      htrc("Exception %d: %s\n", n, g->Message);
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
bool BDOC::SerializeArray(OFFSET arp, bool b) {
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

  for (vp; vp; vp = MVP(vp->Next)) {
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
bool BDOC::SerializeObject(OFFSET obp) {
  bool first = true;
  PBPR prp = MPP(obp);

  if (jp->WriteChr('{'))
    return true;

  for (prp; prp; prp = MPP(prp->Next)) {
    if (first)
      first = false;
    else if (jp->WriteChr(','))
      return true;

    if (jp->WriteChr('"') ||
      jp->WriteStr(MZP(prp->Key)) ||
      jp->WriteChr('"') ||
      jp->WriteChr(':') ||
      SerializeValue(MVP(prp->Vlp)))
      return true;

  } // endfor i

  return jp->WriteChr('}');
} // end of SerializeObject

/***********************************************************************/
/* Serialize a JSON Value.                                             */
/***********************************************************************/
bool BDOC::SerializeValue(PBVAL jvp) {
  char buf[64];

  switch (jvp->Type) {
  case TYPE_JAR:
    return SerializeArray(jvp->To_Val, false);
  case TYPE_JOB:
    return SerializeObject(jvp->To_Val);
  case TYPE_BOOL:
    return jp->WriteStr(jvp->B ? "true" : "false");
  case TYPE_STRG:
  case TYPE_DTM:
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
  default:
    return jp->WriteStr("???");   // TODO
  } // endswitch Type

  strcpy(jp->g->Message, "Unrecognized value");
  return true;
} // end of SerializeValue

/* --------------------------- Class BJSON --------------------------- */

/***********************************************************************/
/*  Program for sub-allocating Bjson structures.                       */
/***********************************************************************/
void* BJSON::BsonSubAlloc(PGLOBAL g, size_t size)
{
  PPOOLHEADER pph;                           /* Points on area header. */
  void* memp = g->Sarea;

  size = ((size + 3) / 4) * 4;       /* Round up size to multiple of 4 */
  pph = (PPOOLHEADER)memp;

  xtrc(16, "SubAlloc in %p size=%zd used=%zd free=%zd\n",
    memp, size, pph->To_Free, pph->FreeBlk);

  if (size > pph->FreeBlk) {   /* Not enough memory left in pool */
    sprintf(g->Message,
      "Not enough memory for request of %zd (used=%zd free=%zd)",
      size, pph->To_Free, pph->FreeBlk);
    xtrc(1, "BsonSubAlloc: %s\n", g->Message);
    throw(1234);
  } /* endif size OS32 code */

  // Do the suballocation the simplest way
  memp = MakePtr(memp, pph->To_Free); /* Points to suballocated block  */
  pph->To_Free += size;               /* New offset of pool free block */
  pph->FreeBlk -= size;               /* New size   of pool free block */
  xtrc(16, "Done memp=%p used=%zd free=%zd\n",
    memp, pph->To_Free, pph->FreeBlk);
  return memp;
} /* end of BsonSubAlloc */

/* ------------------------ Bobject functions ------------------------ */

/***********************************************************************/
/* Sub-allocate and initialize a BPAIR.                                */
/***********************************************************************/
PBPR BJSON::SubAllocPair(PGLOBAL g, OFFSET key, OFFSET val)
{
  PBPR bpp = (PBPR)BsonSubAlloc(g, sizeof(BPAIR));

  bpp->Key = key;
  bpp->Vlp = val;
  bpp->Next = 0;
  return bpp;
} // end of SubAllocPair

/***********************************************************************/
/* Return the number of pairs in this object.                          */
/***********************************************************************/
int BJSON::GetObjectSize(PBPR bop, bool b)
{
  int n = 0;

  for (PBPR brp = bop; brp; brp = MPP(brp->Next))
    // If b return only non null pairs
    if (!b || (brp->Vlp && (MVP(brp->Vlp))->Type != TYPE_NULL))
      n++;

  return n;
} // end of GetObjectSize

/***********************************************************************/
/* Add a new pair to an Object and return it.                          */
/***********************************************************************/
PBPR BJSON::AddPair(PGLOBAL g, PBPR bop, PSZ key, OFFSET val)
{
  PBPR brp, nrp = SubAllocPair(g, MOF(key), val);

  if (bop) {
    for (brp = bop; brp->Next; brp = MPP(brp->Next));

    brp->Next = MOF(nrp);
  } else
    bop = nrp;

  return bop;
} // end of AddPair

/***********************************************************************/
/* Return all object keys as an array.                                 */
/***********************************************************************/
PBVAL BJSON::GetKeyList(PGLOBAL g, PBPR bop)
{
  PBVAL bvp, lvp, fvp = NULL;

  for (PBPR brp = bop; brp; brp = MPP(brp->Next))
    if (fvp) {
      bvp = SubAllocVal(g, brp->Key, TYPE_STRG);
      lvp->Next = MOF(bvp);
      lvp = bvp;
    } else
      lvp = fvp = SubAllocVal(g, brp->Key, TYPE_STRG);

  return fvp;
} // end of GetKeyList

/***********************************************************************/
/* Return all object values as an array.                               */
/***********************************************************************/
PBVAL BJSON::GetObjectValList(PGLOBAL g, PBPR bop)
{
  PBVAL bvp, lvp, fvp = NULL;

  for (PBPR brp = bop; brp; brp = MPP(brp->Next))
    if (fvp) {
      bvp = DupVal(g, MVP(brp->Vlp));
      lvp->Next = MOF(bvp);
      lvp = bvp;
    } else
      lvp = fvp = DupVal(g, MVP(brp->Vlp));

  return fvp;
} // end of GetObjectValList

/***********************************************************************/
/* Get the value corresponding to the given key.                       */
/***********************************************************************/
PBVAL BJSON::GetKeyValue(PBPR bop, PSZ key)
{
  for (PBPR brp = bop; brp; brp = MPP(brp->Next))
    if (!strcmp(MZP(brp->Key), key))
      return MVP(brp->Vlp);

  return NULL;
} // end of GetKeyValue;

/***********************************************************************/
/* Return the text corresponding to all keys (XML like).               */
/***********************************************************************/
PSZ BJSON::GetObjectText(PGLOBAL g, PBPR bop, PSTRG text) {
  if (bop) {
    bool b;

    if (!text) {
      text = new(g) STRING(g, 256);
      b = true;
    } else {
      if (text->GetLastChar() != ' ')
        text->Append(' ');

      b = false;
    }	// endif text

#if 0
    if (b && !bop->Next && !strcmp(MZP(bop->Key), "$date")) {
      int i;
      PSZ s;

      First->Val->GetText(g, text);
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

    } else
#endif // 0

    for (PBPR brp = bop; brp; brp = MPP(brp->Next)) {
      GetValueText(g, MVP(brp->Vlp), text);

      if (brp->Next)
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
PBPR BJSON::SetKeyValue(PGLOBAL g, PBPR bop, OFFSET bvp, PSZ key)
{
  PBPR brp = bop, prp = NULL;

  if (brp) {
    for (brp = bop; brp; brp = MPP(brp->Next))
      if (!strcmp(MZP(brp->Key), key)) {
        brp->Vlp = bvp;
        break;
      } else
        prp = brp;

    if (!brp)
      prp->Vlp = MOF(SubAllocPair(g, MOF(key), bvp));

  } else
    bop = SubAllocPair(g, MOF(key), bvp);

  // Return the first pair of this object
  return bop;
} // end of SetKeyValue

/***********************************************************************/
/* Merge two objects.                                                  */
/***********************************************************************/
PBPR BJSON::MergeObject(PGLOBAL g, PBPR bop1, PBPR bop2)
{
  if (bop1)
    for (PBPR brp = bop2; brp; brp = MPP(brp->Next))
      SetKeyValue(g, bop1, brp->Vlp, MZP(brp->Key));

  else
    bop1 = bop2;

  return bop1;
} // end of MergeObject;

/***********************************************************************/
/* Delete a value corresponding to the given key.                      */
/***********************************************************************/
PBPR BJSON::DeleteKey(PBPR bop, PCSZ key)
{
  PBPR brp, pbrp = NULL;

  for (brp = bop; brp; brp = MPP(brp->Next))
    if (!strcmp(MZP(brp->Key), key)) {
      if (pbrp) {
        pbrp->Next = brp->Next;
        return bop;
      } else
        return MPP(brp->Next);

    } else
      pbrp = brp;

  return bop;
} // end of DeleteKey

/***********************************************************************/
/* True if void or if all members are nulls.                           */
/***********************************************************************/
bool BJSON::IsObjectNull(PBPR bop)
{
  for (PBPR brp = bop; brp; brp = MPP(brp->Next))
    if (brp->Vlp && (MVP(brp->Vlp))->Type != TYPE_NULL)
      return false;

  return true;
} // end of IsObjectNull

/* ------------------------- Barray functions ------------------------ */

/***********************************************************************/
/* Return the number of values in this object.                         */
/***********************************************************************/
int BJSON::GetArraySize(PBVAL bap, bool b)
{
  int n = 0;

  for (PBVAL bvp = bap; bvp; bvp = MVP(bvp->Next))
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
  int i = 0;

  for (PBVAL bvp = bap; bvp; bvp = MVP(bvp->Next))
    if (i == n)
      return bvp;
    else
      i++;

  return NULL;
} // end of GetArrayValue

/***********************************************************************/
/* Add a Value to the Array Value list.                                */
/***********************************************************************/
PBVAL BJSON::AddArrayValue(PGLOBAL g, PBVAL bap, PBVAL nvp, int* x)
{
  if (!nvp)
    nvp = SubAllocVal(g);

  if (bap) {
    int   i = 0, n = *x;
    PBVAL bvp;

    for (bvp = bap; bvp; bvp = MVP(bvp->Next), i++)
      if (!bvp->Next || (x && i == n)) {
        nvp->Next = bvp->Next;
        bvp->Next = MOF(nvp);
        break;
      } // endif Next

  } else
    bap = nvp;

  return bap;
} // end of AddArrayValue

/***********************************************************************/
/* Merge two arrays.                                                   */
/***********************************************************************/
PBVAL BJSON::MergeArray(PGLOBAL g, PBVAL bap1, PBVAL bap2)
{
  if (bap1) {
    for (PBVAL bvp = bap2; bvp; bvp = MVP(bvp->Next))
      AddArrayValue(g, bap1, bvp);

    return bap1;
  } else
    return bap2;

} // end of MergeArray

/***********************************************************************/
/* Set the nth Value of the Array Value list or add it.                          */
/***********************************************************************/
PBVAL BJSON::SetArrayValue(PGLOBAL g, PBVAL bap, PBVAL nvp, int n)
{
  PBVAL bvp = bap, pvp = NULL;

  if (bvp) {
    for (int i = 0; bvp; i++, bvp = MVP(bvp->Next))
      if (i == n) {
        bvp->To_Val = nvp->To_Val;
        bvp->Nd = nvp->Nd;
        bvp->Type = nvp->Type;
        return bap;
      } else
        pvp = bvp;

  } // endif bap

  if (!bvp) {
    bvp = DupVal(g, nvp);

    if (pvp)
      pvp->Next = MOF(bvp);
    else
      bap = bvp;

  } // endif bvp

  return bap;
} // end of SetValue

/***********************************************************************/
/* Return the text corresponding to all values.                        */
/***********************************************************************/
PSZ BJSON::GetArrayText(PGLOBAL g, PBVAL bap, PSTRG text) {
  if (bap) {
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

    for (PBVAL bvp = bap; bvp; bvp = MVP(bvp->Next)) {
      GetValueText(g, bvp, text);

      if (bvp->Next)
        text->Append(", ");
      else if (!b)
        text->Append(')');

    }	// endfor jp

    if (b) {
      text->Trim();
      return text->GetStr();
    }	// endif b

  } // endif First

  return NULL;
} // end of GetText;

/***********************************************************************/
/* Delete a Value from the Arrays Value list.                          */
/***********************************************************************/
PBVAL BJSON::DeleteValue(PBVAL bap, int n)
{
  PBVAL bvp = bap, pvp = NULL;

  if (bvp)
    for (int i = 0; bvp; i++, bvp = MVP(bvp->Next))
      if (i == n) {
        if (pvp)
          pvp->Next = bvp->Next;
        else
          bap = bvp;

        break;
      } // endif i

  return bap;
} // end of DeleteValue

/***********************************************************************/
/* True if void or if all members are nulls.                           */
/***********************************************************************/
bool BJSON::IsArrayNull(PBVAL bap)
{
  for (PBVAL bvp = bap; bvp; bvp = MVP(bvp->Next))
    if (bvp->Type != TYPE_NULL)
      return false;

  return true;
} // end of IsNull

/* ------------------------- Bvalue functions ------------------------ */

/***********************************************************************/
/* Sub-allocate and clear a BVAL.                                      */
/***********************************************************************/
PBVAL BJSON::SubAllocVal(PGLOBAL g)
{
  PBVAL bvp = (PBVAL)BsonSubAlloc(g, sizeof(BVAL));

  bvp->To_Val = 0;
  bvp->Nd = 0;
  bvp->Type = TYPE_UNKNOWN;
  bvp->Next = 0;
  return bvp;
} // end of SubAllocVal

/***********************************************************************/
/* Sub-allocate and initialize a BVAL as string.                       */
/***********************************************************************/
PBVAL BJSON::SubAllocVal(PGLOBAL g, OFFSET toval, JTYP type, short nd)
{
  PBVAL bvp = (PBVAL)BsonSubAlloc(g, sizeof(BVAL));

  bvp->To_Val = toval;
  bvp->Nd = nd;
  bvp->Type = type;
  bvp->Next = 0;
  return bvp;
} // end of SubAllocVal

/***********************************************************************/
/* Allocate a BVALUE with a given string or numeric value.             */
/***********************************************************************/
PBVAL BJSON::SubAllocVal(PGLOBAL g, PVAL valp)
{
  PBVAL vlp = SubAllocVal(g);
  SetValue(g, vlp, valp);
  vlp->Next = NULL;
  return vlp;
} // end of SubAllocVal

/***********************************************************************/
/* Sub-allocate and initialize a BVAL from another BVAL.               */
/***********************************************************************/
PBVAL BJSON::DupVal(PGLOBAL g, PBVAL bvlp) {
  PBVAL bvp = (PBVAL)BsonSubAlloc(g, sizeof(BVAL));

  *bvp = *bvlp;
  bvp->Next = 0;
  return bvp;
} // end of DupVal

#if 0
/***********************************************************************/
/* Constructor for a JVALUE.                                           */
/***********************************************************************/
JVALUE::JVALUE(PJSON jsp) : JSON() {
  if (jsp->GetType() == TYPE_JVAL) {
    PJVAL jvp = (PJVAL)jsp;

    //  Val = ((PJVAL)jsp)->GetVal();
    if (jvp->DataType == TYPE_JSON) {
      Jsp = jvp->GetJsp();
      DataType = TYPE_JSON;
      Nd = 0;
    } else {
      LLn = jvp->LLn;   // Must be LLn on 32 bit machines
      Nd = jvp->Nd;
      DataType = jvp->DataType;
    } // endelse Jsp

  } else {
    Jsp = jsp;
    //  Val = NULL;
    DataType = TYPE_JSON;
    Nd = 0;
  } // endif Type

  Next = NULL;
  Del = false;
  Type = TYPE_JVAL;
} // end of JVALUE constructor

/***********************************************************************/
/* Constructor for a JVALUE with a given string or numeric value.      */
/***********************************************************************/
JVALUE::JVALUE(PGLOBAL g, PVL vlp) : JSON() {
  Jsp = NULL;
  Val = vlp;
  Next = NULL;
  Del = false;
  Type = TYPE_JVAL;
} // end of JVALUE constructor
#endif // 0

#if 0
/***********************************************************************/
/* Constructor for a given string.                                     */
/***********************************************************************/
JVALUE::JVALUE(PGLOBAL g, PCSZ strp) : JSON() {
  Jsp = NULL;
  //Val = AllocVal(g, TYPE_STRG);
  Strp = (char*)strp;
  DataType = TYPE_STRG;
  Nd = 0;
  Next = NULL;
  Del = false;
  Type = TYPE_JVAL;
} // end of JVALUE constructor

/***********************************************************************/
/* Set or reset all Jvalue members.                                    */
/***********************************************************************/
void JVALUE::Clear(void) {
  Jsp = NULL;
  Next = NULL;
  Type = TYPE_JVAL;
  Del = false;
  Nd = 0;
  DataType = TYPE_NULL;
} // end of Clear

/***********************************************************************/
/* Returns the type of the Value's value.                              */
/***********************************************************************/
JTYP JVALUE::GetValType(void) {
  if (DataType == TYPE_JSON)
    return Jsp->GetType();
  //else if (Val)
  //  return Val->Type;
  else
    return DataType;

} // end of GetValType
#endif // 0

/***********************************************************************/
/* Return the size of value's value.                                   */
/***********************************************************************/
int BJSON::GetSize(PBVAL vlp, bool b)
{
  switch (vlp->Type) {
  case TYPE_JAR:
    return GetArraySize(MVP(vlp->To_Val));
  case TYPE_JOB:
    return GetObjectSize(MPP(vlp->To_Val));
  default:
    return 1;
  } // enswitch Type

} // end of GetSize

/***********************************************************************/
/* Return the Value's Object value.                                    */
/***********************************************************************/
PBPR BJSON::GetObject(PBVAL vlp)
{
  if (vlp->Type == TYPE_JOB)
    return MPP(vlp->To_Val);

  return NULL;
} // end of GetObject

/***********************************************************************/
/* Return the Value's Array value.                                     */
/***********************************************************************/
PBVAL BJSON::GetArray(PBVAL vlp)
{
  if (vlp->Type == TYPE_JAR)
    return MVP(vlp->To_Val);

  return NULL;
} // end of GetArray

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
} // end of GetFloat

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ BJSON::GetString(PGLOBAL g, PBVAL vp, char* buff)
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

  return (p == buf) ? (PSZ)PlugDup(g, buf) : p;
} // end of GetString

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ BJSON::GetValueText(PGLOBAL g, PBVAL vlp, PSTRG text) {
  if (vlp->Type == TYPE_JOB)
    return GetObjectText(g, MPP(vlp->To_Val), text);
  else if (vlp->Type == TYPE_JAR)
    return GetArrayText(g, MVP(vlp->To_Val), text);

  char buff[32];
  PSZ  s = (vlp->Type == TYPE_NULL) ? NULL : GetString(g, vlp, buff);

  if (s)
    text->Append(s);
  else if (GetJsonNull())
    text->Append(GetJsonNull());

  return NULL;
} // end of GetText

void BJSON::SetValueObj(PBVAL vlp, PBPR bop)
{
  vlp->To_Val = MOF(bop);
  vlp->Type = TYPE_JOB;
} // end of SetValueObj;

void BJSON::SetValueArr(PBVAL vlp, PBVAL bap)
{
  vlp->To_Val = MOF(bap);
  vlp->Type = TYPE_JAR;
} // end of SetValue;

void BJSON::SetValueVal(PBVAL vlp, PBVAL vp)
{
  vlp->To_Val = vp->To_Val;
  vlp->Nd = vp->Nd;
  vlp->Type = vp->Type;
} // end of SetValue;

void BJSON::SetValue(PGLOBAL g, PBVAL vlp, PVAL valp)
{
  if (!valp || valp->IsNull()) {
    vlp->Type = TYPE_NULL;
  } else switch (valp->GetType()) {
  case TYPE_DATE:
    if (((DTVAL*)valp)->IsFormatted())
      vlp->To_Val = MOF(valp->GetCharValue());
    else {
      char buf[32];

      vlp->To_Val = MOF(PlugDup(g, valp->GetCharString(buf)));
    }	// endif Formatted

    vlp->Type = TYPE_DTM;
    break;
  case TYPE_STRING:
    vlp->To_Val = MOF(valp->GetCharValue());
    vlp->Type = TYPE_STRG;
    break;
  case TYPE_DOUBLE:
  case TYPE_DECIM:
    vlp->Nd = (IsTypeNum(valp->GetType())) ? valp->GetValPrec() : 0;

    if (vlp->Nd <= 6) {
      vlp->F = (float)valp->GetFloatValue();
      vlp->Type = TYPE_FLOAT;
    } else {
      double *dp = (double*)PlugSubAlloc(g, NULL, sizeof(double));

      *dp = valp->GetFloatValue();
      vlp->To_Val = MOF(dp);
      vlp->Type = TYPE_DBL;
    } // endif Nd

    break;
  case TYPE_TINY:
    vlp->B = valp->GetTinyValue() != 0;
    vlp->Type = TYPE_BOOL;
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
      longlong* llp = (longlong*)PlugSubAlloc(g, NULL, sizeof(longlong));

      *llp = valp->GetBigintValue();
      vlp->To_Val = MOF(llp);
      vlp->Type = TYPE_BINT;
    } // endif BigintValue

    break;
  default:
    sprintf(g->Message, "Unsupported typ %d\n", valp->GetType());
    throw(777);
  } // endswitch Type

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
void BJSON::SetBigint(PGLOBAL g, PBVAL vlp, longlong ll)
{
  if (ll >= INT_MIN32 && ll <= INT_MAX32) {
    vlp->N = (int)ll;
    vlp->Type = TYPE_INTG;
  } else {
    longlong* llp = (longlong*)PlugSubAlloc(g, NULL, sizeof(longlong));

    *llp = ll;
    vlp->To_Val = MOF(llp);
    vlp->Type = TYPE_BINT;
  } // endif ll

} // end of SetBigint

/***********************************************************************/
/* Set the Value's value as the given DOUBLE.                          */
/***********************************************************************/
void BJSON::SetFloat(PBVAL vlp, double f) {
  vlp->F = (float)f;
  vlp->Nd = 6;
  vlp->Type = TYPE_FLOAT;
} // end of SetFloat

/***********************************************************************/
/* Set the Value's value as the given string.                          */
/***********************************************************************/
void BJSON::SetString(PBVAL vlp, PSZ s, int ci) {
  vlp->To_Val = MOF(s);
  vlp->Nd = ci;
  vlp->Type = TYPE_STRG;
} // end of SetString

/***********************************************************************/
/* True when its JSON or normal value is null.                         */
/***********************************************************************/
bool BJSON::IsValueNull(PBVAL vlp) {
  bool b;

  switch (vlp->Type) {
  case TYPE_NULL:
    b = true;
    break;
  case TYPE_JOB:
    b = IsObjectNull(MPP(vlp->To_Val));
    break;
  case TYPE_JAR:
    b = IsArrayNull(MVP(vlp->To_Val));
    break;
  default:
    b = false;
  } // endswitch Type

  return b;
  } // end of IsNull
