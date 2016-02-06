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
#include <my_sys.h>
#include <mysqld_error.h>
#include <log.h>


/* This sends the error to the client */
static void log_error(SECURITY_STATUS err, const char *msg)
{
  if (err)
  {
    char buf[1024];
    sspi_errmsg(err, buf, sizeof(buf));
    my_printf_error(ER_UNKNOWN_ERROR, "SSPI server error 0x%x - %s - %s", MYF(0), msg, buf);
  }
  else
  {
    my_printf_error(ER_UNKNOWN_ERROR, "SSPI server error %s", MYF(0), msg);
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
    FreeContextBuffer(&native_names);
    return CR_OK;
  }

  sspi_ret= ImpersonateSecurityContext(ctxt);
  if (sspi_ret == SEC_E_OK)
  {
    ULONG len= name_len;
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


int auth_server(MYSQL_PLUGIN_VIO *vio, const char *user, size_t user_len, int compare_full_name)
{
  int ret;
  SECURITY_STATUS sspi_ret;
  ULONG  attribs = 0;
  TimeStamp   lifetime;
  CredHandle  cred;
  CtxtHandle  ctxt;

  SecBufferDesc inbuf_desc;
  SecBuffer     inbuf;
  SecBufferDesc outbuf_desc;
  SecBuffer     outbuf;
  void*         out= NULL;
  char client_name[MYSQL_USERNAME_LENGTH + 1];

  ret= CR_ERROR;
  SecInvalidateHandle(&cred);
  SecInvalidateHandle(&ctxt);

  out= malloc(SSPI_MAX_TOKEN_SIZE);
  if (!out)
  {
    log_error(SEC_E_OK, "memory allocation failed");
    goto cleanup;
  }
  sspi_ret= AcquireCredentialsHandle(
    srv_principal_name,
    srv_mech_name,
    SECPKG_CRED_INBOUND,
    NULL,
    NULL,
    NULL,
    NULL,
    &cred,
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
      &cred,
      SecIsValidHandle(&ctxt) ? &ctxt : NULL,
      &inbuf_desc,
      attribs,
      SECURITY_NATIVE_DREP,
      &ctxt,
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

  /* Authentication done, now extract and compare user name. */
  ret= get_client_name_from_context(&ctxt, client_name, MYSQL_USERNAME_LENGTH, compare_full_name);
  if (ret != CR_OK)
    goto cleanup;

  /* Always compare case-insensitive on Windows. */
  ret= _stricmp(client_name, user) == 0 ? CR_OK : CR_ERROR;
  if (ret != CR_OK)
  {
    my_printf_error(ER_ACCESS_DENIED_ERROR,
      "GSSAPI name mismatch, requested '%s', actual name '%s'",
      MYF(0), user, client_name);
  }

cleanup:
  if (SecIsValidHandle(&ctxt))
    DeleteSecurityContext(&ctxt);

  if (SecIsValidHandle(&cred))
    FreeCredentialsHandle(&cred);

  free(out);
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
  sql_print_information("SSPI: using principal name '%s', mech '%s'",
    srv_principal_name, srv_mech_name);

  ret = AcquireCredentialsHandle(
    srv_principal_name,
    srv_mech_name,
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
