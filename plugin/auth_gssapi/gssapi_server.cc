#include <my_config.h>
#include <gssapi/gssapi.h>
#include <stdio.h>
#include <mysql/plugin_auth.h>
#include <my_sys.h>
#include <mysqld_error.h>
#include <log.h>
#include "server_plugin.h"
#include "gssapi_errmsg.h"

static gss_name_t service_name = GSS_C_NO_NAME;

/* This sends the error to the client */
static void log_error( OM_uint32 major, OM_uint32 minor, const char *msg)
{
  if (GSS_ERROR(major))
  {
    char sysmsg[1024];
    gssapi_errmsg(major, minor, sysmsg, sizeof(sysmsg));
    my_printf_error(ER_UNKNOWN_ERROR,"Server GSSAPI error (major %u, minor %u) : %s -%s",
      MYF(0), major, minor, msg, sysmsg);
  }
  else
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Server GSSAPI error : %s", MYF(0), msg);
  }
}


/*
  Generate default principal service name formatted as principal name "mariadb/server.fqdn@REALM"
*/
#include <krb5.h>
#ifndef HAVE_KRB5_FREE_UNPARSED_NAME
#define krb5_free_unparsed_name(a,b) krb5_xfree(b)
#endif
static char* get_default_principal_name()
{
  static char default_name[1024];
  char *unparsed_name= NULL;
  krb5_context context= NULL;
  krb5_principal principal= NULL;
  krb5_keyblock *key= NULL;

  if(krb5_init_context(&context))
  {
    sql_print_warning("GSSAPI plugin : krb5_init_context failed");
    goto cleanup;
  }

  if (krb5_sname_to_principal(context, NULL, "mariadb", KRB5_NT_SRV_HST, &principal))
  {
    sql_print_warning("GSSAPI plugin :  krb5_sname_to_principal failed");
    goto cleanup;
  }

  if (krb5_unparse_name(context, principal, &unparsed_name))
  {
    sql_print_warning("GSSAPI plugin :  krb5_unparse_name failed");
    goto cleanup;
  }

  /* Check for entry in keytab */
  if (krb5_kt_read_service_key(context, NULL, principal, 0, (krb5_enctype)0, &key))
  {
    sql_print_warning("GSSAPI plugin : default principal '%s' not found in keytab", unparsed_name);
    goto cleanup;
  }

  strncpy(default_name, unparsed_name, sizeof(default_name)-1);

cleanup:
  if (key)
    krb5_free_keyblock(context, key);
  if (unparsed_name)
    krb5_free_unparsed_name(context, unparsed_name);
  if (principal)
    krb5_free_principal(context, principal);
  if (context)
    krb5_free_context(context);

  return default_name;
}


int plugin_init()
{
  gss_buffer_desc principal_name_buf;
  OM_uint32 major= 0, minor= 0;
  gss_cred_id_t cred= GSS_C_NO_CREDENTIAL;

  if(srv_keytab_path && srv_keytab_path[0])
  {
     setenv("KRB5_KTNAME", srv_keytab_path, 1);
  }

  if(!srv_principal_name || !srv_principal_name[0])
    srv_principal_name= get_default_principal_name();

  /* import service principal from plain text */
  if(srv_principal_name && srv_principal_name[0])
  {
    sql_print_information("GSSAPI plugin : using principal name '%s'", srv_principal_name);
    principal_name_buf.length= strlen(srv_principal_name);
    principal_name_buf.value= srv_principal_name;
    major= gss_import_name(&minor, &principal_name_buf, GSS_C_NT_USER_NAME, &service_name);
    if(GSS_ERROR(major))
    {
      log_error(major, minor, "gss_import_name");
      return -1;
    }
  }
  else
  {
    service_name=  GSS_C_NO_NAME;
  }



  /* Check if SPN configuration is OK */
  major= gss_acquire_cred(&minor, service_name, GSS_C_INDEFINITE,
                            GSS_C_NO_OID_SET, GSS_C_ACCEPT, &cred, NULL,
                            NULL);

  if (GSS_ERROR(major))
  {
    log_error(major, minor, "gss_acquire_cred failed");
    return -1;
  }
  gss_release_cred(&minor, &cred);

  return 0;
}

