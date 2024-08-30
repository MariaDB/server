/*
 * Profile functions
 *
 * Copyright 1993 Miguel de Icaza
 * Copyright 1996 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */
#include "my_global.h"

#include <ctype.h>
//#include <errno.h>
#include <fcntl.h>
//#include <io.h>  commented this line out to compile for solaris
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
//#include <sys/types.h>
//#include <memory.h>
#include "osutil.h"
#include "global.h"
#include "inihandl.h"

// The types and variables used locally
//typedef int bool;
typedef unsigned int uint;
//#define SVP(S)  ((S) ? S : "<null>")
#define _strlwr(P)  strlwr(P)  //OB: changed this line
#define MAX_PATHNAME_LEN  256
#define N_CACHED_PROFILES  10
#ifndef WIN32
#define stricmp    strcasecmp
#define _strnicmp  strncasecmp
#endif // !WIN32
#define EnterCriticalSection(x)
#define LeaveCriticalSection(x)

#if defined(TEST_MODULE)
// Stand alone test program
#include <stdarg.h>
        int trace = 0;
void    htrc(char const *fmt, ...)
{
  va_list ap;
  va_start (ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end (ap);
} /* end of htrc */
#else   // !TEST_MODULE
// Normal included functions
//extern  int trace;
//void    htrc(char const *fmt, ...);
#endif  // !TEST MODULE


typedef struct tagPROFILEKEY {
  char                 *value;
  struct tagPROFILEKEY *next;
  char                  name[1];
  } PROFILEKEY;

typedef struct tagPROFILESECTION {
  struct tagPROFILEKEY     *key;
  struct tagPROFILESECTION *next;
  char                      name[1];
  } PROFILESECTION;

typedef struct {
  BOOL             changed;
  PROFILESECTION  *section;
//char            *dos_name;
//char            *unix_name;
  char            *filename;
  time_t           mtime;
  } PROFILE;

#define memfree(P)   if (P) free(P)

/* Cached profile files */
static PROFILE *MRUProfile[N_CACHED_PROFILES] = {NULL};

#define CurProfile (MRUProfile[0])

/* wine.ini config file registry root */
//static HKEY wine_profile_key;

#define PROFILE_MAX_LINE_LEN   1024

/* Wine profile name in $HOME directory; must begin with slash */
//static const char PROFILE_WineIniName[] = "/.winerc";

/* Wine profile: the profile file being used */
//static char PROFILE_WineIniUsed[MAX_PATHNAME_LEN] = "";

/* Check for comments in profile */
#define IS_ENTRY_COMMENT(str)  ((str)[0] == ';')

//static const WCHAR wininiW[] = { 'w','i','n','.','i','n','i',0 };

//static CRITICAL_SECTION PROFILE_CritSect = CRITICAL_SECTION_INIT("PROFILE_CritSect");

BOOL  WritePrivateProfileString(LPCSTR section, LPCSTR entry,
                                LPCSTR string, LPCSTR filename);

/***********************************************************************
 *           PROFILE_CopyEntry
 *
 * Copy the content of an entry into a buffer, removing quotes,
 * and possibly translating environment variables.
 ***********************************************************************/
static void PROFILE_CopyEntry( char *buffer, const char *value, uint len,
                               int handle_env )
{
  const char *p;
  char quote = '\0';

  if (!buffer)
    return;

  if ((*value == '\'') || (*value == '\"'))
    if (value[1] && (value[strlen(value)-1] == *value))
      quote = *value++;

  if (!handle_env) {
    strncpy(buffer, value, len);

    if (quote && (len >= strlen(value)))
      buffer[strlen(buffer)-1] = '\0';

    return;
    } // endif handle

  for (p = value; (*p && (len > 1)); *buffer++ = *p++, len--) {
    if ((*p == '$') && (p[1] == '{')) {
      char        env_val[1024];
      const char *env_p;
      const char *p2 = strchr(p, '}');

      if (!p2)
        continue;  /* ignore it */

      strncpy(env_val, p + 2, MY_MIN((int) sizeof(env_val), (int)(p2-p)-1));

      if ((env_p = getenv(env_val)) != NULL) {
        int buffer_len;

        strncpy( buffer, env_p, len );
        buffer_len = strlen( buffer );
        buffer += buffer_len;
        len -= buffer_len;
        } // endif env_p

      p = p2 + 1;
      } // endif p

    } // endfor p

  if (quote && (len > 1))
    buffer--;

  *buffer = '\0';
}  // end of PROFILE_CopyEntry


/***********************************************************************
 *           PROFILE_Save
 *
 * Save a profile tree to a file.
 ***********************************************************************/
static void PROFILE_Save( FILE *file, PROFILESECTION *section )
{
  PROFILEKEY *key;
  int secno;

  for (secno= 0; section; section= section->next) {
    if (section->name[0]) {
      fprintf(file, "%s[%s]\n", secno ? "\n" : "", SVP(section->name));
      secno++;
    }

    for (key = section->key; key; key = key->next) {
      if (key->name[0]) {
        fprintf(file, "%s", SVP(key->name));

        if (key->value)
          fprintf(file, "=%s", SVP(key->value));

        fprintf(file, "\n");
      } // endif key->name
    }
  }  // endfor section

} // end of PROFILE_Save


/***********************************************************************
 *           PROFILE_Free
 *
 * Free a profile tree.
 ***********************************************************************/
static void PROFILE_Free( PROFILESECTION *section )
{
  PROFILESECTION *next_section;
  PROFILEKEY *key, *next_key;

  for (; section; section = next_section) {
    for (key = section->key; key; key = next_key) {
      next_key = key->next;
      memfree(key->value);
      free(key);
      }  // endfor key

    next_section = section->next;
    free(section);
    } // endfor section

} // end of PROFILE_Free

static int PROFILE_isspace(char c)
{
  /* CR and ^Z (DOS EOF) are spaces too  (found on CD-ROMs) */
  if (isspace(c) || c=='\r' || c==0x1a)
    return 1;

  return 0;
} // end of PROFILE_isspace


/***********************************************************************
 *           PROFILE_Load
 *
 * Load a profile tree from a file.
 ***********************************************************************/
static PROFILESECTION *PROFILE_Load( FILE *file )
{
  char  buffer[PROFILE_MAX_LINE_LEN];
  char *p, *p2;
  int   line = 0;
  PROFILESECTION  *section, *first_section;
  PROFILESECTION* *next_section;
  PROFILEKEY      *key, *prev_key, **next_key;

  first_section = (PROFILESECTION*)malloc(sizeof(*section));

  if (first_section == NULL)
    return NULL;

  first_section->name[0] = 0;
  first_section->key  = NULL;
  first_section->next = NULL;
  next_section = &first_section->next;
  next_key     = &first_section->key;
  prev_key     = NULL;

  while (fgets(buffer, PROFILE_MAX_LINE_LEN, file)) {
    line++;
    p = buffer;

    while (*p && PROFILE_isspace(*p))
      p++;

    if (*p == '[') {  /* section start */
      if (!(p2 = strrchr( p, ']'))) {
        fprintf(stderr, "Invalid section header at line %d: '%s'\n",
                line, p);
      }  else {
        *p2 = '\0';
        p++;

        if (!(section = (PROFILESECTION*)malloc(sizeof(*section) + strlen(p))))
          break;

        strcpy(section->name, p);
        section->key  = NULL;
        section->next = NULL;
        *next_section = section;
        next_section  = &section->next;
        next_key      = &section->key;
        prev_key      = NULL;

        if (trace(2))
          htrc("New section: '%s'\n",section->name);

        continue;
      }  // endif p2

      }  // endif p

    p2 = p + strlen(p) - 1;

    while ((p2 > p) && ((*p2 == '\n') || PROFILE_isspace(*p2)))
      *p2-- = '\0';

    if ((p2 = strchr(p, '=')) != NULL) {
      char *p3 = p2 - 1;

      while ((p3 > p) && PROFILE_isspace(*p3))
        *p3-- = '\0';

      *p2++ = '\0';

      while (*p2 && PROFILE_isspace(*p2))
        p2++;

      } // endif p2

    if (*p || !prev_key || *prev_key->name) {
      if (!(key = (PROFILEKEY*)malloc(sizeof(*key) + strlen(p))))
        break;

      strcpy(key->name, p);

      if (p2) {
        key->value = (char*)malloc(strlen(p2)+1);
        strcpy(key->value, p2);
      } else
        key->value = NULL;

      key->next = NULL;
      *next_key = key;
      next_key  = &key->next;
      prev_key  = key;

      if (trace(2))
        htrc("New key: name='%s', value='%s'\n",
              key->name,key->value?key->value:"(none)");

      } // endif p || prev_key

    } // endif *p

  return first_section;
} // end of PROFILE_Load

/***********************************************************************
 *           PROFILE_FlushFile
 *
 * Flush the current profile to disk if changed.
 ***********************************************************************/
static BOOL PROFILE_FlushFile(void)
{
//char       *p, buffer[MAX_PATHNAME_LEN];
//const char *unix_name;
  FILE       *file = NULL;
  struct stat buf;
  
  if (trace(2))
    htrc("PROFILE_FlushFile: CurProfile=%p\n", CurProfile);

  if (!CurProfile) {
    fprintf(stderr, "No current profile!\n");
    return FALSE;
    } // endif !CurProfile

  if (!CurProfile->changed || !CurProfile->filename)
    return TRUE;

#if 0
  if (!(file = fopen(unix_name, "w"))) {
    /* Try to create it in $HOME/.wine */
    /* FIXME: this will need a more general solution */
    //strcpy( buffer, get_config_dir() );
    //p = buffer + strlen(buffer);
    //*p++ = '/';
    char *p1 = strrchr(CurProfile->filename, '\\');

    p = buffer;              // OB: To be elaborate

    if (p1) 
      p1++;
    else
      p1 = CurProfile->dos_name;

    strcpy(p, p1);
    _strlwr(p);
    file = fopen(buffer, "w");
    unix_name = buffer;
    }  // endif !unix_name
#endif // 0

  if (!(file = fopen(CurProfile->filename, "w"))) {
    fprintf(stderr, "could not save profile file %s\n", CurProfile->filename);
    return FALSE;
    } // endif !file

  if (trace(2))
    htrc("Saving '%s'\n", CurProfile->filename);

  PROFILE_Save(file, CurProfile->section);
  fclose(file);
  CurProfile->changed = FALSE;

  if (!stat(CurProfile->filename, &buf))
    CurProfile->mtime = buf.st_mtime;

  return TRUE;
}  // end of PROFILE_FlushFile


/***********************************************************************
 *           PROFILE_ReleaseFile
 *
 * Flush the current profile to disk and remove it from the cache.
 ***********************************************************************/
static void PROFILE_ReleaseFile(void)
{
  PROFILE_FlushFile();
  PROFILE_Free(CurProfile->section);
//memfree(CurProfile->dos_name);
//memfree(CurProfile->unix_name);
  memfree(CurProfile->filename);
  CurProfile->changed   = FALSE;
  CurProfile->section   = NULL;
//CurProfile->dos_name  = NULL;
//CurProfile->unix_name = NULL;
  CurProfile->filename  = NULL;
  CurProfile->mtime     = 0;
}  // end of PROFILE_ReleaseFile


/***********************************************************************
 *           PROFILE_Open
 *
 * Open a profile file, checking the cached file first.
 ***********************************************************************/
static BOOL PROFILE_Open(LPCSTR filename)
{
//char        buffer[MAX_PATHNAME_LEN];
//char       *p;
  FILE       *file = NULL;
  int         i, j;
  struct stat buf;
  PROFILE    *tempProfile;
  
  if (trace(2))
    htrc("PROFILE_Open: CurProfile=%p N=%d\n", CurProfile, N_CACHED_PROFILES);

  /* First time around */
  if (!CurProfile)
    for (i = 0; i < N_CACHED_PROFILES; i++) {
      MRUProfile[i] = (PROFILE*)malloc(sizeof(PROFILE));
      
      if (MRUProfile[i] == NULL)
        break;
        
      MRUProfile[i]->changed=FALSE;
      MRUProfile[i]->section=NULL;
//    MRUProfile[i]->dos_name=NULL;
//    MRUProfile[i]->unix_name=NULL;
      MRUProfile[i]->filename=NULL;
      MRUProfile[i]->mtime=0;
      } // endfor i

  /* Check for a match */
  for (i = 0; i < N_CACHED_PROFILES; i++) {
    if (trace(2))
      htrc("MRU=%s i=%d\n", SVP(MRUProfile[i]->filename), i);
      
    if (MRUProfile[i]->filename && !strcmp(filename, MRUProfile[i]->filename)) {
      if (i) {
        PROFILE_FlushFile();
        tempProfile = MRUProfile[i];
          
        for (j = i; j > 0; j--)
          MRUProfile[j] = MRUProfile[j-1];
          
        CurProfile=tempProfile;
        } // endif i
        
      if (!stat(CurProfile->filename, &buf) && CurProfile->mtime == buf.st_mtime) {
        if (trace(2))
          htrc("(%s): already opened (mru=%d)\n", filename, i);
          
      } else {
        if (trace(2))
          htrc("(%s): already opened, needs refreshing (mru=%d)\n",  filename, i);

      } // endif stat
      
      return TRUE;
      } // endif filename
      
    } // endfor i

  /* Flush the old current profile */
  PROFILE_FlushFile();

  /* Make the oldest profile the current one only in order to get rid of it */
  if (i == N_CACHED_PROFILES) {
    tempProfile = MRUProfile[N_CACHED_PROFILES-1];
    
    for(i = N_CACHED_PROFILES-1; i > 0; i--)
      MRUProfile[i] = MRUProfile[i-1];
        
    CurProfile = tempProfile;
    } // endif i
    
  if (CurProfile->filename)
    PROFILE_ReleaseFile();

  /* OK, now that CurProfile is definitely free we assign it our new file */
//  newdos_name = HeapAlloc( GetProcessHeap(), 0, strlen(full_name.short_name)+1 );
//  strcpy( newdos_name, full_name.short_name );

//  newdos_name = malloc(strlen(filename)+1);
//  strcpy(newdos_name, filename);

//  CurProfile->dos_name = newdos_name;
  CurProfile->filename = (char*)malloc(strlen(filename) + 1);
  strcpy(CurProfile->filename, filename);

  /* Try to open the profile file, first in $HOME/.wine */

  /* FIXME: this will need a more general solution */
//  strcpy( buffer, get_config_dir() );
//  p = buffer + strlen(buffer);
//  *p++ = '/';
//  strcpy( p, strrchr( newdos_name, '\\' ) + 1 );
//  p = buffer;
//  strcpy(p, filename);
//  _strlwr(p);
  
  if (trace(2))
    htrc("Opening %s\n", filename);
    
  if ((file = fopen(filename, "r"))) {
    if (trace(2))
      htrc("(%s): found it\n", filename);

//    CurProfile->unix_name = malloc(strlen(buffer)+1);
//    strcpy(CurProfile->unix_name, buffer);
    } /* endif file */

  if (file) {
    CurProfile->section = PROFILE_Load(file);
    fclose(file);
    
    if (!stat(CurProfile->filename, &buf))
      CurProfile->mtime = buf.st_mtime;
      
  } else {
    /* Does not exist yet, we will create it in PROFILE_FlushFile */
    fprintf(stderr, "profile file %s not found\n", filename);
  } /* endif file */

  return TRUE;
}


/***********************************************************************
 *           PROFILE_Close
 *
 * Flush the named profile to disk and remove it from the cache.
 ***********************************************************************/
void PROFILE_Close(LPCSTR filename)
{
  int         i;
  BOOL        close = FALSE;
  struct stat buf;
  PROFILE    *tempProfile;
  
  if (trace(2))
    htrc("PROFILE_Close: CurProfile=%p N=%d\n", CurProfile, N_CACHED_PROFILES);

  /* Check for a match */
  for (i = 0; i < N_CACHED_PROFILES; i++) {
    if (trace(2))
      htrc("MRU=%s i=%d\n", SVP(MRUProfile[i]->filename), i);
      
    if (MRUProfile[i]->filename && !strcmp(filename, MRUProfile[i]->filename)) {
      if (i) {
        /* Make the profile to close current */
        tempProfile = MRUProfile[i];
        MRUProfile[i] = MRUProfile[0];
        MRUProfile[0] = tempProfile;
        CurProfile=tempProfile;
        } // endif i
      
      if (trace(2)) {
        if (!stat(CurProfile->filename, &buf) && CurProfile->mtime == buf.st_mtime)
          htrc("(%s): already opened (mru=%d)\n", filename, i);
        else
          htrc("(%s): already opened, needs refreshing (mru=%d)\n",  filename, i);

        } // endif trace
      
      close = TRUE;
      break;
      } // endif filename
      
    } // endfor i

  if (close)
    PROFILE_ReleaseFile();

}  // end of PROFILE_Close


/***********************************************************************
 *           PROFILE_End
 *
 * Terminate and release the cache.
 ***********************************************************************/
void PROFILE_End(void)
{
  int i;

  if (trace(3))
    htrc("PROFILE_End: CurProfile=%p N=%d\n", CurProfile, N_CACHED_PROFILES);

	if (!CurProfile)						   //	Sergey Vojtovich
		return;

  /* Close all opened files and free the cache structure */
  for (i = 0; i < N_CACHED_PROFILES; i++) {
    if (trace(3))
      htrc("MRU=%s i=%d\n", SVP(MRUProfile[i]->filename), i);

//  CurProfile = MRUProfile[i];			Sergey Vojtovich
//  PROFILE_ReleaseFile();					see MDEV-9997
    free(MRUProfile[i]);
    } // endfor i

}  // end of PROFILE_End


/***********************************************************************
 *           PROFILE_DeleteSection
 *
 * Delete a section from a profile tree.
 ***********************************************************************/
static BOOL PROFILE_DeleteSection(PROFILESECTION* *section, LPCSTR name)
{
  while (*section) {
    if ((*section)->name[0] && !stricmp((*section)->name, name)) {
      PROFILESECTION *to_del = *section;

      *section = to_del->next;
      to_del->next = NULL;
      PROFILE_Free(to_del);
      return TRUE;
      } // endif section

    section = &(*section)->next;
    } // endwhile section

  return FALSE;
}  // end of PROFILE_DeleteSection


/***********************************************************************
 *           PROFILE_DeleteKey
 *
 * Delete a key from a profile tree.
 ***********************************************************************/
static BOOL PROFILE_DeleteKey(PROFILESECTION* *section,
                              LPCSTR section_name, LPCSTR key_name)
{
  while (*section) {
    if ((*section)->name[0] && !stricmp((*section)->name, section_name)) {
      PROFILEKEY* *key = &(*section)->key;

      while (*key) {
        if (!stricmp((*key)->name, key_name))  {
          PROFILEKEY *to_del = *key;

          *key = to_del->next;
          memfree(to_del->value);
          free(to_del);
          return TRUE;
          } // endif name

        key = &(*key)->next;
        } // endwhile *key

      } // endif section->name

    section = &(*section)->next;
    } // endwhile *section

  return FALSE;
}  // end of PROFILE_DeleteKey


/***********************************************************************
 *           PROFILE_DeleteAllKeys
 *
 * Delete all keys from a profile tree.
 ***********************************************************************/
static void PROFILE_DeleteAllKeys(LPCSTR section_name)
{
  PROFILESECTION* *section= &CurProfile->section;

  while (*section) {
    if ((*section)->name[0] && !stricmp((*section)->name, section_name)) {
      PROFILEKEY* *key = &(*section)->key;

      while (*key) {
        PROFILEKEY *to_del = *key;

        *key = to_del->next;
        memfree(to_del->value);
        free(to_del);
        CurProfile->changed = TRUE;
        } // endwhile *key

      } // endif section->name

    section = &(*section)->next;
    }  // endwhile *section

} // end of PROFILE_DeleteAllKeys


/***********************************************************************
 *           PROFILE_Find
 *
 * Find a key in a profile tree, optionally creating it.
 ***********************************************************************/
static PROFILEKEY *PROFILE_Find(PROFILESECTION* *section, 
                                const char *section_name,
                                const char *key_name, 
                                BOOL create, BOOL create_always)
{
  const char *p;
  int seclen, keylen;

  while (PROFILE_isspace(*section_name))
    section_name++;

  p = section_name + strlen(section_name) - 1;

  while ((p > section_name) && PROFILE_isspace(*p))
    p--;

  seclen = p - section_name + 1;

  while (PROFILE_isspace(*key_name))
    key_name++;

  p = key_name + strlen(key_name) - 1;

  while ((p > key_name) && PROFILE_isspace(*p))
    p--;

  keylen = p - key_name + 1;

  while (*section) {
    if (((*section)->name[0])
         && (!(_strnicmp((*section)->name, section_name, seclen )))
         && (((*section)->name)[seclen] == '\0')) {
      PROFILEKEY* *key = &(*section)->key;

      while (*key) {
        /* If create_always is FALSE then we check if the keyname already exists.
         * Otherwise we add it regardless of its existence, to allow
         * keys to be added more then once in some cases.
         */
        if (!create_always) {
          if ((!(_strnicmp( (*key)->name, key_name, keylen )))
               && (((*key)->name)[keylen] == '\0'))
            return *key;

          }  // endif !create_always

        key = &(*key)->next;
        } // endwhile *key

      if (!create)
        return NULL;

      if (!(*key = (PROFILEKEY*)malloc(sizeof(PROFILEKEY) + strlen(key_name))))
        return NULL;

      strcpy((*key)->name, key_name);
      (*key)->value = NULL;
      (*key)->next  = NULL;
      return *key;
      } // endifsection->name

    section = &(*section)->next;
    } // endwhile *section

  if (!create)
    return NULL;

  *section = (PROFILESECTION*)malloc(sizeof(PROFILESECTION) + strlen(section_name));

  if (*section == NULL)
    return NULL;

  strcpy((*section)->name, section_name);
  (*section)->next = NULL;

  if (!((*section)->key = (tagPROFILEKEY*)malloc(sizeof(PROFILEKEY) + strlen(key_name)))) {
    free(*section);
    return NULL;
    }  // endif malloc

  strcpy((*section)->key->name, key_name);
  (*section)->key->value = NULL;
  (*section)->key->next  = NULL;
  return (*section)->key;
}  // end of PROFILE_Find


/***********************************************************************
 *           PROFILE_GetSection
 *
 * Returns all keys of a section.
 * If return_values is TRUE, also include the corresponding values.
 ***********************************************************************/
static int PROFILE_GetSection(PROFILESECTION *section, LPCSTR section_name,
                              LPSTR buffer, uint len,
                              BOOL handle_env, BOOL return_values)
{
  PROFILEKEY *key;

  if(!buffer) 
    return 0;

  while (section) {
    if (section->name[0] && !stricmp(section->name, section_name)) {
      uint oldlen = len;

      for (key = section->key; key; key = key->next) {
        if (len <= 2)
          break;

        if (!*key->name)
          continue;  /* Skip empty lines */

        if (IS_ENTRY_COMMENT(key->name))
          continue;  /* Skip comments */

        PROFILE_CopyEntry(buffer, key->name, len - 1, handle_env);
        len -= strlen(buffer) + 1;
        buffer += strlen(buffer) + 1;

        if (len < 2)
          break;

        if (return_values && key->value) {
          buffer[-1] = '=';
          PROFILE_CopyEntry(buffer, key->value, len - 1, handle_env);
          len -= strlen(buffer) + 1;
          buffer += strlen(buffer) + 1;
          } // endif return_values

        } // endfor key

      *buffer = '\0';

      if (len <= 1) {
        /*If either lpszSection or lpszKey is NULL and the supplied
          destination buffer is too small to hold all the strings,
          the last string is truncated and followed by two null characters.
          In this case, the return value is equal to cchReturnBuffer
          minus two. */
        buffer[-1] = '\0';
        return oldlen - 2;
        } // endif len

      return oldlen - len;
      } // endif section->name

    section = section->next;
    } // endwhile section

  buffer[0] = buffer[1] = '\0';
  return 0;
}  // end of PROFILE_GetSection


/* See GetPrivateProfileSectionNamesA for documentation */
static int PROFILE_GetSectionNames(LPSTR buffer, uint len)
{
  LPSTR           buf;
  uint            f,l;
  PROFILESECTION *section;

  if (trace(2))
    htrc("GetSectionNames: buffer=%p len=%u\n", buffer, len);

  if (!buffer || !len)
    return 0;

  if (len == 1) {
    *buffer='\0';
    return 0;
    } // endif len

  f = len - 1;
  buf = buffer;
  section = CurProfile->section;

  if (trace(2))
    htrc("GetSectionNames: section=%p\n", section);

  while (section != NULL) {
    if (trace(2))
      htrc("section=%s\n", section->name);

    if (section->name[0]) {
      l = strlen(section->name) + 1;

      if (trace(2))
        htrc("l=%u f=%u\n", l, f);

      if (l > f) {
        if (f > 0) {
          strncpy(buf, section->name, f-1);
          buf += f-1;
          *buf++='\0';
          } // endif f

        *buf = '\0';
        return len - 2;
        } // endif l

      strcpy(buf, section->name);
      buf += l;
      f -= l;
      } // endif section->name

    section = section->next;
    } // endwhile section

  *buf='\0';
  return buf-buffer;
}  // end of  PROFILE_GetSectionNames


/***********************************************************************
 *           PROFILE_GetString
 *
 * Get a profile string.
 *
 * Tests with GetPrivateProfileString16, W95a,
 * with filled buffer ("****...") and section "set1" and key_name "1" valid:
 * section      key_name        def_val         res     buffer
 * "set1"       "1"             "x"             43      [data]
 * "set1"       "1   "          "x"             43      [data]          (!)
 * "set1"       "  1  "'        "x"             43      [data]          (!)
 * "set1"       ""              "x"             1       "x"
 * "set1"       ""              "x   "          1       "x"             (!)
 * "set1"       ""              "  x   "        3       "  x"           (!)
 * "set1"       NULL            "x"             6       "1\02\03\0\0"
 * "set1"       ""              "x"             1       "x"
 * NULL         "1"             "x"             0       ""              (!)
 * ""           "1"             "x"             1       "x"
 * NULL         NULL            ""              0       ""
 *
 *************************************************************************/
static int PROFILE_GetString(LPCSTR section, LPCSTR key_name,
                             LPCSTR def_val, LPSTR buffer, uint len)
{
  PROFILEKEY *key = NULL;

  if(!buffer)
    return 0;

  if (!def_val)
    def_val = "";

  if (key_name && key_name[0]) {
    key = PROFILE_Find(&CurProfile->section, section, key_name, FALSE, FALSE);
    PROFILE_CopyEntry(buffer, (key && key->value) ? key->value : def_val, len, FALSE);
    
    if (trace(2))
      htrc("('%s','%s','%s'): returning '%s'\n", 
            section, key_name, def_val, buffer );

    return strlen(buffer);
    } // endif key_name

  if (key_name && !(key_name[0]))
    /* Win95 returns 0 on keyname "". Tested with Likse32 bon 000227 */
    return 0;

  if (section && section[0])
    return PROFILE_GetSection(CurProfile->section, section, buffer, len,
                              FALSE, FALSE);
  buffer[0] = '\0';
  return 0;
}  // end of PROFILE_GetString


/***********************************************************************
 *           PROFILE_SetString
 *
 * Set a profile string.
 ***********************************************************************/
static BOOL PROFILE_SetString(LPCSTR section_name, LPCSTR key_name,
                              LPCSTR value, BOOL create_always)
{
  if (!key_name) {       /* Delete a whole section */
    if (trace(2))
      htrc("Deleting('%s')\n", section_name);

    CurProfile->changed |= PROFILE_DeleteSection(&CurProfile->section,
                                                 section_name);
    return TRUE;         /* Even if PROFILE_DeleteSection() has failed,
                            this is not an error on application's level.*/
  } else if (!value) {   /* Delete a key */
    if (trace(2))
      htrc("Deleting('%s','%s')\n", section_name, key_name);

    CurProfile->changed |= PROFILE_DeleteKey(&CurProfile->section,
                                             section_name, key_name);
    return TRUE;         /* same error handling as above */
  } else {               /* Set the key value */
    PROFILEKEY *key = PROFILE_Find(&CurProfile->section, section_name,
                                    key_name, TRUE, create_always);
    if (trace(2))
      htrc("Setting('%s','%s','%s')\n", section_name, key_name, value);

    if (!key)
      return FALSE;

    if (key->value) {
      /* strip the leading spaces. We can safely strip \n\r and
       * friends too, they should not happen here anyway. */
      while (PROFILE_isspace(*value))
        value++;

      if (!strcmp(key->value, value)) {
        if (trace(2))
          htrc("  no change needed\n" );

        return TRUE;     /* No change needed */
        }  // endif value

      if (trace(2))
        htrc("  replacing '%s'\n", key->value);

      free(key->value);
    }  else if (trace(2))
      htrc("  creating key\n" );

    key->value = (char*)malloc(strlen(value) + 1);
    strcpy(key->value, value);
    CurProfile->changed = TRUE;
  } // endelse

  return TRUE;
}  // end of PROFILE_SetString


/***********************************************************************
 *           PROFILE_GetStringItem
 *
 *  Convenience function that turns a string 'xxx, yyy, zzz' into
 *  the 'xxx\0 yyy, zzz' and returns a pointer to the 'yyy, zzz'.
 ***********************************************************************/
#if 0
char *PROFILE_GetStringItem(char* start)
{
  char *lpchX, *lpch;

  for (lpchX = start, lpch = NULL; *lpchX != '\0'; lpchX++) {
    if (*lpchX == ',') {
      if (lpch)
        *lpch = '\0';
      else
        *lpchX = '\0';

      while(*(++lpchX))
        if (!PROFILE_isspace(*lpchX))
          return lpchX;

    } else if (PROFILE_isspace(*lpchX) && !lpch) {
      lpch = lpchX;
    } else
      lpch = NULL;

    } // endfor lpchX

  if (lpch)
    *lpch = '\0';

  return NULL;
}  // end of PROFILE_GetStringItem
#endif

/**********************************************************************
 * if allow_section_name_copy is TRUE, allow the copying :
 *   - of Section names if 'section' is NULL
 *   - of Keys in a Section if 'entry' is NULL
 * (see MSDN doc for GetPrivateProfileString)
 **********************************************************************/
static int PROFILE_GetPrivateProfileString(LPCSTR section, LPCSTR entry,
                                           LPCSTR def_val, LPSTR buffer,
                                           uint len, LPCSTR filename,
                                           BOOL allow_section_name_copy)
{
  int   ret;
  LPSTR pDefVal = NULL;

  if (!filename)
    filename = "win.ini";

  /* strip any trailing ' ' of def_val. */
  if (def_val) {
    LPSTR p = (LPSTR)&def_val[strlen(def_val)]; // even "" works !

    while (p > def_val)
      if ((*(--p)) != ' ')
        break;

    if (*p == ' ') {        /* ouch, contained trailing ' ' */
      int len = p - (LPSTR)def_val;

      pDefVal = (LPSTR)malloc(len + 1);
      strncpy(pDefVal, def_val, len);
      pDefVal[len] = '\0';
      } // endif *p

    } // endif def_val

  if (!pDefVal)
    pDefVal = (LPSTR)def_val;

  EnterCriticalSection(&PROFILE_CritSect);

  if (PROFILE_Open(filename)) {
    if ((allow_section_name_copy) && (section == NULL))
      ret = PROFILE_GetSectionNames(buffer, len);
    else
      /* PROFILE_GetString already handles the 'entry == NULL' case */
      ret = PROFILE_GetString(section, entry, pDefVal, buffer, len);

  } else {
    strncpy(buffer, pDefVal, len);
    ret = strlen(buffer);
  }  // endif Open

  LeaveCriticalSection(&PROFILE_CritSect);

  if (pDefVal != def_val) /* allocated */
    memfree(pDefVal);

  return ret;
}  // end of PROFILE_GetPrivateProfileString

/********************** API functions **********************************/

/***********************************************************************
 *           GetPrivateProfileStringA   (KERNEL32.@)
 ***********************************************************************/
int GetPrivateProfileString(LPCSTR section, LPCSTR entry, LPCSTR def_val,
                            LPSTR buffer, DWORD len, LPCSTR filename)
{
  return PROFILE_GetPrivateProfileString(section, entry, def_val,
                                         buffer, len, filename, TRUE);
} // end of GetPrivateProfileString


/***********************************************************************
 *           GetPrivateProfileIntA   (KERNEL32.@)
 ***********************************************************************/
uint GetPrivateProfileInt(LPCSTR section, LPCSTR entry,
                          int def_val, LPCSTR filename)
{
  char buffer[20];
  int  result;

  if (!PROFILE_GetPrivateProfileString(section, entry, "", buffer,
                                       sizeof(buffer), filename, FALSE))
    return def_val;

  /* FIXME: if entry can be found but it's empty, then Win16 is
   * supposed to return 0 instead of def_val ! Difficult/problematic
   * to implement (every other failure also returns zero buffer),
   * thus wait until testing framework avail for making sure nothing
   * else gets broken that way. */
  if (!buffer[0])
    return (uint)def_val;

  /* Don't use strtol() here !
   * (returns LONG_MAX/MIN on overflow instead of "proper" overflow)
   YES, scan for unsigned format ! (otherwise compatibility error) */
  if (!sscanf(buffer, "%u", &result))
    return 0;

  return (uint)result;
}  // end of GetPrivateProfileInt


/***********************************************************************
 *           GetPrivateProfileSectionA   (KERNEL32.@)
 ***********************************************************************/
int GetPrivateProfileSection(LPCSTR section, LPSTR buffer,
                             DWORD len, LPCSTR filename)
{
  int ret = 0;

  EnterCriticalSection( &PROFILE_CritSect );

  if (PROFILE_Open(filename))
    ret = PROFILE_GetSection(CurProfile->section, section, buffer, len,
                             FALSE, TRUE);

  LeaveCriticalSection( &PROFILE_CritSect );
  return ret;
}  // end of GetPrivateProfileSection


/***********************************************************************
 *           WritePrivateProfileStringA   (KERNEL32.@)
 ***********************************************************************/
BOOL WritePrivateProfileString(LPCSTR section, LPCSTR entry,
                               LPCSTR string, LPCSTR filename)
{
  BOOL ret = FALSE;

  EnterCriticalSection( &PROFILE_CritSect );

  if (PROFILE_Open(filename)) {
    if (!section && !entry && !string) /* documented "file flush" case */
      PROFILE_ReleaseFile();  /* always return FALSE in this case */
    else {
      if (!section) {
        //FIXME("(NULL?,%s,%s,%s)? \n",entry,string,filename);
      } else {
        ret = PROFILE_SetString(section, entry, string, FALSE);

        if (ret)
          ret = PROFILE_FlushFile();

      } // endif section

    } // endif section || entry|| string

    } // endif Open

  LeaveCriticalSection( &PROFILE_CritSect );
  return ret;
}  // end of WritePrivateProfileString


/***********************************************************************
 *           WritePrivateProfileSectionA   (KERNEL32.@)
 ***********************************************************************/
BOOL WritePrivateProfileSection(LPCSTR section,
                                LPCSTR string, LPCSTR filename )
{
  BOOL  ret = FALSE;
  LPSTR p ;

  EnterCriticalSection(&PROFILE_CritSect);

  if (PROFILE_Open(filename)) {
    if (!section && !string)
      PROFILE_ReleaseFile();  /* always return FALSE in this case */
    else if (!string) {       /* delete the named section*/
      ret = PROFILE_SetString(section, NULL, NULL, FALSE);

      if (ret)
        ret = PROFILE_FlushFile();
    }  else {
      PROFILE_DeleteAllKeys(section);
      ret = TRUE;

      while (*string) {
        LPSTR buf = (LPSTR)malloc(strlen(string) + 1);
        strcpy(buf, string);

        if ((p = strchr(buf, '='))) {
          *p='\0';
          ret = PROFILE_SetString(section, buf, p+1, TRUE);
          } // endif p

        free(buf);
        string += strlen(string) + 1;

        if (ret)
          ret = PROFILE_FlushFile();

        } // endwhile *string

    }  // endelse

    }  // endif Open

  LeaveCriticalSection(&PROFILE_CritSect);
  return ret;
}  // end of WritePrivateProfileSection


/***********************************************************************
 *           GetPrivateProfileSectionNamesA  (KERNEL32.@)
 *
 * Returns the section names contained in the specified file.
 * FIXME: Where do we find this file when the path is relative?
 * The section names are returned as a list of strings with an extra
 * '\0' to mark the end of the list. Except for that the behavior
 * depends on the Windows version.
 *
 * Win95:
 * - if the buffer is 0 or 1 character long then it is as if it was of
 *   infinite length.
 * - otherwise, if the buffer is to small only the section names that fit
 *   are returned.
 * - note that this means if the buffer was to small to return even just
 *   the first section name then a single '\0' will be returned.
 * - the return value is the number of characters written in the buffer,
 *   except if the buffer was too smal in which case len-2 is returned
 *
 * Win2000:
 * - if the buffer is 0, 1 or 2 characters long then it is filled with
 *   '\0' and the return value is 0
 * - otherwise if the buffer is too small then the first section name that
 *   does not fit is truncated so that the string list can be terminated
 *   correctly (double '\0')
 * - the return value is the number of characters written in the buffer
 *   except for the trailing '\0'. If the buffer is too small, then the
 *   return value is len-2
 * - Win2000 has a bug that triggers when the section names and the
 *   trailing '\0' fit exactly in the buffer. In that case the trailing
 *   '\0' is missing.
 *
 * Wine implements the observed Win2000 behavior (except for the bug).
 *
 * Note that when the buffer is big enough then the return value may be any
 * value between 1 and len-1 (or len in Win95), including len-2.
 */
#ifdef TEST_MODULE
static DWORD
GetPrivateProfileSectionNames(LPSTR buffer, DWORD size,  LPCSTR filename)
{
  DWORD ret = 0;
  
  if (trace(2))
    htrc("GPPSN: filename=%s\n", filename);

  EnterCriticalSection(&PROFILE_CritSect);

  if (PROFILE_Open(filename))
    ret = PROFILE_GetSectionNames(buffer, size);

  LeaveCriticalSection(&PROFILE_CritSect);
  return ret;
}  // end of GetPrivateProfileSectionNames


/************************************************************************
 * Program to test the above
 ************************************************************************/
int main(int argc, char**argv) {
  char  buff[128];
  char *p, *inifile = "D:\\Plug\\Data\\contact.ini";
  DWORD n;

  n = GetPrivateProfileSectionNames(buff, 128, inifile);
  printf("Sections: n=%d\n", n);

  for (p = buff; *p; p += (strlen(p) + 1))
    printf("section=[%s]\n", p);

  GetPrivateProfileString("BER", "name", "?", buff, 128, inifile);
  printf("[BER](name) = %s\n", buff);

  WritePrivateProfileString("FOO", "city", NULL, inifile);
  GetPrivateProfileString("FOO", "city", "?", buff, 128, inifile);
  printf("[FOO](city) = %s\n", buff);

  printf("FOO city: "); 
  fgets(buff, sizeof(buff), stdin);
  if (buff[strlen(buff) - 1] == '\n') 
      buff[strlen(buff) - 1] = '\0'; 
  WritePrivateProfileString("FOO", "city", buff, inifile);
  GetPrivateProfileString("FOO", "city", "???", buff, 128, inifile);
  printf("After write, [FOO](City) = %s\n", buff);

  printf("New city: "); 
  fgets(buff, sizeof(buff), stdin);
  if (buff[strlen(buff) - 1] == '\n') 
      buff[strlen(buff) - 1] = '\0'; 
  WritePrivateProfileString("FOO", "city", buff, inifile);
  GetPrivateProfileString("FOO", "city", "???", buff, 128, inifile);
  printf("After update, [FOO](City) = %s\n", buff);

  printf("FOO name: "); 
  fgets(buff, sizeof(buff), stdin);
  if (buff[strlen(buff) - 1] == '\n') 
      buff[strlen(buff) - 1] = '\0'; 
  WritePrivateProfileString("FOO", "name", buff, inifile);
  GetPrivateProfileString("FOO", "name", "X", buff, 128, inifile);
  printf("[FOO](name) = %s\n", buff);
}  // end of main
#endif // TEST_MODULE
