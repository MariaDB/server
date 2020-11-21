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

#define ARGS       MY_MIN(24,(int)len-i),s+MY_MAX(i-3,0)

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
BDOC::BDOC(void) : jp(NULL), base(NULL), s(NULL), len(0) 
{ 
  pty[0] = pty[1] = pty[2] = true;
} // end of BDOC constructor

/***********************************************************************/
/*  Program for sub-allocating Bson structures.                        */
/***********************************************************************/
void* BDOC::BsonSubAlloc(PGLOBAL g, size_t size) {
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



/***********************************************************************/
/* Parse a json string.                                                */
/* Note: when pretty is not known, the caller set pretty to 3.         */
/***********************************************************************/
PBVAL BDOC::ParseJson(PGLOBAL g, char* js, size_t lng, int* ptyp, bool* comma) {
  int   i, pretty = (ptyp) ? *ptyp : 3;
  bool  b = false;
  PBVAL bvp = NULL;

  xtrc(1, "ParseJson: s=%.10s len=%zd\n", s, len);

  if (!s || !len) {
    strcpy(g->Message, "Void JSON object");
    return NULL;
  } else if (comma)
    *comma = false;

  // Trying to guess the pretty format
  if (s[0] == '[' && (s[1] == '\n' || (s[1] == '\r' && s[2] == '\n')))
    pty[0] = false;

  s = js;
  len = lng;

  try {
    bvp = (PBVAL)PlugSubAlloc(g, NULL, sizeof(BVAL));
    bvp->Type = TYPE_UNKNOWN;
    base = bvp;

    for (i = 0; i < len; i++)
      switch (s[i]) {
      case '[':
        if (bvp->Type != TYPE_UNKNOWN)
          bvp->To_Val = ParseAsArray(g, i, pretty, ptyp);
        else
          bvp->To_Val = ParseArray(g, ++i);

        bvp->Type = TYPE_JAR;
        break;
      case '{':
        if (bvp->Type != TYPE_UNKNOWN) {
          bvp->To_Val = ParseAsArray(g, i, pretty, ptyp);
          bvp->Type = TYPE_JAR;
        } else if ((bvp->To_Val = ParseObject(g, ++i)))
          bvp->Type = TYPE_JOB;
        else
          throw 2;

        break;
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        break;
      case ',':
        if (bvp->Type != TYPE_UNKNOWN && (pretty == 1 || pretty == 3)) {
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
        if (bvp->Type != TYPE_UNKNOWN) {
          bvp->To_Val = ParseAsArray(g, i, pretty, ptyp);
          bvp->Type = TYPE_JAR;
        } else if ((bvp->To_Val = MakeOff(base, ParseValue(g, i))))
          bvp->Type = TYPE_JVAL;
        else
          throw 4;

        break;
      }; // endswitch s[i]

    if (bvp->Type == TYPE_UNKNOWN)
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

      return MakeOff(base, vlp);
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
        lastvlp->Next = MakeOff(base, vlp);
        lastvlp = vlp;
      } else
        firstvlp = lastvlp = ParseValue(g, i);

      level = (b) ? 1 : 2;
      break;
    }; // endswitch s[i]

  if (b) {
    // Case of Pretty == 0
    return MakeOff(base, vlp);
  } // endif b

  throw ("Unexpected EOF in array");
} // end of ParseArray

/***********************************************************************/
/* Sub-allocate and initialize a BPAIR.                                */
/***********************************************************************/
PBPR BDOC::SubAllocPair(PGLOBAL g, OFFSET key)
{
  PBPR bpp = (PBPR)BsonSubAlloc(g, sizeof(BPAIR));

  bpp->Key = key;
  bpp->Vlp = 0;
  bpp->Next = 0;
  return bpp;
} // end of SubAllocPair

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
          lastbpp->Next = MakeOff(base, bpp);
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
        lastbpp->Vlp = MakeOff(base, ParseValue(g, ++i));
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

      return MakeOff(base, firstbpp);
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
/* Sub-allocate and initialize a BVAL.                                 */
/***********************************************************************/
PBVAL BDOC::SubAllocVal(PGLOBAL g)
{
  PBVAL bvp = (PBVAL)BsonSubAlloc(g, sizeof(BVAL));

  bvp->To_Val = 0;
  bvp->Nd = 0;
  bvp->Type = TYPE_UNKNOWN;
  bvp->Next = 0;
  return bvp;
} // end of SubAllocVal

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
      return MakeOff(base, p);
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
        vlp->To_Val = MakeOff(base, dvp);
        vlp->Type = TYPE_DBL;
      } else {
        vlp->F = (float)dv;
        vlp->Type = TYPE_FLOAT;
      } // endif nd

      vlp->Nd = nd;
    } else {
      long long iv = strtoll(buf, NULL, 10);

      if (iv > INT_MAX32 || iv < INT_MIN32) {
        long long *llp = (long long*)PlugSubAlloc(g, NULL, sizeof(long long));

        *llp = iv;
        vlp->To_Val = MakeOff(base, llp);
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
/* Serialize a JSON document tree:                                     */
/***********************************************************************/
PSZ BDOC::Serialize(PGLOBAL g, PBVAL bvp, char* fn, int pretty) {
  PSZ   str = NULL;
  bool  b = false, err = true;
  JOUT* jp;
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
      err = SerializeValue((PBVAL)MakePtr(base, bvp->To_Val));
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
  PBVAL vp = (PBVAL)MakePtr(base, arp);

  if (b) {
    if (jp->Prty()) {
      if (jp->WriteChr('['))
        return true;
      else if (jp->Prty() == 1 && (jp->WriteStr(EL) || jp->WriteChr('\t')))
        return true;

    } // endif Prty

  } else if (jp->WriteChr('['))
    return true;

  for (vp; vp; vp = (PBVAL)MakePtr(base, vp->Next)) {
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

  } // endfor i

  if (b && jp->Prty() == 1 && jp->WriteStr(EL))
    return true;

  return ((!b || jp->Prty()) && jp->WriteChr(']'));
} // end of SerializeArray

/***********************************************************************/
/* Serialize a JSON Object.                                            */
/***********************************************************************/
bool BDOC::SerializeObject(OFFSET obp) {
  bool first = true;
  PBPR prp = (PBPR)MakePtr(base, obp);

  if (jp->WriteChr('{'))
    return true;

  for (prp; prp; prp = (PBPR)MakePtr(base, prp->Next)) {
    if (first)
      first = false;
    else if (jp->WriteChr(','))
      return true;

    if (jp->WriteChr('"') ||
      jp->WriteStr((const char*)MakePtr(base, prp->Key)) ||
      jp->WriteChr('"') ||
      jp->WriteChr(':') ||
      SerializeValue((PBVAL)MakePtr(base, prp->Vlp)))
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
    return jp->Escape((const char*)MakePtr(base, jvp->To_Val));
  case TYPE_INTG:
    sprintf(buf, "%d", jvp->N);
    return jp->WriteStr(buf);
  case TYPE_BINT:
    sprintf(buf, "%lld", *(long long*)MakePtr(base, jvp->To_Val));
    return jp->WriteStr(buf);
  case TYPE_FLOAT:
    sprintf(buf, "%.*f", jvp->Nd, jvp->F);
    return jp->WriteStr(buf);
  case TYPE_DBL:
    sprintf(buf, "%.*lf", jvp->Nd, *(double*)MakePtr(base, jvp->To_Val));
    return jp->WriteStr(buf);
  case TYPE_NULL:
    return jp->WriteStr("null");
  default:
    return jp->WriteStr("???");   // TODO
  } // endswitch Type

  strcpy(jp->g->Message, "Unrecognized value");
  return true;
} // end of SerializeValue

#if 0
/* -------------------------- Class JOBJECT -------------------------- */

/***********************************************************************/
/* Return the number of pairs in this object.                          */
/***********************************************************************/
int JOBJECT::GetSize(bool b) {
  int n = 0;

  for (PJPR jpp = First; jpp; jpp = jpp->Next)
    // If b return only non null pairs
    if (!b || jpp->Val && !jpp->Val->IsNull())
      n++;

  return n;
} // end of GetSize

/***********************************************************************/
/* Add a new pair to an Object.                                        */
/***********************************************************************/
PJPR JOBJECT::AddPair(PGLOBAL g, PCSZ key) {
  PJPR jpp = (PJPR)PlugSubAlloc(g, NULL, sizeof(JPAIR));

  jpp->Key = key;
  jpp->Next = NULL;
  jpp->Val = NULL;

  if (Last)
    Last->Next = jpp;
  else
    First = jpp;

  Last = jpp;
  return jpp;
} // end of AddPair

/***********************************************************************/
/* Return all keys as an array.                                        */
/***********************************************************************/
PJAR JOBJECT::GetKeyList(PGLOBAL g) {
  PJAR jarp = new(g) JARRAY();

  for (PJPR jpp = First; jpp; jpp = jpp->Next)
    jarp->AddArrayValue(g, new(g) JVALUE(g, jpp->Key));

  jarp->InitArray(g);
  return jarp;
} // end of GetKeyList

/***********************************************************************/
/* Return all values as an array.                                      */
/***********************************************************************/
PJAR JOBJECT::GetValList(PGLOBAL g) {
  PJAR jarp = new(g) JARRAY();

  for (PJPR jpp = First; jpp; jpp = jpp->Next)
    jarp->AddArrayValue(g, jpp->Val);

  jarp->InitArray(g);
  return jarp;
} // end of GetValList

/***********************************************************************/
/* Get the value corresponding to the given key.                       */
/***********************************************************************/
PJVAL JOBJECT::GetKeyValue(const char* key) {
  for (PJPR jp = First; jp; jp = jp->Next)
    if (!strcmp(jp->Key, key))
      return jp->Val;

  return NULL;
} // end of GetValue;

/***********************************************************************/
/* Return the text corresponding to all keys (XML like).               */
/***********************************************************************/
PSZ JOBJECT::GetText(PGLOBAL g, PSTRG text) {
  if (First) {
    bool b;

    if (!text) {
      text = new(g) STRING(g, 256);
      b = true;
    } else {
      if (text->GetLastChar() != ' ')
        text->Append(' ');

      b = false;
    }	// endif text

    if (b && !First->Next && !strcmp(First->Key, "$date")) {
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

    } else for (PJPR jp = First; jp; jp = jp->Next) {
      jp->Val->GetText(g, text);

      if (jp->Next)
        text->Append(' ');

    }	// endfor jp

    if (b) {
      text->Trim();
      return text->GetStr();
    }	// endif b

  } // endif First

  return NULL;
} // end of GetText;

/***********************************************************************/
/* Merge two objects.                                                  */
/***********************************************************************/
bool JOBJECT::Merge(PGLOBAL g, PJSON jsp) {
  if (jsp->GetType() != TYPE_JOB) {
    strcpy(g->Message, "Second argument is not an object");
    return true;
  } // endif Type

  PJOB jobp = (PJOB)jsp;

  for (PJPR jpp = jobp->First; jpp; jpp = jpp->Next)
    SetKeyValue(g, jpp->Val, jpp->Key);

  return false;
} // end of Marge;

/***********************************************************************/
/* Set or add a value corresponding to the given key.                  */
/***********************************************************************/
void JOBJECT::SetKeyValue(PGLOBAL g, PJVAL jvp, PCSZ key) {
  PJPR jp;

  for (jp = First; jp; jp = jp->Next)
    if (!strcmp(jp->Key, key)) {
      jp->Val = jvp;
      break;
    } // endif key

  if (!jp) {
    jp = AddPair(g, key);
    jp->Val = jvp;
  } // endif jp

} // end of SetValue

/***********************************************************************/
/* Delete a value corresponding to the given key.                      */
/***********************************************************************/
void JOBJECT::DeleteKey(PCSZ key) {
  PJPR jp, * pjp = &First;

  for (jp = First; jp; jp = jp->Next)
    if (!strcmp(jp->Key, key)) {
      *pjp = jp->Next;
      break;
    } else
      pjp = &jp->Next;

} // end of DeleteKey

/***********************************************************************/
/* True if void or if all members are nulls.                           */
/***********************************************************************/
bool JOBJECT::IsNull(void) {
  for (PJPR jp = First; jp; jp = jp->Next)
    if (!jp->Val->IsNull())
      return false;

  return true;
} // end of IsNull

/* -------------------------- Class JARRAY --------------------------- */

/***********************************************************************/
/* JARRAY constructor.                                                 */
/***********************************************************************/
JARRAY::JARRAY(void) : JSON() {
  Type = TYPE_JAR;
  Size = 0;
  Alloc = 0;
  First = Last = NULL;
  Mvals = NULL;
}	// end of JARRAY constructor

/***********************************************************************/
/* Return the number of values in this object.                         */
/***********************************************************************/
int JARRAY::GetSize(bool b) {
  if (b) {
    // Return only non null values
    int n = 0;

    for (PJVAL jvp = First; jvp; jvp = jvp->Next)
      if (!jvp->IsNull())
        n++;

    return n;
  } else
    return Size;

} // end of GetSize

/***********************************************************************/
/* Make the array of values from the values list.                      */
/***********************************************************************/
void JARRAY::InitArray(PGLOBAL g) {
  int   i;
  PJVAL jvp, * pjvp = &First;

  for (Size = 0, jvp = First; jvp; jvp = jvp->Next)
    if (!jvp->Del)
      Size++;

  if (Size > Alloc) {
    // No need to realloc after deleting values
    Mvals = (PJVAL*)PlugSubAlloc(g, NULL, Size * sizeof(PJVAL));
    Alloc = Size;
  } // endif Size

  for (i = 0, jvp = First; jvp; jvp = jvp->Next)
    if (!jvp->Del) {
      Mvals[i++] = jvp;
      pjvp = &jvp->Next;
      Last = jvp;
    } else
      *pjvp = jvp->Next;

} // end of InitArray

/***********************************************************************/
/* Get the Nth value of an Array.                                      */
/***********************************************************************/
PJVAL JARRAY::GetArrayValue(int i) {
  if (Mvals && i >= 0 && i < Size)
    return Mvals[i];
  else
    return NULL;
} // end of GetValue

/***********************************************************************/
/* Add a Value to the Array Value list.                                */
/***********************************************************************/
PJVAL JARRAY::AddArrayValue(PGLOBAL g, PJVAL jvp, int* x) {
  if (!jvp)
    jvp = new(g) JVALUE;

  if (x) {
    int   i = 0, n = *x;
    PJVAL jp, * jpp = &First;

    for (jp = First; jp && i < n; i++, jp = *(jpp = &jp->Next));

    (*jpp) = jvp;

    if (!(jvp->Next = jp))
      Last = jvp;

  } else {
    if (!First)
      First = jvp;
    else if (Last == First)
      First->Next = Last = jvp;
    else
      Last->Next = jvp;

    Last = jvp;
    Last->Next = NULL;
  } // endif x

  return jvp;
} // end of AddValue

/***********************************************************************/
/* Merge two arrays.                                                   */
/***********************************************************************/
bool JARRAY::Merge(PGLOBAL g, PJSON jsp) {
  if (jsp->GetType() != TYPE_JAR) {
    strcpy(g->Message, "Second argument is not an array");
    return true;
  } // endif Type

  PJAR arp = (PJAR)jsp;

  for (int i = 0; i < arp->size(); i++)
    AddArrayValue(g, arp->GetArrayValue(i));

  InitArray(g);
  return false;
} // end of Merge

/***********************************************************************/
/* Set the nth Value of the Array Value list.                          */
/***********************************************************************/
bool JARRAY::SetArrayValue(PGLOBAL g, PJVAL jvp, int n) {
  int   i = 0;
  PJVAL jp, * jpp = &First;

  for (jp = First; i < n; i++, jp = *(jpp = &jp->Next))
    if (!jp)
      *jpp = jp = new(g) JVALUE;

  *jpp = jvp;
  jvp->Next = (jp ? jp->Next : NULL);
  return false;
} // end of SetValue

/***********************************************************************/
/* Return the text corresponding to all values.                        */
/***********************************************************************/
PSZ JARRAY::GetText(PGLOBAL g, PSTRG text) {
  if (First) {
    bool  b;
    PJVAL jp;

    if (!text) {
      text = new(g) STRING(g, 256);
      b = true;
    } else {
      if (text->GetLastChar() != ' ')
        text->Append(" (");
      else
        text->Append('(');

      b = false;
    }

    for (jp = First; jp; jp = jp->Next) {
      jp->GetText(g, text);

      if (jp->Next)
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
bool JARRAY::DeleteValue(int n) {
  PJVAL jvp = GetArrayValue(n);

  if (jvp) {
    jvp->Del = true;
    return false;
  } else
    return true;

} // end of DeleteValue

/***********************************************************************/
/* True if void or if all members are nulls.                           */
/***********************************************************************/
bool JARRAY::IsNull(void) {
  for (int i = 0; i < Size; i++)
    if (!Mvals[i]->IsNull())
      return false;

  return true;
} // end of IsNull

/* -------------------------- Class JVALUE- -------------------------- */

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

#if 0
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

/***********************************************************************/
/* Constructor for a JVALUE with a given string or numeric value.      */
/***********************************************************************/
JVALUE::JVALUE(PGLOBAL g, PVAL valp) : JSON() {
  Jsp = NULL;
  //Val = NULL;
  SetValue(g, valp);
  Next = NULL;
  Del = false;
  Type = TYPE_JVAL;
} // end of JVALUE constructor

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

/***********************************************************************/
/* Return the Value's Object value.                                    */
/***********************************************************************/
PJOB JVALUE::GetObject(void) {
  if (DataType == TYPE_JSON && Jsp->GetType() == TYPE_JOB)
    return (PJOB)Jsp;

  return NULL;
} // end of GetObject

/***********************************************************************/
/* Return the Value's Array value.                                     */
/***********************************************************************/
PJAR JVALUE::GetArray(void) {
  if (DataType == TYPE_JSON && Jsp->GetType() == TYPE_JAR)
    return (PJAR)Jsp;

  return NULL;
} // end of GetArray

/***********************************************************************/
/* Return the Value's as a Value class.                                */
/***********************************************************************/
PVAL JVALUE::GetValue(PGLOBAL g) {
  PVAL valp = NULL;

  if (DataType != TYPE_JSON)
    if (DataType == TYPE_STRG)
      valp = AllocateValue(g, Strp, DataType, Nd);
    else
      valp = AllocateValue(g, &LLn, DataType, Nd);

  return valp;
} // end of GetValue

/***********************************************************************/
/* Return the Value's Integer value.                                   */
/***********************************************************************/
int JVALUE::GetInteger(void) {
  int n;

  switch (DataType) {
  case TYPE_INTG: n = N;           break;
  case TYPE_DBL:  n = (int)F;      break;
  case TYPE_DTM:
  case TYPE_STRG: n = atoi(Strp);  break;
  case TYPE_BOOL: n = (B) ? 1 : 0; break;
  case TYPE_BINT: n = (int)LLn;    break;
  default:
    n = 0;
  } // endswitch Type

  return n;
} // end of GetInteger

/***********************************************************************/
/* Return the Value's Big integer value.                               */
/***********************************************************************/
long long JVALUE::GetBigint(void) {
  long long lln;

  switch (DataType) {
  case TYPE_BINT: lln = LLn;          break;
  case TYPE_INTG: lln = (long long)N; break;
  case TYPE_DBL:  lln = (long long)F; break;
  case TYPE_DTM:
  case TYPE_STRG: lln = atoll(Strp);  break;
  case TYPE_BOOL: lln = (B) ? 1 : 0;  break;
  default:
    lln = 0;
  } // endswitch Type

  return lln;
} // end of GetBigint

/***********************************************************************/
/* Return the Value's Double value.                                    */
/***********************************************************************/
double JVALUE::GetFloat(void) {
  double d;

  switch (DataType) {
  case TYPE_DBL:  d = F;               break;
  case TYPE_BINT: d = (double)LLn;     break;
  case TYPE_INTG: d = (double)N;       break;
  case TYPE_DTM:
  case TYPE_STRG: d = atof(Strp);      break;
  case TYPE_BOOL: d = (B) ? 1.0 : 0.0; break;
  default:
    d = 0.0;
  } // endswitch Type

  return d;
} // end of GetFloat

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ JVALUE::GetString(PGLOBAL g, char* buff) {
  char  buf[32];
  char* p = (buff) ? buff : buf;

  switch (DataType) {
  case TYPE_DTM:
  case TYPE_STRG:
    p = Strp;
    break;
  case TYPE_INTG:
    sprintf(p, "%d", N);
    break;
  case TYPE_BINT:
    sprintf(p, "%lld", LLn);
    break;
  case TYPE_DBL:
    sprintf(p, "%.*lf", Nd, F);
    break;
  case TYPE_BOOL:
    p = (char*)((B) ? "true" : "false");
    break;
  case TYPE_NULL:
    p = (char*)"null";
    break;
  default:
    p = NULL;
  } // endswitch Type


  return (p == buf) ? (char*)PlugDup(g, buf) : p;
} // end of GetString

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ JVALUE::GetText(PGLOBAL g, PSTRG text) {
  if (DataType == TYPE_JSON)
    return Jsp->GetText(g, text);

  char buff[32];
  PSZ  s = (DataType == TYPE_NULL) ? NULL : GetString(g, buff);

  if (s)
    text->Append(s);
  else if (GetJsonNull())
    text->Append(GetJsonNull());

  return NULL;
} // end of GetText

void JVALUE::SetValue(PJSON jsp) {
  if (DataType == TYPE_JSON && jsp->GetType() == TYPE_JVAL) {
    Jsp = jsp->GetJsp();
    Nd = ((PJVAL)jsp)->Nd;
    DataType = ((PJVAL)jsp)->DataType;
    //  Val = ((PJVAL)jsp)->GetVal();
  } else {
    Jsp = jsp;
    DataType = TYPE_JSON;
  } // endif Type

} // end of SetValue;

void JVALUE::SetValue(PGLOBAL g, PVAL valp) {
  //if (!Val)
  //  Val = AllocVal(g, TYPE_VAL);

  if (!valp || valp->IsNull()) {
    DataType = TYPE_NULL;
  } else switch (valp->GetType()) {
  case TYPE_DATE:
    if (((DTVAL*)valp)->IsFormatted())
      Strp = valp->GetCharValue();
    else {
      char buf[32];

      Strp = PlugDup(g, valp->GetCharString(buf));
    }	// endif Formatted

    DataType = TYPE_DTM;
    break;
  case TYPE_STRING:
    Strp = valp->GetCharValue();
    DataType = TYPE_STRG;
    break;
  case TYPE_DOUBLE:
  case TYPE_DECIM:
    F = valp->GetFloatValue();

    if (IsTypeNum(valp->GetType()))
      Nd = valp->GetValPrec();

    DataType = TYPE_DBL;
    break;
  case TYPE_TINY:
    B = valp->GetTinyValue() != 0;
    DataType = TYPE_BOOL;
  case TYPE_INT:
    N = valp->GetIntValue();
    DataType = TYPE_INTG;
    break;
  case TYPE_BIGINT:
    LLn = valp->GetBigintValue();
    DataType = TYPE_BINT;
    break;
  default:
    sprintf(g->Message, "Unsupported typ %d\n", valp->GetType());
    throw(777);
  } // endswitch Type

} // end of SetValue

/***********************************************************************/
/* Set the Value's value as the given integer.                         */
/***********************************************************************/
void JVALUE::SetInteger(PGLOBAL g, int n) {
  N = n;
  DataType = TYPE_INTG;
} // end of SetInteger

/***********************************************************************/
/* Set the Value's Boolean value as a tiny integer.                    */
/***********************************************************************/
void JVALUE::SetBool(PGLOBAL g, bool b) {
  B = b;
  DataType = TYPE_BOOL;
} // end of SetTiny

/***********************************************************************/
/* Set the Value's value as the given big integer.                     */
/***********************************************************************/
void JVALUE::SetBigint(PGLOBAL g, long long ll) {
  LLn = ll;
  DataType = TYPE_BINT;
} // end of SetBigint

/***********************************************************************/
/* Set the Value's value as the given DOUBLE.                          */
/***********************************************************************/
void JVALUE::SetFloat(PGLOBAL g, double f) {
  F = f;
  Nd = 6;
  DataType = TYPE_DBL;
} // end of SetFloat

/***********************************************************************/
/* Set the Value's value as the given string.                          */
/***********************************************************************/
void JVALUE::SetString(PGLOBAL g, PSZ s, int ci) {
  Strp = s;
  Nd = ci;
  DataType = TYPE_STRG;
} // end of SetString

/***********************************************************************/
/* True when its JSON or normal value is null.                         */
/***********************************************************************/
bool JVALUE::IsNull(void) {
  return (DataType == TYPE_JSON) ? Jsp->IsNull() : DataType == TYPE_NULL;
} // end of IsNull
#endif // 0
