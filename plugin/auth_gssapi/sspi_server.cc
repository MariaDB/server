/* Copyright (c) 2015, Shuang Qiu, Robbie Harwood,
Vladislav Vaintroub & MariaDB Corporation

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include "sspi.h"
#include "common.h"
#include "server_plugin.h"
#include <mysql/plugin_auth.h>
#include <mysqld_error.h>
#include <sddl.h>

/* This sends the error to the client */
static void log_error(SECURITY_STATUS err, const char *msg)
{
  if (err)
  {
    char buf[1024];
    sspi_errmsg(err, buf, sizeof(buf));
    my_printf_error(ER_UNKNOWN_ERROR, "SSPI server error 0x%x - %s - %s", 0, err, msg, buf);
  }
  else
  {
    my_printf_error(ER_UNKNOWN_ERROR, "SSPI server error %s", 0, msg);
  }

}

static char INVALID_KERBEROS_PRINCIPAL[] = "localhost";

static char *get_default_principal_name()
{
  static char default_principal[PRINCIPAL_NAME_MAX +1];
  ULONG size= sizeof(default_principal);

  if (GetUserNameEx(NameUserPrincipal,default_principal,&size))
    return default_principal;

  size= sizeof(default_principal);
  if (GetUserNameEx(NameServicePrincipal,default_principal,&size))
    return default_principal;

  char domain[PRINCIPAL_NAME_MAX+1];
  char host[PRINCIPAL_NAME_MAX+1];
  size= sizeof(domain);
  if (GetComputerNameEx(ComputerNameDnsDomain,domain,&size) && size > 0)
  {
    size= sizeof(host);
    if (GetComputerNameEx(ComputerNameDnsHostname,host,&size))
    {
      _snprintf(default_principal,sizeof(default_principal),"%s$@%s",host, domain);
      return default_principal;
    }
  }
  /* Unable to retrieve useful name, return something */
  return INVALID_KERBEROS_PRINCIPAL;
}


/* Extract client name from SSPI context */
static int get_client_name_from_context(CtxtHandle *ctxt,
  char *name,
  size_t name_len,
  int use_full_name)
{
  SecPkgContext_NativeNames native_names;
  SECURITY_STATUS sspi_ret;
  char *p;

  sspi_ret= QueryContextAttributes(ctxt, SECPKG_ATTR_NATIVE_NAMES, &native_names);
  if (sspi_ret == SEC_E_OK)
  {
    /* Extract user from Kerberos principal name user@realm */
    if(!use_full_name)
    {
      p = strrchr(native_names.sClientName,'@');
      if(p)
        *p = 0;
    }
    strncpy(name, native_names.sClientName, name_len);

    if (native_names.sClientName)
      FreeContextBuffer(native_names.sClientName);
    if (native_names.sServerName)
      FreeContextBuffer(native_names.sServerName);

    return CR_OK;
  }

  sspi_ret= ImpersonateSecurityContext(ctxt);
  if (sspi_ret == SEC_E_OK)
  {
    ULONG len= (ULONG)name_len;
    if (!GetUserNameEx(NameSamCompatible, name, &len))
    {
      log_error(GetLastError(), "GetUserNameEx");
      RevertSecurityContext(ctxt);
      return CR_ERROR;
    }
    RevertSecurityContext(ctxt);

    /* Extract user from Windows name realm\user */
    if (!use_full_name)
    {
      p = strrchr(name, '\\');
      if (p)
      {
        p++;
        memmove(name, p, name + len + 1 - p);
      }
    }
    return CR_OK;
  }

  log_error(sspi_ret, "ImpersonateSecurityContext");
  return CR_ERROR;
}


/*
  Check if username from SSPI context matches the name requested
  in MYSQL_SERVER_AUTH_INFO

  There are 2 ways to specify SSPI username
  username, of auth_string.

  if auth_string is used,  we compare full name (i.e , with user+domain)
  if not, we match just the user name.
*/
static bool check_username_match(CtxtHandle *ctxt,
  MYSQL_SERVER_AUTH_INFO *auth_info)
{
  char client_name[MYSQL_USERNAME_LENGTH + 1];
  const char *user= 0;
  int compare_full_name;;
  if (auth_info->auth_string_length > 0)
  {
    compare_full_name= 1;
    user= auth_info->auth_string;
  }
  else
  {
    compare_full_name= 0;
    user= auth_info->user_name;
  }
  if (get_client_name_from_context(ctxt, client_name, MYSQL_USERNAME_LENGTH,
                                   compare_full_name) != CR_OK)
  {
    return false;
  }

  /* Always compare case-insensitive on Windows. */
  if (_stricmp(client_name, user))
  {
    my_printf_error(ER_ACCESS_DENIED_ERROR,
      "GSSAPI name mismatch, requested '%s', actual name '%s'", 0, user,
      client_name);
    return false;
  }
  return true;
}


