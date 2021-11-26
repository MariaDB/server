/*************** json CPP Declares Source Code File (.H) ***************/
/*  Name: json.cpp   Version 1.6                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2021  */
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
#include "json.h"

#define ARGS       MY_MIN(24,(int)len-i),s+MY_MAX(i-3,0)

#if defined(_WIN32)
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

void trans_func(unsigned int u, _EXCEPTION_POINTERS* pExp)
{
  throw SE_Exception(u, pExp->ExceptionRecord);
} // end of trans_func

char *GetExceptionDesc(PGLOBAL g, unsigned int e);
#endif   // SE_CATCH

char *GetJsonNull(void);
int   GetDefaultPrec(void);
int   PrepareColist(char*);

/***********************************************************************/
/* IsNum: check whether this string is all digits.                     */
/***********************************************************************/
bool IsNum(PSZ s)
{
  char* p = s;

  if (*p == '-')
    p++;

  if (*p == ']')
    return false;
  else for (; *p; p++)
    if (*p == ']')
      break;
    else if (!isdigit(*p))
      return false;

  return true;
} // end of IsNum

/***********************************************************************/
/* IsArray: check whether this is a Mongo array path.                  */
/***********************************************************************/
bool IsArray(PSZ s)
{
  char* p = s;

  if (!p || !*p)
    return false;
  else for (; *p; p++)
    if (*p == '.')
      break;
    else if (!isdigit(*p))
      return false;

  return true;
} // end of IsArray

/***********************************************************************/
/* NextChr: return the first found '[' or Sep pointer.                 */
/***********************************************************************/
char* NextChr(PSZ s, char sep)
{
  char* p1 = strchr(s, '[');
  char* p2 = strchr(s, sep);

  if (!p2)
    return p1;
  else if (p1)
    return MY_MIN(p1, p2);

  return p2;
} // end of NextChr

/***********************************************************************/
/* Stringified: check that this column is in the stringified list.     */
/***********************************************************************/
bool Stringified(PCSZ strfy, char *colname)
{
  if (strfy) {
    char *p, colist[512];
    int   n;

    strncpy(colist, strfy, sizeof(colist) - 1);
    n = PrepareColist(colist); 

    for (p = colist; n && p; p += (strlen(p) + 1), n--)
      if (!stricmp(p, colname))
        return true;

  } // endif strfy

  return false;
} // end of Stringified

#if 0
/***********************************************************************/
/* Allocate a VAL structure, make sure common field and Nd are zeroed. */
/***********************************************************************/
PVL AllocVal(PGLOBAL g, JTYP type)
{
  PVL vlp = (PVL)PlugSubAlloc(g, NULL, sizeof(VAL));

  vlp->LLn = 0;
  vlp->Nd = 0;
  vlp->Type = type;
  return vlp;
} // end of AllocVal
#endif // 0

