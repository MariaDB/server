/*************** json CPP Declares Source Code File (.H) ***************/
/*  Name: json.cpp   Version 1.2                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2014 - 2015  */
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

#define ARGS       MY_MIN(24,len-i),s+MY_MAX(i-3,0)

#if defined(__WIN__)
#define EL  "\r\n"
#else
#define EL  "\n"
#endif

/***********************************************************************/
/* Parse a json string.                                                */
/* Note: when pretty is not known, the caller set pretty to 3.         */
/***********************************************************************/
PJSON ParseJson(PGLOBAL g, char *s, int len, int *ptyp, bool *comma)
{
	int   i, rc, pretty = (ptyp) ? *ptyp : 3;
	bool  b = false, pty[3] = {true, true, true};
  PJSON jsp = NULL;
  STRG  src;

  if (!s || !len) {
    strcpy(g->Message, "Void JSON object");
    return NULL;
  } else if (comma)
    *comma = false;

  src.str = s;
  src.len = len;

	// Trying to guess the pretty format
	if (s[0] == '[' && (s[1] == '\n' || (s[1] == '\r' && s[2] == '\n')))
		pty[0] = false;

  // Save stack and allocation environment and prepare error return
  if (g->jump_level == MAX_JUMP) {
    strcpy(g->Message, MSG(TOO_MANY_JUMPS));
    return NULL;
    } // endif jump_level

  if ((rc= setjmp(g->jumper[++g->jump_level])) != 0) {
    goto err;
    } // endif rc

	for (i = 0; i < len; i++)
    switch (s[i]) {
      case '[':
        if (jsp)
					goto tryit;
        else if (!(jsp = ParseArray(g, ++i, src, pty)))
          goto err;

        break;
      case '{':
        if (jsp)
					goto tryit;
				else if (!(jsp = ParseObject(g, ++i, src, pty)))
          goto err;

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
        goto err;
      case '(':
        b = true;
        break;
      case ')':
        if (b) {
          b = false;
          break;
          } // endif b

      default:
				if (jsp)
					goto tryit;
				else if (!(jsp = ParseValue(g, i, src, pty)))
					goto err;

				break;
	}; // endswitch s[i]

	if (!jsp)
		sprintf(g->Message, "Invalid Json string '%.*s'", 50, s);
	else if (ptyp && pretty == 3) {
		*ptyp = 3;     // Not recognized pretty

		for (i = 0; i < 3; i++)
			if (pty[i]) {
				*ptyp = i;
				break;
			} // endif pty

	} // endif ptyp

  g->jump_level--;
  return jsp;

tryit:
	if (pty[0] && (!pretty || pretty > 2)) {
		if ((jsp = ParseArray(g, (i = 0), src, pty)) && ptyp && pretty == 3)
			*ptyp = (pty[0]) ? 0 : 3;

		g->jump_level--;
		return jsp;
	} else
		strcpy(g->Message, "More than one item in file");

err:
  g->jump_level--;
  return NULL;
} // end of ParseJson

/***********************************************************************/
/* Parse a JSON Array.                                                 */
/***********************************************************************/
PJAR ParseArray(PGLOBAL g, int& i, STRG& src, bool *pty)
{
  char  *s = src.str;
  int    len = src.len;
  int    level = 0;
	bool   b = (!i);
  PJAR   jarp = new(g) JARRAY;
  PJVAL  jvp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
      case ',':
        if (level < 2) {
          sprintf(g->Message, "Unexpected ',' near %.*s",ARGS);
          return NULL;
        } else
          level = 1;

        break;
      case ']':
        if (level == 1) {
          sprintf(g->Message, "Unexpected ',]' near %.*s", ARGS);
          return NULL;
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
          return NULL;
        } else if ((jvp = ParseValue(g, i, src, pty)))
          jarp->AddValue(g, jvp);
        else
          return NULL;

        level = (b) ? 1 : 2;
        break;
    }; // endswitch s[i]

	if (b) {
		// Case of Pretty == 0
		jarp->InitArray(g);
		return jarp;
	} // endif b

  strcpy(g->Message, "Unexpected EOF in array");
  return NULL;
} // end of ParseArray