/*
  Checks the security token extracted from SSPI context
  for membership in specfied group.

  @param ctxt  -  SSPI context
  @param group_name - group name to check membership against
  NOTE: this can also be a user's name

  @param use_sid - whether name is SID
  @last_error - will be set, if the function returns false, and
  some of the API's have failed.
  @failing_api - name of the API that has failed(for error logging)
*/
static bool check_group_match(CtxtHandle *ctxt, const char *name,
  bool name_is_sid)
{
  BOOL is_member= FALSE;
  bool is_impersonating= false;
  bool free_sid= false;
  PSID sid= 0;


#define FAIL(msg)                                                             \
  do                                                                          \
  {                                                                           \
    log_error(GetLastError(), msg);                                           \
    goto cleanup;                                                             \
  } while (0)

  /* Get the group SID.*/
  if (name_is_sid)
  {
    if (!ConvertStringSidToSidA(name, &sid))
      FAIL("ConvertStringSidToSid");
    free_sid= true;
  }
  else
  {
    /* Get the SID of the specified group via LookupAccountName().*/
    char sid_buf[SECURITY_MAX_SID_SIZE];
    char domain[256];
    DWORD sid_size= sizeof(sid_buf);
    DWORD domain_size= sizeof(domain);

    SID_NAME_USE sid_name_use;
    sid= (PSID) sid_buf;

    if (!LookupAccountName(0, name, sid, &sid_size, domain,
          &domain_size, &sid_name_use))
    {
      FAIL("LookupAccountName");
    }
  }

  /* Impersonate, to check group membership */
  if (ImpersonateSecurityContext(ctxt))
    FAIL("ImpersonateSecurityContext");
  is_impersonating= true;

  if (!CheckTokenMembership(GetCurrentThreadToken(), sid, &is_member))
      FAIL("CheckTokenMembership");

cleanup:
  if (is_impersonating)
    RevertSecurityContext(ctxt);
  if (free_sid)
    LocalFree(sid);
  return is_member;
}

static SECURITY_STATUS sspi_get_context(MYSQL_PLUGIN_VIO *vio,
  CtxtHandle *ctxt, CredHandle *cred)
{
  SECURITY_STATUS sspi_ret= SEC_E_OK;
  ULONG attribs= 0;
  TimeStamp lifetime;
  SecBufferDesc inbuf_desc;
  SecBuffer     inbuf;
  SecBufferDesc outbuf_desc;
  SecBuffer     outbuf;
  void*         out= NULL;

  SecInvalidateHandle(cred);
  SecInvalidateHandle(ctxt);

  out= malloc(SSPI_MAX_TOKEN_SIZE);
  if (!out)
  {
    log_error(SEC_E_OK, "memory allocation failed");
    sspi_ret= SEC_E_INSUFFICIENT_MEMORY;
    goto cleanup;
  }
  sspi_ret= AcquireCredentialsHandle(
    srv_principal_name,
    (LPSTR)srv_mech_name,
    SECPKG_CRED_INBOUND,
    NULL,
    NULL,
    NULL,
    NULL,
    cred,
    &lifetime);

  if (SEC_ERROR(sspi_ret))
  {
    log_error(sspi_ret, "AcquireCredentialsHandle failed");
    goto cleanup;
  }

  inbuf.cbBuffer= 0;
  inbuf.BufferType= SECBUFFER_TOKEN;
  inbuf.pvBuffer= NULL;
  inbuf_desc.ulVersion= SECBUFFER_VERSION;
  inbuf_desc.cBuffers= 1;
  inbuf_desc.pBuffers= &inbuf;

  outbuf.BufferType= SECBUFFER_TOKEN;
  outbuf.cbBuffer= SSPI_MAX_TOKEN_SIZE;
  outbuf.pvBuffer= out;

  outbuf_desc.ulVersion= SECBUFFER_VERSION;
  outbuf_desc.cBuffers= 1;
  outbuf_desc.pBuffers= &outbuf;

  do
  {
    /* Read SSPI blob from client. */
    int len= vio->read_packet(vio, (unsigned char **)&inbuf.pvBuffer);
    if (len < 0)
    {
      log_error(SEC_E_OK, "communication error(read)");
      goto cleanup;
    }
    inbuf.cbBuffer= len;
    outbuf.cbBuffer= SSPI_MAX_TOKEN_SIZE;
    sspi_ret= AcceptSecurityContext(
      cred,
      SecIsValidHandle(ctxt) ? ctxt : NULL,
      &inbuf_desc,
      attribs,
      SECURITY_NATIVE_DREP,
      ctxt,
      &outbuf_desc,
      &attribs,
      &lifetime);

    if (SEC_ERROR(sspi_ret))
    {
      log_error(sspi_ret, "AcceptSecurityContext");
      goto cleanup;
    }
    if (sspi_ret != SEC_E_OK && sspi_ret != SEC_I_CONTINUE_NEEDED)
    {
      log_error(sspi_ret, "AcceptSecurityContext unexpected return value");
      goto cleanup;
    }
    if (outbuf.cbBuffer)
    {
      /* Send generated blob to client. */
      if (vio->write_packet(vio, (unsigned char *)outbuf.pvBuffer, outbuf.cbBuffer))
      {
        log_error(SEC_E_OK, "communicaton error(write)");
        goto cleanup;
      }
    }
  } while (sspi_ret == SEC_I_CONTINUE_NEEDED);

cleanup:
  free(out);
  return sspi_ret;
}


