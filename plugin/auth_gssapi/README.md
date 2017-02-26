# GSSAPI/SSPI authentication for MariaDB

This article gives instructions on configuring GSSAPI authentication plugin
for MariaDB for passwordless login.

On Unix systems, GSSAPI is usually synonymous with Kerberos authentication.
Windows has slightly different but very similar API called SSPI,  that along with Kerberos, also supports NTLM authentication.

This plugin includes support for Kerberos on Unix, but also can be used as for Windows authentication with or without domain
environment.

## Server-side preparations on Unix
To use the plugin, some preparation need to be done on the server side on Unixes.
MariaDB server will need read access to the Kerberos keytab file, that contains  service principal name for the MariaDB server.


If you are using **Unix Kerberos KDC (MIT,Heimdal)**

-	Create service principal using kadmin tool

```
kadmin -q "addprinc -randkey mariadb/host.domain.com"
```

(replace host.domain.com with fully qualified DNS name for the server host)

-	Export the newly created user to the keytab file

```
kadmin -q "ktadd -k /path/to/mariadb.keytab mariadb/host.domain.com"
```

More details can be found [here](http://www.microhowto.info/howto/create_a_service_principal_using_mit_kerberos.html)
and [here](http://www.microhowto.info/howto/add_a_host_or_service_principal_to_a_keytab_using_mit_kerberos.html)

If you are using **Windows Active Directory KDC**
you can need to create keytab using ktpass.exe tool on Windows,  map principal user to an existing domain user like this

```
ktpass.exe /princ mariadb/host.domain.com@DOMAIN.COM /mapuser someuser /pass MyPas$w0rd /out mariadb.keytab /crypto all /ptype KRB5_NT_PRINCIPAL /mapop set
```

and then transfer the keytab file to the Unix server. See [Microsoft documentation](https://technet.microsoft.com/en-us/library/cc753771.aspx) for details.


## Server side preparations on Windows.
Usually nothing need to be done.  MariaDB server should to run on a domain joined machine, either as NetworkService account
(which is default if it runs as service) or run under any other domain account credentials.
Creating service principal is not required here (but you can still do it using [_setspn_](https://technet.microsoft.com/en-us/library/cc731241.aspx) tool)


# Installing plugin
-	Start the server

-	On Unix, edit my the my.cnf/my.ini configuration file, set the parameter gssapi-keytab-path to point to previously
created keytab path.

```
	gssapi-keytab-path=/path/to/mariadb.keytab
```

-	Optionally on Unix, in case the service principal name differs from default mariadb/host.domain.com@REALM,
configure alternative principal name with

```
    gssapi-principal-name=alternative/principalname@REALM
```

-	In mysql command line client, execute

```
	INSTALL SONAME 'auth_gssapi'
```

#Creating users

Now, you can create a user for GSSAPI/SSPI authentication. CREATE USER command, for Kerberos user
would be like this (*long* form, see below for short one)

```
CREATE USER usr1 IDENTIFIED WITH gssapi AS 'usr1@EXAMPLE.COM';
```

(replace  with real username and realm)

The part after AS is mechanism specific, and needs to be ``machine\\usr1`` for Windows users identified with NTLM.

You may also use alternative *short* form of CREATE USER

```
CREATE USER usr1 IDENTIFIED WITH gssapi;
```

If this syntax is used, realm part is *not* used for comparison
thus 'usr1@EXAMPLE.COM', 'usr1@EXAMPLE.CO.UK' and 'mymachine\usr1' will all identify as 'usr1'.

#Login as GSSAPI user with command line clients

Using command line client, do

```
mysql --plugin-dir=/path/to/plugin-dir -u usr1
```

#Plugin variables
-	**gssapi-keytab-path** (Unix only) - Path to the server keytab file
-	**gssapi-principal-name** - name of the service principal.
-	**gssapi-mech-name** (Windows only) - Name of the SSPI package used by server. Can be either 'Kerberos' or 'Negotiate'.
 Defaults to 'Negotiate' (both Kerberos and NTLM users can connect)
 Set it to 'Kerberos', to prevent less secure NTLM in domain environments,  but leave it as default(Negotiate)
 to allow non-domain environment (e.g if server does not run in domain environment).


#Implementation

Overview of the protocol between client and server

1. Server : Construct gssapi-principal-name if not set in my.cnf. On Unixes defaults to hostbased name for service "mariadb". On Windows to user's or machine's domain names.
Acquire credentials for gssapi-principal-name with ```gss_acquire_cred() / AcquireSecurityCredentials()```.
Send packet with principal name and mech ```"gssapi-principal-name\0gssapi-mech-name\0"``` to client ( on Unix, empty string used for gssapi-mech)

2. Client: execute ```gss_init_sec_context() / InitializeSecurityContext()``` passing gssapi-principal-name / gssapi-mech-name parameters.
Send resulting GSSAPI blob to server.

3. Server : receive blob from client, execute ```gss_accept_sec_context()/ AcceptSecurityContext()```, send resulting blob back to client

4. Perform  2. and 3. can until both client and server decide that authentication is done, or until some error occurred. If authentication was successful, GSSAPI context (an opaque structure) is generated on both client and server sides.

5. Server : Client name is extracted from the context, and compared to the name provided by client(with or without realm). If name matches, plugin returns success.