/***********************************************************************/
/* Parse a JSON Object.                                                */
/***********************************************************************/
PJOB ParseObject(PGLOBAL g, int& i, STRG& src, bool *pty)
{
  PSZ   key;
  char *s = src.str;
  int   len = src.len;
  int   level = 0;
  PJOB  jobp = new(g) JOBJECT;
  PJPR  jpp = NULL;

  for (; i < len; i++)
    switch (s[i]) {
      case '"':
        if (level < 2) {
          if ((key = ParseString(g, ++i, src))) {
            jpp = jobp->AddPair(g, key);
            level = 1;
          } else
            return NULL;

        } else {
          sprintf(g->Message, "misplaced string near %.*s", ARGS);
          return NULL;
        } // endif level

        break;
      case ':':
        if (level == 1) {
          if (!(jpp->Val = ParseValue(g, ++i, src, pty)))
            return NULL;

          level = 2;
        } else {
          sprintf(g->Message, "Unexpected ':' near %.*s", ARGS);
          return NULL;
        } // endif level

        break;
      case ',':
        if (level < 2) {
          sprintf(g->Message, "Unexpected ',' near %.*s", ARGS);
          return NULL;
        } else
          level = 1;

        break;
      case '}':
        if (level == 1) {
          sprintf(g->Message, "Unexpected '}' near %.*s", ARGS);
          return NULL;
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
        return NULL;
    }; // endswitch s[i]

  strcpy(g->Message, "Unexpected EOF in Object");
  return NULL;
} // end of ParseObject

/***********************************************************************/
/* Parse a JSON Value.                                                 */
/***********************************************************************/
PJVAL ParseValue(PGLOBAL g, int& i, STRG& src, bool *pty)
{
  char *strval, *s = src.str;
  int   n, len = src.len;
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
      if (!(jvp->Jsp = ParseArray(g, ++i, src, pty)))
        return NULL;

      break;
    case '{':
      if (!(jvp->Jsp = ParseObject(g, ++i, src, pty)))
        return NULL;

      break;
    case '"':
      if ((strval = ParseString(g, ++i, src)))
        jvp->Value = AllocateValue(g, strval, TYPE_STRING);
      else
        return NULL;

      break;
    case 't':
      if (!strncmp(s + i, "true", 4)) {
        n = 1;
        jvp->Value = AllocateValue(g, &n, TYPE_TINY);
        i += 3;
      } else
        goto err;

      break;
    case 'f':
      if (!strncmp(s + i, "false", 5)) {
        n = 0;
        jvp->Value = AllocateValue(g, &n, TYPE_TINY);
        i += 4;
      } else
        goto err;

      break;
    case 'n':
      if (!strncmp(s + i, "null", 4))
        i += 3;
      else
        goto err;

      break;
    case '-':
    default:
      if (s[i] == '-' || isdigit(s[i])) {
        if (!(jvp->Value = ParseNumeric(g, i, src)))
          goto err;

      } else
        goto err;

    }; // endswitch s[i]

  return jvp;

err:
  sprintf(g->Message, "Unexpected character '%c' near %.*s",
          s[i], ARGS);
  return NULL;
} // end of ParseValue

/***********************************************************************/
/*  Unescape and parse a JSON string.                                  */
/***********************************************************************/
char *ParseString(PGLOBAL g, int& i, STRG& src)
{
  char  *s = src.str;
  uchar *p;
  int    n = 0, len = src.len;

  // Be sure of memory availability
  if (len + 1 - i > (signed)((PPOOLHEADER)g->Sarea)->FreeBlk) {
    strcpy(g->Message, "ParseString: Out of memory");
    return NULL;
    } // endif len

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
  strcpy(g->Message, "Unexpected EOF in String");
  return NULL;
} // end of ParseString