/***********************************************************************/
/* Parse a json string.                                                */
/* Note: when pretty is not known, the caller set pretty to 3.         */
/***********************************************************************/
PJSON ParseJson(PGLOBAL g, char* s, size_t len, int* ptyp, bool* comma)
{
  int   i, pretty = (ptyp) ? *ptyp : 3;
  bool  b = false, pty[3] = { true,true,true };
  PJSON jsp = NULL;
  PJDOC jdp = NULL;

  if (trace(1))
    htrc("ParseJson: s=%.10s len=%zd\n", s, len);

  if (!s || !len) {
    strcpy(g->Message, "Void JSON object");
    return NULL;
  } else if (comma)
    *comma = false;

  // Trying to guess the pretty format
  if (s[0] == '[' && (s[1] == '\n' || (s[1] == '\r' && s[2] == '\n')))
    pty[0] = false;

  try {
    jdp = new(g) JDOC;
    jdp->s = s;
    jdp->len = len;
    jdp->pty = pty;

    for (i = 0; i < jdp->len; i++)
      switch (s[i]) {
      case '[':
        if (jsp)
          jsp = jdp->ParseAsArray(g, i, pretty, ptyp);
        else
          jsp = jdp->ParseArray(g, ++i);

        break;
      case '{':
        if (jsp)
          jsp = jdp->ParseAsArray(g, i, pretty, ptyp);
        else if (!(jsp = jdp->ParseObject(g, ++i)))
          throw 2;

        break;
      case ' ':
      case '\t':
      case '\n':
      case '\r':
        break;
      case ',':
        if (jsp && (pretty == 1 || pretty == 3)) {
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
        /* falls through */
      default:
        if (jsp)
          jsp = jdp->ParseAsArray(g, i, pretty, ptyp);
        else if (!(jsp = jdp->ParseValue(g, i)))
          throw 4;

        break;
      }; // endswitch s[i]

    if (!jsp)
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
    jsp = NULL;
  } catch (const char* msg) {
    strcpy(g->Message, msg);
    jsp = NULL;
  } // end catch

  return jsp;
} // end of ParseJson

/***********************************************************************/
/* Serialize a JSON document tree:                                     */
/***********************************************************************/
PSZ Serialize(PGLOBAL g, PJSON jsp, char* fn, int pretty) {
  PSZ   str = NULL;
  bool  b = false, err = true;
  JOUT* jp;
  FILE* fs = NULL;
  PJDOC jdp = NULL;

  g->Message[0] = 0;

  try {
    jdp = new(g) JDOC; // MUST BE ALLOCATED BEFORE jp !!!!!
    jdp->dfp = GetDefaultPrec();

    if (!jsp) {
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

    jdp->SetJp(jp);

    switch (jsp->GetType()) {
    case TYPE_JAR:
      err = jdp->SerializeArray((PJAR)jsp, b);
      break;
    case TYPE_JOB:
      err = ((b && jp->Prty()) && jp->WriteChr('\t'));
      err |= jdp->SerializeObject((PJOB)jsp);
      break;
    case TYPE_JVAL:
      err = jdp->SerializeValue((PJVAL)jsp);
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


/* -------------------------- Class JOUTSTR -------------------------- */

/***********************************************************************/
/* JOUTSTR constructor.                                                */
/***********************************************************************/
JOUTSTR::JOUTSTR(PGLOBAL g) : JOUT(g) {
  PPOOLHEADER pph = (PPOOLHEADER)g->Sarea;

  N = 0;
  Max = pph->FreeBlk;
  Max = (Max > 32) ? Max - 32 : Max;
  Strp = (char*)PlugSubAlloc(g, NULL, 0);  // Size not know yet
} // end of JOUTSTR constructor

/***********************************************************************/
/* Concatenate a string to the Serialize string.                       */
/***********************************************************************/
bool JOUTSTR::WriteStr(const char* s) {
  if (s) {
    size_t len = strlen(s);

    if (N + len > Max)
      return true;

    memcpy(Strp + N, s, len);
    N += len;
    return false;
  } else
    return true;

} // end of WriteStr

/***********************************************************************/
/* Concatenate a character to the Serialize string.                    */
/***********************************************************************/
bool JOUTSTR::WriteChr(const char c) {
  if (N + 1 > Max)
    return true;

  Strp[N++] = c;
  return false;
} // end of WriteChr

/***********************************************************************/
/* Escape and Concatenate a string to the Serialize string.            */
/***********************************************************************/
bool JOUTSTR::Escape(const char* s)
{
  if (s) {
    WriteChr('"');

    for (unsigned int i = 0; s[i]; i++)
      switch (s[i]) {
        case '"':
        case '\\':
        case '\t':
        case '\n':
        case '\r':
        case '\b':
        case '\f': WriteChr('\\');
          // fall through
        default:
          WriteChr(s[i]);
          break;
      } // endswitch s[i]

    WriteChr('"');
  } else
    WriteStr("null");

  return false;
} // end of Escape

/* ------------------------- Class JOUTFILE -------------------------- */

/***********************************************************************/
/* Write a string to the Serialize file.                               */
/***********************************************************************/
bool JOUTFILE::WriteStr(const char* s)
{
  // This is temporary
  fputs(s, Stream);
  return false;
} // end of WriteStr

/***********************************************************************/
/* Write a character to the Serialize file.                            */
/***********************************************************************/
bool JOUTFILE::WriteChr(const char c)
{
  // This is temporary
  fputc(c, Stream);
  return false;
} // end of WriteChr

/***********************************************************************/
/* Escape and Concatenate a string to the Serialize string.            */
/***********************************************************************/
bool JOUTFILE::Escape(const char* s)
{
  // This is temporary
  if (s) {
    fputc('"', Stream);

    for (unsigned int i = 0; s[i]; i++)
      switch (s[i]) {
        case '"':  fputs("\\\"", Stream); break;
        case '\\': fputs("\\\\", Stream); break;
        case '\t': fputs("\\t", Stream); break;
        case '\n': fputs("\\n", Stream); break;
        case '\r': fputs("\\r", Stream); break;
        case '\b': fputs("\\b", Stream); break;
        case '\f': fputs("\\f", Stream); break;
        default:
          fputc(s[i], Stream);
          break;
      } // endswitch s[i]

    fputc('"', Stream);
  } else
    fputs("null", Stream);

  return false;
} // end of Escape

/* ------------------------- Class JOUTPRT --------------------------- */

/***********************************************************************/
/* Write a string to the Serialize pretty file.                        */
/***********************************************************************/
bool JOUTPRT::WriteStr(const char* s)
{
  // This is temporary
  if (B) {
    fputs(EL, Stream);
    M--;

    for (int i = 0; i < M; i++)
      fputc('\t', Stream);

    B = false;
  } // endif B

  fputs(s, Stream);
  return false;
} // end of WriteStr

/***********************************************************************/
/* Write a character to the Serialize pretty file.                     */
/***********************************************************************/
bool JOUTPRT::WriteChr(const char c)
{
  switch (c) {
  case ':':
    fputs(": ", Stream);
    break;
  case '{':
  case '[':
#if 0
    if (M)
      fputs(EL, Stream);

    for (int i = 0; i < M; i++)
      fputc('\t', Stream);
#endif // 0

    fputc(c, Stream);
    fputs(EL, Stream);
    M++;

    for (int i = 0; i < M; i++)
      fputc('\t', Stream);

    break;
  case '}':
  case ']':
    M--;
    fputs(EL, Stream);

    for (int i = 0; i < M; i++)
      fputc('\t', Stream);

    fputc(c, Stream);
    B = true;
    break;
  case ',':
    fputc(c, Stream);
    fputs(EL, Stream);

    for (int i = 0; i < M; i++)
      fputc('\t', Stream);

    B = false;
    break;
  default:
    fputc(c, Stream);
  } // endswitch c

  return false;
} // end of WriteChr

/* --------------------------- Class JDOC ---------------------------- */

/***********************************************************************/
/* Parse several items as being in an array.                           */
/***********************************************************************/
PJAR JDOC::ParseAsArray(PGLOBAL g, int& i, int pretty, int *ptyp)
{
  if (pty[0] && (!pretty || pretty > 2)) {
    PJAR jsp;

    if ((jsp = ParseArray(g, (i = 0))) && ptyp && pretty == 3)
      *ptyp = (pty[0]) ? 0 : 3;

    return jsp;
  } else
    strcpy(g->Message, "More than one item in file");

  return NULL;
} // end of ParseAsArray

/***********************************************************************/
/* Parse a JSON Array.                                                 */
/***********************************************************************/
PJAR JDOC::ParseArray(PGLOBAL g, int& i)
{
  int  level = 0;
  bool b = (!i);
  PJAR jarp = new(g) JARRAY;

  for (; i < len; i++)
    switch (s[i]) {
      case ',':
        if (level < 2) {
          sprintf(g->Message, "Unexpected ',' near %.*s",ARGS);
          throw 1;
        } else
          level = 1;

        break;
      case ']':
        if (level == 1) {
          sprintf(g->Message, "Unexpected ',]' near %.*s", ARGS);
          throw 1;
        } // endif level

        jarp->InitArray(g);
        return jarp;
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
        } else
          jarp->AddArrayValue(g, ParseValue(g, i));

        level = (b) ? 1 : 2;
        break;
    }; // endswitch s[i]

  if (b) {
    // Case of Pretty == 0
    jarp->InitArray(g);
    return jarp;
  } // endif b

  throw ("Unexpected EOF in array");
} // end of ParseArray

/***********************************************************************/
/* Parse a JSON Object.                                                */
/***********************************************************************/
PJOB JDOC::ParseObject(PGLOBAL g, int& i)
{
  PSZ   key;
  int   level = -1;
  PJOB  jobp = new(g) JOBJECT;
  PJPR  jpp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
      case '"':
        if (level < 2) {
          key = ParseString(g, ++i);
          jpp = jobp->AddPair(g, key);
          level = 1;
        } else {
          sprintf(g->Message, "misplaced string near %.*s", ARGS);
          throw 2;
        } // endif level

        break;
      case ':':
        if (level == 1) {
          jpp->Val = ParseValue(g, ++i);
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
        if (level == 0 || level == 1) {
          sprintf(g->Message, "Unexpected '}' near %.*s", ARGS);
          throw 2;
          } // endif level

        return jobp;
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
PJVAL JDOC::ParseValue(PGLOBAL g, int& i)
{
  PJVAL jvp = new(g) JVALUE;

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
      jvp->Jsp = ParseArray(g, ++i);
      jvp->DataType = TYPE_JSON;
      break;
    case '{':
      jvp->Jsp = ParseObject(g, ++i);
      jvp->DataType = TYPE_JSON;
      break;
    case '"':
//    jvp->Val = AllocVal(g, TYPE_STRG);
      jvp->Strp = ParseString(g, ++i);
      jvp->DataType = TYPE_STRG;
      break;
    case 't':
      if (!strncmp(s + i, "true", 4)) {
//      jvp->Val = AllocVal(g, TYPE_BOOL);
        jvp->B = true;
        jvp->DataType = TYPE_BOOL;
        i += 3;
      } else
        goto err;

      break;
    case 'f':
      if (!strncmp(s + i, "false", 5)) {
//      jvp->Val = AllocVal(g, TYPE_BOOL);
        jvp->B = false;
        jvp->DataType = TYPE_BOOL;
        i += 4;
      } else
        goto err;

      break;
    case 'n':
      if (!strncmp(s + i, "null", 4)) {
        jvp->DataType = TYPE_NULL;
        i += 3;
      } else
        goto err;

      break;
    case '-':
    default:
      if (s[i] == '-' || isdigit(s[i]))
        ParseNumeric(g, i, jvp);
      else
        goto err;

    }; // endswitch s[i]

  return jvp;

err:
  sprintf(g->Message, "Unexpected character '%c' near %.*s", s[i], ARGS);
  throw 3;
} // end of ParseValue

/***********************************************************************/
/*  Unescape and parse a JSON string.                                  */
/***********************************************************************/
char *JDOC::ParseString(PGLOBAL g, int& i)
{
  uchar *p;
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
        return (char*)p;
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
                  p[n]   = (uchar)(0x80 | (hex & 0x3F));
                } else if (hex < 0x10000) {
                  p[n++] = (uchar)(0xE0 | (hex >> 12));
                  p[n++] = (uchar)(0x80 | ((hex >> 6) & 0x3f));
                  p[n]   = (uchar)(0x80 | (hex & 0x3f));
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

          } else switch(s[i]) {
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
void JDOC::ParseNumeric(PGLOBAL g, int& i, PJVAL vlp)
{
  char  buf[50];
  int   n = 0;
  short nd = 0;
  bool  has_dot = false;
  bool  has_e = false;
  bool  found_digit = false;
//PVL   vlp = NULL;

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

//    vlp = AllocVal(g, TYPE_DBL);
      vlp->F = dv;
      vlp->Nd = nd;
      vlp->DataType = TYPE_DBL;
    } else {
      long long iv = strtoll(buf, NULL, 10);

      if (iv > INT_MAX32 || iv < INT_MIN32) {
//      vlp = AllocVal(g, TYPE_BINT);
        vlp->LLn = iv;
        vlp->DataType = TYPE_BINT;
      } else {
//      vlp = AllocVal(g, TYPE_INTG);
        vlp->N = (int)iv;
        vlp->DataType = TYPE_INTG;
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
/* Serialize a JSON Array.                                             */
/***********************************************************************/
bool JDOC::SerializeArray(PJAR jarp, bool b)
{
  bool first = true;

  if (b) {
    if (js->Prty()) {
      if (js->WriteChr('['))
        return true;
      else if (js->Prty() == 1 && (js->WriteStr(EL) || js->WriteChr('\t')))
        return true;

    } // endif Prty

  } else if (js->WriteChr('['))
    return true;

  for (int i = 0; i < jarp->size(); i++) {
    if (first)
      first = false;
    else if ((!b || js->Prty()) && js->WriteChr(','))
      return true;
    else if (b) {
      if (js->Prty() < 2 && js->WriteStr(EL))
        return true;
      else if (js->Prty() == 1 && js->WriteChr('\t'))
        return true;

    } // endif b

    if (SerializeValue(jarp->GetArrayValue(i)))
      return true;

    } // endfor i

  if (b && js->Prty() == 1 && js->WriteStr(EL))
    return true;

  return ((!b || js->Prty()) && js->WriteChr(']'));
} // end of SerializeArray

/***********************************************************************/
/* Serialize a JSON Object.                                            */
/***********************************************************************/
bool JDOC::SerializeObject(PJOB jobp)
{
  bool first = true;

  if (js->WriteChr('{'))
    return true;

  for (PJPR pair = jobp->GetFirst(); pair; pair = pair->Next) {
    if (first)
      first = false;
    else if (js->WriteChr(','))
      return true;

    if (js->WriteChr('"') ||
        js->WriteStr(pair->Key) ||
        js->WriteChr('"') ||
        js->WriteChr(':') ||
        SerializeValue(pair->Val))
      return true;

    } // endfor i

  return js->WriteChr('}');
} // end of SerializeObject

/***********************************************************************/
/* Serialize a JSON Value.                                             */
/***********************************************************************/
bool JDOC::SerializeValue(PJVAL jvp)
{
  char buf[64];
  PJAR jap;
  PJOB jop;
  //PVL  vlp;

  if ((jap = jvp->GetArray()))
    return SerializeArray(jap, false);
  else if ((jop = jvp->GetObject()))
    return SerializeObject(jop);
//else if (!(vlp = jvp->Val))
//  return js->WriteStr("null");
  else switch (jvp->DataType) {
    case TYPE_BOOL:
      return js->WriteStr(jvp->B ? "true" : "false");
    case TYPE_STRG:
    case TYPE_DTM:
      return js->Escape(jvp->Strp);
    case TYPE_INTG:
      sprintf(buf, "%d", jvp->N);
      return js->WriteStr(buf);
    case TYPE_BINT:
      sprintf(buf, "%lld", jvp->LLn);
      return js->WriteStr(buf);
    case TYPE_DBL:  // dfp to limit to the default number of decimals
      sprintf(buf, "%.*f", MY_MIN(jvp->Nd, dfp), jvp->F);
      return js->WriteStr(buf);
    case TYPE_NULL:
      return js->WriteStr("null");
    default:
      return js->WriteStr("???");   // TODO
  } // endswitch Type

  strcpy(js->g->Message, "Unrecognized value");
  return true;
} // end of SerializeValue

/* -------------------------- Class JOBJECT -------------------------- */

/***********************************************************************/
/* Return the number of pairs in this object.                          */
/***********************************************************************/
int JOBJECT::GetSize(bool b) {
  int n = 0;

  for (PJPR jpp = First; jpp; jpp = jpp->Next)
    // If b return only non null pairs
    if (!b || (jpp->Val && !jpp->Val->IsNull()))
      n++;

  return n;
} // end of GetSize

/***********************************************************************/
/* Add a new pair to an Object.                                        */
/***********************************************************************/
PJPR JOBJECT::AddPair(PGLOBAL g, PCSZ key)
{
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
PJAR JOBJECT::GetKeyList(PGLOBAL g)
{
  PJAR jarp = new(g) JARRAY();

  for (PJPR jpp = First; jpp; jpp = jpp->Next)
    jarp->AddArrayValue(g, new(g) JVALUE(g, jpp->Key));

  jarp->InitArray(g);
  return jarp;
} // end of GetKeyList

/***********************************************************************/
/* Return all values as an array.                                      */
/***********************************************************************/
PJAR JOBJECT::GetValList(PGLOBAL g)
{
  PJAR jarp = new(g) JARRAY();

  for (PJPR jpp = First; jpp; jpp = jpp->Next)
    jarp->AddArrayValue(g, jpp->Val);

  jarp->InitArray(g);
  return jarp;
} // end of GetValList

/***********************************************************************/
/* Get the value corresponding to the given key.                       */
/***********************************************************************/
PJVAL JOBJECT::GetKeyValue(const char* key)
{
  for (PJPR jp = First; jp; jp = jp->Next)
    if (!strcmp(jp->Key, key))
      return jp->Val;

  return NULL;
} // end of GetValue;

/***********************************************************************/
/* Return the text corresponding to all keys (XML like).               */
/***********************************************************************/
PSZ JOBJECT::GetText(PGLOBAL g, PSTRG text)
{
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
bool JOBJECT::Merge(PGLOBAL g, PJSON jsp)
{
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
void JOBJECT::SetKeyValue(PGLOBAL g, PJVAL jvp, PCSZ key)
{
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
void JOBJECT::DeleteKey(PCSZ key)
{
  PJPR jp, *pjp = &First;

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
bool JOBJECT::IsNull(void)
{
  for (PJPR jp = First; jp; jp = jp->Next)
    if (!jp->Val->IsNull())
      return false;

  return true;
} // end of IsNull

/* -------------------------- Class JARRAY --------------------------- */

/***********************************************************************/
/* JARRAY constructor.                                                 */
/***********************************************************************/
JARRAY::JARRAY(void) : JSON()
{
	Type = TYPE_JAR;  
	Size = 0; 
	Alloc = 0; 
	First = Last = NULL; 
	Mvals = NULL;
}	// end of JARRAY constructor

/***********************************************************************/
/* Return the number of values in this object.                         */
/***********************************************************************/
int JARRAY::GetSize(bool b)
{
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
void JARRAY::InitArray(PGLOBAL g)
{
  int   i;
  PJVAL jvp, *pjvp = &First;

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
PJVAL JARRAY::GetArrayValue(int i)
{
  if (Mvals && i >= 0 && i < Size)
    return Mvals[i];
  else if (Mvals && i < 0 && i >= -Size)
    return Mvals[Size + i];
  else
    return NULL;
} // end of GetValue

/***********************************************************************/
/* Add a Value to the Array Value list.                                */
/***********************************************************************/
PJVAL JARRAY::AddArrayValue(PGLOBAL g, PJVAL jvp, int *x)
{
  if (!jvp)
    jvp = new(g) JVALUE;

  if (x) {
    int   i = 0, n = *x;
    PJVAL jp, *jpp = &First;

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
bool JARRAY::Merge(PGLOBAL g, PJSON jsp)
{
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
/* Set the nth Value of the Array Value list or add it.                */
/***********************************************************************/
void JARRAY::SetArrayValue(PGLOBAL g, PJVAL jvp, int n)
{
  int   i = 0;
  PJVAL jp, *jpp = &First;

  for (jp = First; i < n; i++, jp = *(jpp = &jp->Next))
    if (!jp)
      *jpp = jp = new(g) JVALUE;

  *jpp = jvp;
  jvp->Next = (jp ? jp->Next : NULL);
} // end of SetValue

/***********************************************************************/
/* Return the text corresponding to all values.                        */
/***********************************************************************/
PSZ JARRAY::GetText(PGLOBAL g, PSTRG text)
{
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
bool JARRAY::DeleteValue(int n)
{
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
bool JARRAY::IsNull(void)
{
  for (int i = 0; i < Size; i++)
    if (!Mvals[i]->IsNull())
      return false;

  return true;
} // end of IsNull

/* -------------------------- Class JVALUE- -------------------------- */

/***********************************************************************/
/* Constructor for a JVALUE.                                           */
/***********************************************************************/
JVALUE::JVALUE(PJSON jsp) : JSON()
{
  if (jsp && jsp->GetType() == TYPE_JVAL) {
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
    DataType = Jsp ? TYPE_JSON : TYPE_NULL;
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
JVALUE::JVALUE(PGLOBAL g, PVL vlp) : JSON()
{
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
JVALUE::JVALUE(PGLOBAL g, PCSZ strp) : JSON()
{
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
void JVALUE::Clear(void)
{
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
JTYP JVALUE::GetValType(void)
{
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
PJOB JVALUE::GetObject(void)
{
  if (DataType == TYPE_JSON && Jsp->GetType() == TYPE_JOB)
    return (PJOB)Jsp;

  return NULL;
} // end of GetObject

/***********************************************************************/
/* Return the Value's Array value.                                     */
/***********************************************************************/
PJAR JVALUE::GetArray(void)
{
  if (DataType == TYPE_JSON && Jsp->GetType() == TYPE_JAR)
    return (PJAR)Jsp;

  return NULL;
} // end of GetArray

/***********************************************************************/
/* Return the Value's as a Value class.                                */
/***********************************************************************/
PVAL JVALUE::GetValue(PGLOBAL g)
{
  PVAL valp = NULL;

  if (DataType != TYPE_JSON)
  {
    if (DataType == TYPE_STRG)
      valp = AllocateValue(g, Strp, DataType, Nd);
    else
      valp = AllocateValue(g, &LLn, DataType, Nd);
  }

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
long long JVALUE::GetBigint(void)
{
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
double JVALUE::GetFloat(void)
{
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
PSZ JVALUE::GetString(PGLOBAL g, char *buff)
{
  char  buf[32];
  char *p = (buff) ? buff : buf;

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
PSZ JVALUE::GetText(PGLOBAL g, PSTRG text)
{
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

void JVALUE::SetValue(PJSON jsp)
{
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

void JVALUE::SetValue(PGLOBAL g, PVAL valp)
{
//if (!Val)
//  Val = AllocVal(g, TYPE_VAL);

  if (!valp || valp->IsNull()) {
    DataType = TYPE_NULL;
  } else switch (valp->GetType()) {
  case TYPE_DATE:
		if (((DTVAL*)valp)->IsFormatted())
			Strp = PlugDup(g, valp->GetCharValue());
		else {
			char buf[32];

			Strp = PlugDup(g, valp->GetCharString(buf));
		}	// endif Formatted

		DataType = TYPE_DTM;
		break;
	case TYPE_STRING:
    Strp = PlugDup(g, valp->GetCharValue());
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
    break;
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
void JVALUE::SetInteger(PGLOBAL g, int n)
{
  N = n;
  DataType = TYPE_INTG;
} // end of SetInteger

/***********************************************************************/
/* Set the Value's Boolean value as a tiny integer.                    */
/***********************************************************************/
void JVALUE::SetBool(PGLOBAL g, bool b)
{
  B = b;
  DataType = TYPE_BOOL;
} // end of SetTiny

/***********************************************************************/
/* Set the Value's value as the given big integer.                     */
/***********************************************************************/
void JVALUE::SetBigint(PGLOBAL g, long long ll)
{
  LLn = ll;
  DataType = TYPE_BINT;
} // end of SetBigint

/***********************************************************************/
/* Set the Value's value as the given DOUBLE.                          */
/***********************************************************************/
void JVALUE::SetFloat(PGLOBAL g, double f)
{
  F = f;
  Nd = GetDefaultPrec();
  DataType = TYPE_DBL;
} // end of SetFloat

/***********************************************************************/
/* Set the Value's value as the given string.                          */
/***********************************************************************/
void JVALUE::SetString(PGLOBAL g, PSZ s, int ci)
{
  Strp = s;
  Nd = ci;
  DataType = TYPE_STRG;
} // end of SetString

/***********************************************************************/
/* True when its JSON or normal value is null.                         */
/***********************************************************************/
bool JVALUE::IsNull(void)
{
  return (DataType == TYPE_JSON) ? Jsp->IsNull() : DataType == TYPE_NULL;
} // end of IsNull


/* ---------------------------- Class SWAP --------------------------- */

/***********************************************************************/
/* Replace all pointers by offsets or the opposite.                    */
/***********************************************************************/
void SWAP::SwapJson(PJSON jsp, bool move)
{
  if (move)
    MoffJson(jsp);
  else
    MptrJson((PJSON)MakeOff(Base, jsp));

  return;
} // end of SwapJson

/***********************************************************************/
/* Replace all pointers by offsets.                                    */
/***********************************************************************/
size_t SWAP::MoffJson(PJSON jsp) {
  size_t res = 0;

  if (jsp)
    switch (jsp->Type) {
    case TYPE_JAR:
      res = MoffArray((PJAR)jsp);
      break;
    case TYPE_JOB:
      res = MoffObject((PJOB)jsp);
      break;
    case TYPE_JVAL:
      res = MoffJValue((PJVAL)jsp);
      break;
    default:
      throw "Invalid json tree";
    } // endswitch Type

  return res;
} // end of MoffJson

/***********************************************************************/
/* Replace all array pointers by offsets.                              */
/***********************************************************************/
size_t SWAP::MoffArray(PJAR jarp)
{
  if (jarp->First) {
    for (int i = 0; i < jarp->Size; i++)
      jarp->Mvals[i] = (PJVAL)MakeOff(Base, jarp->Mvals[i]);

    jarp->Mvals = (PJVAL*)MakeOff(Base, jarp->Mvals);
    jarp->First = (PJVAL)MoffJValue(jarp->First);
    jarp->Last = (PJVAL)MakeOff(Base, jarp->Last);
  } // endif First

  return MakeOff(Base, jarp);
} // end of MoffArray

/***********************************************************************/
/* Replace all object pointers by offsets.                             */
/***********************************************************************/
size_t SWAP::MoffObject(PJOB jobp) {
  if (jobp->First) {
    jobp->First = (PJPR)MoffPair(jobp->First);
    jobp->Last = (PJPR)MakeOff(Base, jobp->Last);
  } // endif First

  return MakeOff(Base, jobp);
} // end of MoffObject

/***********************************************************************/
/* Replace all pair pointers by offsets.                               */
/***********************************************************************/
size_t SWAP::MoffPair(PJPR jpp) {
  jpp->Key = (PCSZ)MakeOff(Base, (void*)jpp->Key);

  if (jpp->Val)
    jpp->Val = (PJVAL)MoffJValue(jpp->Val);

  if (jpp->Next)
    jpp->Next = (PJPR)MoffPair(jpp->Next);

  return MakeOff(Base, jpp);
} // end of MoffPair

/***********************************************************************/
/* Replace all jason value pointers by offsets.                        */
/***********************************************************************/
size_t SWAP::MoffJValue(PJVAL jvp) {
  if (!jvp->Del) {
    if (jvp->DataType == TYPE_JSON)
      jvp->Jsp = (PJSON)MoffJson(jvp->Jsp);
    else if (jvp->DataType == TYPE_STRG)
      jvp->Strp = (PSZ)MakeOff(Base, (jvp->Strp));

//  if (jvp->Val)
//    jvp->Val = (PVL)MoffVal(jvp->Val);

  } // endif Del

  if (jvp->Next)
    jvp->Next = (PJVAL)MoffJValue(jvp->Next);

  return MakeOff(Base, jvp);
} // end of MoffJValue

#if 0
/***********************************************************************/
/* Replace string pointers by offset.                                  */
/***********************************************************************/
size_t SWAP::MoffVal(PVL vlp) {
  if (vlp->Type == TYPE_STRG)
    vlp->Strp = (PSZ)MakeOff(Base, (vlp->Strp));

  return MakeOff(Base, vlp);
} // end of MoffVal
#endif // 0

/***********************************************************************/
/* Replace all offsets by pointers.                                    */
/***********************************************************************/
PJSON SWAP::MptrJson(PJSON ojp) {      // ojp is an offset
  PJSON jsp = (PJSON)MakePtr(Base, (size_t)ojp);

  if (ojp)
    switch (jsp->Type) {
    case TYPE_JAR:
      jsp = MptrArray((PJAR)ojp);
      break;
    case TYPE_JOB:
      jsp = MptrObject((PJOB)ojp);
      break;
    case TYPE_JVAL:
      jsp = MptrJValue((PJVAL)ojp);
      break;
    default:
      throw "Invalid json tree";
    } // endswitch Type

  return jsp;
} // end of MptrJson

/***********************************************************************/
/* Replace all array offsets by pointers.                              */
/***********************************************************************/
PJAR SWAP::MptrArray(PJAR ojar) {
  PJAR jarp = (PJAR)MakePtr(Base, (size_t)ojar);

  jarp = (PJAR)new((long long)jarp) JARRAY(0);

  if (jarp->First) {
    jarp->Mvals = (PJVAL*)MakePtr(Base, (size_t)jarp->Mvals);

    for (int i = 0; i < jarp->Size; i++)
      jarp->Mvals[i] = (PJVAL)MakePtr(Base, (size_t)jarp->Mvals[i]);

    jarp->First = (PJVAL)MptrJValue(jarp->First);
    jarp->Last = (PJVAL)MakePtr(Base, (size_t)jarp->Last);
  } // endif First

  return jarp;
} // end of MptrArray

/***********************************************************************/
/* Replace all object offsets by pointers.                             */
/***********************************************************************/
PJOB SWAP::MptrObject(PJOB ojob) {
  PJOB jobp = (PJOB)MakePtr(Base, (size_t)ojob);

  jobp = (PJOB)new((long long)jobp) JOBJECT(0);

  if (jobp->First) {
    jobp->First = (PJPR)MptrPair(jobp->First);
    jobp->Last = (PJPR)MakePtr(Base, (size_t)jobp->Last);
  } // endif First

  return jobp;
} // end of MptrObject

/***********************************************************************/
/* Replace all pair offsets by pointers.                               */
/***********************************************************************/
PJPR SWAP::MptrPair(PJPR ojp) {
  PJPR jpp = (PJPR)MakePtr(Base, (size_t)ojp);

  jpp->Key = (PCSZ)MakePtr(Base, (size_t)jpp->Key);

  if (jpp->Val)
    jpp->Val = (PJVAL)MptrJValue(jpp->Val);

  if (jpp->Next)
    jpp->Next = (PJPR)MptrPair(jpp->Next);

  return jpp;
} // end of MptrPair

/***********************************************************************/
/* Replace all value offsets by pointers.                              */
/***********************************************************************/
PJVAL SWAP::MptrJValue(PJVAL ojv) {
  PJVAL jvp = (PJVAL)MakePtr(Base, (size_t)ojv);

  jvp = (PJVAL)new((long long)jvp) JVALUE(0);

  if (!jvp->Del) {
    if (jvp->DataType == TYPE_JSON)
      jvp->Jsp = (PJSON)MptrJson(jvp->Jsp);
    else if (jvp->DataType == TYPE_STRG)
      jvp->Strp = (PSZ)MakePtr(Base, (size_t)jvp->Strp);

//  if (jvp->Val)
//    jvp->Val = (PVL)MptrVal(jvp->Val);

  } // endif Del

  if (jvp->Next)
    jvp->Next = (PJVAL)MptrJValue(jvp->Next);

  return jvp;
} // end of MptrJValue

#if 0
/***********************************************************************/
/* Replace string offsets by a pointer.                                */
/***********************************************************************/
PVL SWAP::MptrVal(PVL ovl) {
  PVL vlp = (PVL)MakePtr(Base, (size_t)ovl);

  if (vlp->Type == TYPE_STRG)
    vlp->Strp = (PSZ)MakePtr(Base, (size_t)vlp->Strp);

  return vlp;
} // end of MptrValue
#endif // 0