int plugin_deinit()
{
  if (service_name != GSS_C_NO_NAME)
  {
    OM_uint32 minor;
    gss_release_name(&minor, &service_name);
  }
  return 0;
}


int auth_server(MYSQL_PLUGIN_VIO *vio,const char *user, size_t userlen, int use_full_name)
{

  int rc= CR_ERROR; /* return code */

  /* GSSAPI related fields */
  OM_uint32 major= 0, minor= 0, flags= 0;
  gss_cred_id_t cred= GSS_C_NO_CREDENTIAL; /* credential identifier */
  gss_ctx_id_t ctxt= GSS_C_NO_CONTEXT; /* context identifier */
  gss_name_t client_name;
  gss_buffer_desc client_name_buf, input, output;
  char *client_name_str;

  /* server acquires credential */
  major= gss_acquire_cred(&minor, service_name, GSS_C_INDEFINITE,
                            GSS_C_NO_OID_SET, GSS_C_ACCEPT, &cred, NULL,
                            NULL);

  if (GSS_ERROR(major))
  {
    log_error(major, minor, "gss_acquire_cred failed");
    goto cleanup;
  }

  input.length= 0;
  input.value= NULL;
  do
  {
    /* receive token from peer */
    int len= vio->read_packet(vio, (unsigned char **) &input.value);
    if (len < 0)
    {
      log_error(0, 0, "fail to read token from client");
      goto cleanup;
    }

    input.length= len;
    major= gss_accept_sec_context(&minor, &ctxt, cred, &input,
                                  GSS_C_NO_CHANNEL_BINDINGS, &client_name,
                                  NULL, &output, &flags, NULL, NULL);
    if (GSS_ERROR(major))
    {

      log_error(major, minor, "gss_accept_sec_context");
      rc= CR_ERROR;
      goto cleanup;
    }

    /* send token to peer */
    if (output.length)
    {
      if (vio->write_packet(vio, (const uchar *) output.value, output.length))
      {
        gss_release_buffer(&minor, &output);
        log_error(major, minor, "communication error(write)");
        goto cleanup;
      }
      gss_release_buffer(&minor, &output);
    }
  } while (major & GSS_S_CONTINUE_NEEDED);

  /* extract plain text client name */
  major= gss_display_name(&minor, client_name, &client_name_buf, NULL);
  if (GSS_ERROR(major))
  {
    log_error(major, minor, "gss_display_name");
    goto cleanup;
  }

  client_name_str= (char *)client_name_buf.value;

  /*
   * Compare input user name with the actual one. Return success if
   * the names match exactly, or if use_full_name parameter is not set
   * up to the '@' separator.
   */
  if ((userlen == client_name_buf.length) ||
      (!use_full_name
       && userlen < client_name_buf.length
       && client_name_str[userlen] == '@'))
  {
    if (strncmp(client_name_str, user, userlen) == 0)
    {
      rc= CR_OK;
    }
  }

  if(rc != CR_OK)
  {
    my_printf_error(ER_ACCESS_DENIED_ERROR,
      "GSSAPI name mismatch, requested '%s', actual name '%.*s'",
      MYF(0), user, (int)client_name_buf.length, client_name_str);
  }

  gss_release_buffer(&minor, &client_name_buf);


cleanup:
  if (ctxt != GSS_C_NO_CONTEXT)
    gss_delete_sec_context(&minor, &ctxt, GSS_C_NO_BUFFER);
  if (cred != GSS_C_NO_CREDENTIAL)
    gss_release_cred(&minor, &cred);

  return(rc);
}