/***********************************************************************/
/* Parse a JSON numeric value.                                         */
/***********************************************************************/
PVAL ParseNumeric(PGLOBAL g, int& i, STRG& src)
{
  char *s = src.str, buf[50];
  int   n = 0, len = src.len;
  short nd = 0;
	bool  has_dot = false;
	bool  has_e = false;
	bool  found_digit = false;
  PVAL  valp = NULL;

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

        // passthru
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

      valp = AllocateValue(g, &dv, TYPE_DOUBLE, nd);
    } else {
      long long iv = strtoll(buf, NULL, 10);

      valp = AllocateValue(g, &iv, TYPE_BIGINT);
    } // endif has

    i--;  // Unstack  following character
    return valp;
  } else {
    strcpy(g->Message, "No digit found");
    return NULL;
  } // endif found_digit

 err:
  strcpy(g->Message, "Unexpected EOF in number");
  return NULL;
} // end of ParseNumeric

/***********************************************************************/
/* Serialize a JSON tree:                                              */
/***********************************************************************/
PSZ Serialize(PGLOBAL g, PJSON jsp, char *fn, int pretty)
{
	PSZ   str = NULL;
  bool  b = false, err = true;
  JOUT *jp;
	FILE *fs = NULL;

	g->Message[0] = 0;

	// Save stack and allocation environment and prepare error return
	if (g->jump_level == MAX_JUMP) {
		strcpy(g->Message, MSG(TOO_MANY_JUMPS));
		return NULL;
	} // endif jump_level

	if (setjmp(g->jumper[++g->jump_level])) {
		str = NULL;
		goto fin;
	} // endif jmp

	if (!jsp) {
    strcpy(g->Message, "Null json tree");
    goto fin;
  } else if (!fn) {
    // Serialize to a string
    jp = new(g) JOUTSTR(g);
    b = pretty == 1;
	} else {
		if (!(fs = fopen(fn, "wb"))) {
			sprintf(g->Message, MSG(OPEN_MODE_ERROR),
				"w", (int)errno, fn);
			strcat(strcat(g->Message, ": "), strerror(errno));
			goto fin;;
		} else if (pretty >= 2) {
			// Serialize to a pretty file
			jp = new(g)JOUTPRT(g, fs);
		} else {
			// Serialize to a flat file
			b = true;
			jp = new(g)JOUTFILE(g, fs, pretty);
		} // endif's

	}	// endif's

  switch (jsp->GetType()) {
    case TYPE_JAR:
      err = SerializeArray(jp, (PJAR)jsp, b);
      break;
    case TYPE_JOB:
      err = ((b && jp->Prty()) && jp->WriteChr('\t'));
      err |= SerializeObject(jp, (PJOB)jsp);
      break;
    case TYPE_JVAL:
      err = SerializeValue(jp, (PJVAL)jsp);
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

fin:
	g->jump_level--;
	return str;
} // end of Serialize

/***********************************************************************/
/* Serialize a JSON Array.                                             */
/***********************************************************************/
bool SerializeArray(JOUT *js, PJAR jarp, bool b)
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

    if (SerializeValue(js, jarp->GetValue(i)))
      return true;

    } // endfor i

	if (b && js->Prty() == 1 && js->WriteStr(EL))
    return true;

	return ((!b || js->Prty()) && js->WriteChr(']'));
} // end of SerializeArray

/***********************************************************************/
/* Serialize a JSON Object.                                            */
/***********************************************************************/
bool SerializeObject(JOUT *js, PJOB jobp)
{
  bool first = true;

  if (js->WriteChr('{'))
    return true;

  for (PJPR pair = jobp->First; pair; pair = pair->Next) {
    if (first)
      first = false;
    else if (js->WriteChr(','))
      return true;

    if (js->WriteChr('"') ||
        js->WriteStr(pair->Key) ||
        js->WriteChr('"') ||
        js->WriteChr(':') ||
        SerializeValue(js, pair->Val))
      return true;

    } // endfor i

  return js->WriteChr('}');
} // end of SerializeObject

/***********************************************************************/
/* Serialize a JSON Value.                                             */
/***********************************************************************/
bool SerializeValue(JOUT *js, PJVAL jvp)
{
  PJAR jap;
  PJOB jop;
  PVAL valp;

  if ((jap = jvp->GetArray()))
    return SerializeArray(js, jap, false);
  else if ((jop = jvp->GetObject()))
    return SerializeObject(js, jop);
  else if (!(valp = jvp->Value) || valp->IsNull())
    return js->WriteStr("null");
  else switch (valp->GetType()) {
    case TYPE_TINY:
      return js->WriteStr(valp->GetTinyValue() ? "true" : "false");
    case TYPE_STRING:
      return js->Escape(valp->GetCharValue());
    default:
      if (valp->IsTypeNum()) {
        char buf[32];

        return js->WriteStr(valp->GetCharString(buf));
        } // endif valp

    } // endswitch Type

	strcpy(js->g->Message, "Unrecognized value");
	return true;
} // end of SerializeValue