int auth_server(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *auth_info)
{
  int ret= CR_ERROR;
  const char* group = 0;
  bool use_sid = 0;

  CtxtHandle ctxt;
  CredHandle cred;
  if (sspi_get_context(vio, &ctxt, &cred) != SEC_E_OK)
    goto cleanup;


  /*
    Authentication done, now test user name, or group
    membership.
    First, find out if matching group was requested.
  */
  static struct
  {
    const char *str;
    size_t len;
    bool sid;
  } prefixes[]= {{"GROUP:", sizeof("GROUP:") - 1, false},
                 {"SID:", sizeof("SID:") - 1, true}};
  group= 0;
  for (auto &prefix : prefixes)
  {
    if (auth_info->auth_string_length >= prefix.len &&
        !strncmp(auth_info->auth_string, prefix.str, prefix.len))
    {
      group= auth_info->auth_string + prefix.len;
      use_sid= prefix.sid;
      break;
    }
  }

  if (group)
  {
    /* Test group membership.*/
    ret= check_group_match(&ctxt, group, use_sid) ? CR_OK : CR_ERROR;
  }
  else
  {
    /* Compare username. */
    ret= check_username_match(&ctxt, auth_info) ? CR_OK : CR_ERROR;
  }

cleanup:
  if (SecIsValidHandle(&ctxt))
    DeleteSecurityContext(&ctxt);

  if (SecIsValidHandle(&cred))
    FreeCredentialsHandle(&cred);

  return ret;
}

int plugin_init()
{
  CredHandle cred;
  SECURITY_STATUS ret;

  /*
    Use negotiate by default, which accepts raw kerberos
    and also NTLM.
  */
  if (srv_mech == PLUGIN_MECH_DEFAULT)
    srv_mech=  PLUGIN_MECH_SPNEGO;

  if(srv_mech == PLUGIN_MECH_KERBEROS)
    srv_mech_name= "Kerberos";
  else if(srv_mech == PLUGIN_MECH_SPNEGO )
    srv_mech_name= "Negotiate";

  if(!srv_principal_name[0])
  {
    srv_principal_name= get_default_principal_name();
  }
  my_printf_error(ER_UNKNOWN_ERROR, "SSPI: using principal name '%s', mech '%s'",
                  ME_ERROR_LOG | ME_NOTE, srv_principal_name, srv_mech_name);

  ret = AcquireCredentialsHandle(
    srv_principal_name,
    (LPSTR)srv_mech_name,
    SECPKG_CRED_INBOUND,
    NULL,
    NULL,
    NULL,
    NULL,
    &cred,
    NULL);
  if (SEC_ERROR(ret))
  {
    log_error(ret, "AcquireCredentialsHandle");
    return -1;
  }
  FreeCredentialsHandle(&cred);
  return 0;
}

int plugin_deinit()
{
  return 0;
}