/* -------------------------- Class JOUTSTR -------------------------- */

/***********************************************************************/
/* JOUTSTR constructor.                                                */
/***********************************************************************/
JOUTSTR::JOUTSTR(PGLOBAL g) : JOUT(g)
{
  PPOOLHEADER pph = (PPOOLHEADER)g->Sarea;

  N = 0;
  Max = pph->FreeBlk;
  Max = (Max > 32) ? Max - 32 : Max;
  Strp = (char*)PlugSubAlloc(g, NULL, 0);  // Size not know yet
} // end of JOUTSTR constructor

/***********************************************************************/
/* Concatenate a string to the Serialize string.                       */
/***********************************************************************/
bool JOUTSTR::WriteStr(const char *s)
{
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
bool JOUTSTR::WriteChr(const char c)
{
  if (N + 1 > Max)
    return true;

  Strp[N++] = c;
  return false;
} // end of WriteChr

/***********************************************************************/
/* Escape and Concatenate a string to the Serialize string.            */
/***********************************************************************/
bool JOUTSTR::Escape(const char *s)
{
  WriteChr('"');

  for (unsigned int i = 0; i < strlen(s); i++)
    switch (s[i]) {
      case '"':  
      case '\\':
      case '\t':
      case '\n':
      case '\r':
      case '\b':
      case '\f': WriteChr('\\');
        // passthru
      default:
        WriteChr(s[i]);
        break;
      } // endswitch s[i]

  WriteChr('"');
  return false;
} // end of Escape

/* ------------------------- Class JOUTFILE -------------------------- */

/***********************************************************************/
/* Write a string to the Serialize file.                               */
/***********************************************************************/
bool JOUTFILE::WriteStr(const char *s)
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
bool JOUTFILE::Escape(const char *s)
{
  // This is temporary
  fputc('"', Stream);

  for (unsigned int i = 0; i < strlen(s); i++)
    switch (s[i]) {
      case '"':  fputs("\\\"", Stream); break;
      case '\\': fputs("\\\\", Stream); break;
      case '\t': fputs("\\t",  Stream); break;
      case '\n': fputs("\\n",  Stream); break;
      case '\r': fputs("\\r",  Stream); break;
      case '\b': fputs("\\b",  Stream); break;
      case '\f': fputs("\\f",  Stream); break;
      default:
        fputc(s[i], Stream);
        break;
      } // endswitch s[i]

  fputc('"', Stream);
  return false;
} // end of Escape

/* ------------------------- Class JOUTPRT --------------------------- */

/***********************************************************************/
/* Write a string to the Serialize pretty file.                        */
/***********************************************************************/
bool JOUTPRT::WriteStr(const char *s)
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

/* -------------------------- Class JOBJECT -------------------------- */

/***********************************************************************/
/* Add a new pair to an Object.                                        */
/***********************************************************************/
PJPR JOBJECT::AddPair(PGLOBAL g, PSZ key)
{
  PJPR jpp = new(g) JPAIR(key);

  if (Last)
    Last->Next = jpp;
  else
    First = jpp;

  Last = jpp;
  Size++;
  return jpp;
} // end of AddPair

/***********************************************************************/
/* Return all keys as an array.                                        */
/***********************************************************************/
PJAR JOBJECT::GetKeyList(PGLOBAL g)
{
	PJAR jarp = new(g) JARRAY();

	for (PJPR jpp = First; jpp; jpp = jpp->Next)
		jarp->AddValue(g, new(g) JVALUE(g, jpp->GetKey()));

	jarp->InitArray(g);
	return jarp;
} // end of GetKeyList

/***********************************************************************/
/* Get the value corresponding to the given key.                       */
/***********************************************************************/
PJVAL JOBJECT::GetValue(const char* key)
{
  for (PJPR jp = First; jp; jp = jp->Next)
    if (!strcmp(jp->Key, key))
      return jp->Val;

  return NULL;
} // end of GetValue;

/***********************************************************************/
/* Return the text corresponding to all keys (XML like).               */
/***********************************************************************/
PSZ JOBJECT::GetText(PGLOBAL g, PSZ text)
{
  int n;

  if (!text) {
    text = (char*)PlugSubAlloc(g, NULL, 0);
    text[0] = 0;
    n = 1;
  } else
    n = 0;

  if (!First && n)
    return NULL;
  else for (PJPR jp = First; jp; jp = jp->Next)
    jp->Val->GetText(g, text);

  if (n)
    PlugSubAlloc(g, NULL, strlen(text) + 1);

  return text + n;
} // end of GetValue;

/***********************************************************************/
/* Merge two objects.                                                  */
/***********************************************************************/
bool JOBJECT::Merge(PGLOBAL g, PJSON jsp)
{
	if (jsp->GetType() != TYPE_JOB) {
		strcpy(g->Message, "Second argument is not an object");
		return true;
	}	// endif Type

	PJOB jobp = (PJOB)jsp;

	for (PJPR jpp = jobp->First; jpp; jpp = jpp->Next)
		SetValue(g, jpp->GetVal(), jpp->GetKey());

	return false;
} // end of Marge;

/***********************************************************************/
/* Set or add a value corresponding to the given key.                  */
/***********************************************************************/
void JOBJECT::SetValue(PGLOBAL g, PJVAL jvp, PSZ key)
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
/* Delete a value corresponding to the given key.                  */
/***********************************************************************/
void JOBJECT::DeleteKey(PSZ key)
{
	PJPR jp, *pjp = &First;

	for (jp = First; jp; jp = jp->Next)
		if (!strcmp(jp->Key, key)) {
			*pjp = jp->Next;
			Size--;
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
PJVAL JARRAY::GetValue(int i)
{
  if (Mvals && i >= 0 && i < Size)
    return Mvals[i];
  else
    return NULL;
} // end of GetValue

/***********************************************************************/
/* Add a Value to the Array Value list.                                */
/***********************************************************************/
PJVAL JARRAY::AddValue(PGLOBAL g, PJVAL jvp, int *x)
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
	}	// endif Type

	PJAR arp = (PJAR)jsp;

	for (int i = 0; i < jsp->size(); i++)
		AddValue(g, arp->GetValue(i));

	InitArray(g);
	return false;
} // end of Merge

/***********************************************************************/
/* Set the nth Value of the Array Value list.                          */
/***********************************************************************/
bool JARRAY::SetValue(PGLOBAL g, PJVAL jvp, int n)
{
  int   i = 0;
  PJVAL jp, *jpp = &First;

  for (jp = First; i < n; i++, jp = *(jpp = &jp->Next))
    if (!jp)
      *jpp = jp = new(g) JVALUE;

  *jpp = jvp;
  jvp->Next = (jp ? jp->Next : NULL);
  return false;
} // end of SetValue

/***********************************************************************/
/* Delete a Value from the Arrays Value list.                          */
/***********************************************************************/
bool JARRAY::DeleteValue(int n)
{
  PJVAL jvp = GetValue(n);

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
/* Constructor for a Value with a given string or numeric value.       */
/***********************************************************************/
JVALUE::JVALUE(PGLOBAL g, PVAL valp) : JSON()
{
  Jsp = NULL; 
  Value = AllocateValue(g, valp); 
  Next = NULL;
  Del = false;
} // end of JVALUE constructor

/***********************************************************************/
/* Constructor for a given string.                                     */
/***********************************************************************/
JVALUE::JVALUE(PGLOBAL g, PSZ strp) : JSON()
{
	Jsp = NULL;
	Value = AllocateValue(g, strp, TYPE_STRING);
	Next = NULL;
	Del = false;
} // end of JVALUE constructor

/***********************************************************************/
/* Returns the type of the Value's value.                              */
/***********************************************************************/
JTYP JVALUE::GetValType(void)
{
  if (Jsp)
    return Jsp->GetType();
  else if (Value)
    return (JTYP)Value->GetType();
  else
    return (JTYP)TYPE_VOID;

} // end of GetValType

/***********************************************************************/
/* Return the Value's Object value.                                    */
/***********************************************************************/
PJOB JVALUE::GetObject(void)
{
  if (Jsp && Jsp->GetType() == TYPE_JOB)
    return (PJOB)Jsp;

  return NULL;
} // end of GetObject

/***********************************************************************/
/* Return the Value's Array value.                                     */
/***********************************************************************/
PJAR JVALUE::GetArray(void)
{
  if (Jsp && Jsp->GetType() == TYPE_JAR)
    return (PJAR)Jsp;

  return NULL;
} // end of GetArray

/***********************************************************************/
/* Return the Value's Integer value.                                   */
/***********************************************************************/
int JVALUE::GetInteger(void)
{
  return (Value) ? Value->GetIntValue() : 0;
} // end of GetInteger

/***********************************************************************/
/* Return the Value's Big integer value.                               */
/***********************************************************************/
long long JVALUE::GetBigint(void)
{
	return (Value) ? Value->GetBigintValue() : 0;
} // end of GetBigint

/***********************************************************************/
/* Return the Value's Double value.                                    */
/***********************************************************************/
double JVALUE::GetFloat(void)
{
  return (Value) ? Value->GetFloatValue() : 0.0;
} // end of GetFloat

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ JVALUE::GetString(void)
{
  char buf[32];
  return (Value) ? Value->GetCharString(buf) : NULL;
} // end of GetString

/***********************************************************************/
/* Return the Value's String value.                                    */
/***********************************************************************/
PSZ JVALUE::GetText(PGLOBAL g, PSZ text)
{
  if (Jsp && Jsp->GetType() == TYPE_JOB)
    return Jsp->GetText(g, text);

  char buf[32];
  PSZ  s = (Value) ? Value->GetCharString(buf) : NULL;

  if (s)
    strcat(strcat(text, " "), s);
  else
    strcat(text, " ???");

  return text;
} // end of GetText

void JVALUE::SetValue(PJSON jsp)
{ 
	if (jsp && jsp->GetType() == TYPE_JVAL) {
		Jsp = jsp->GetJsp();
		Value = jsp->GetValue();
	} else {
		Jsp = jsp;
		Value = NULL;
	} // endif Type

}	// end of SetValue;

/***********************************************************************/
/* Set the Value's value as the given integer.                         */
/***********************************************************************/
void JVALUE::SetInteger(PGLOBAL g, int n)
{
  Value = AllocateValue(g, &n, TYPE_INT);
	Jsp = NULL;
} // end of SetInteger

/***********************************************************************/
/* Set the Value's Boolean value as a tiny integer.                    */
/***********************************************************************/
void JVALUE::SetTiny(PGLOBAL g, char n)
{
	Value = AllocateValue(g, &n, TYPE_TINY);
	Jsp = NULL;
} // end of SetInteger

/***********************************************************************/
/* Set the Value's value as the given big integer.                     */
/***********************************************************************/
void JVALUE::SetBigint(PGLOBAL g, long long ll)
{
	Value = AllocateValue(g, &ll, TYPE_BIGINT);
	Jsp = NULL;
} // end of SetBigint

/***********************************************************************/
/* Set the Value's value as the given DOUBLE.                          */
/***********************************************************************/
void JVALUE::SetFloat(PGLOBAL g, double f)
{
  Value = AllocateValue(g, &f, TYPE_DOUBLE, 6);
	Jsp = NULL;
} // end of SetFloat

/***********************************************************************/
/* Set the Value's value as the given string.                          */
/***********************************************************************/
void JVALUE::SetString(PGLOBAL g, PSZ s, short c)
{
  Value = AllocateValue(g, s, TYPE_STRING, c);
	Jsp = NULL;
} // end of SetString

/***********************************************************************/
/* True when its JSON or normal value is null.                         */
/***********************************************************************/
bool JVALUE::IsNull(void)
{
  return (Jsp) ? Jsp->IsNull() : (Value) ? Value->IsZero() : true;
} // end of IsNull

