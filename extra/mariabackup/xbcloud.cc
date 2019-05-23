/******************************************************
Copyright (c) 2014 Percona LLC and/or its affiliates.

The xbstream utility: serialize/deserialize files in the XBSTREAM format.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

#include <my_global.h>
#include <my_default.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <ev.h>
#include <unistd.h>
#include <errno.h>
#include <gcrypt.h>
#include <assert.h>
#include <my_sys.h>
#include <my_dir.h>
#include <my_getopt.h>
#include <algorithm>
#include <map>
#include <string>
#include <jsmn.h>
#include "xbstream.h"

using std::min;
using std::max;
using std::map;
using std::string;

#define XBCLOUD_VERSION "1.0"

#define SWIFT_MAX_URL_SIZE 8192
#define SWIFT_MAX_HDR_SIZE 8192

#define SWIFT_CHUNK_SIZE 11 * 1024 * 1024

#if ((LIBCURL_VERSION_MAJOR >= 7) && (LIBCURL_VERSION_MINOR >= 16))
#define OLD_CURL_MULTI 0
#else
#define OLD_CURL_MULTI 1
#endif

/*****************************************************************************/

typedef struct swift_auth_info_struct swift_auth_info;
typedef struct connection_info_struct connection_info;
typedef struct socket_info_struct socket_info;
typedef struct global_io_info_struct global_io_info;
typedef struct slo_chunk_struct slo_chunk;
typedef struct container_list_struct container_list;
typedef struct object_info_struct object_info;

struct swift_auth_info_struct {
	char url[SWIFT_MAX_URL_SIZE];
	char token[SWIFT_MAX_HDR_SIZE];
};

struct global_io_info_struct {
	struct ev_loop *loop;
	struct ev_io input_event;
	struct ev_timer timer_event;
	CURLM *multi;
	int still_running;
	int eof;
	curl_socket_t input_fd;
	connection_info **connections;
	long chunk_no;
	connection_info *current_connection;
	const char *url;
	const char *container;
	const char *token;
	const char *backup_name;
};

struct socket_info_struct {
	curl_socket_t sockfd;
	CURL *easy;
	int action;
	long timeout;
	struct ev_io ev;
	int evset;
	global_io_info *global;
};

struct connection_info_struct {
	CURL *easy;
	global_io_info *global;
	char *buffer;
	size_t buffer_size;
	size_t filled_size;
	size_t upload_size;
	bool chunk_uploaded;
	bool chunk_acked;
	char error[CURL_ERROR_SIZE];
	struct curl_slist *slist;
	char *name;
	size_t name_len;
	char hash[33];
	size_t chunk_no;
	bool magic_verified;
	size_t chunk_path_len;
	xb_chunk_type_t chunk_type;
	size_t payload_size;
	size_t chunk_size;
	int retry_count;
	bool upload_started;
	ulong global_idx;
};

struct slo_chunk_struct {
	char name[SWIFT_MAX_URL_SIZE];
	char md5[33];
	int idx;
	size_t size;
};

struct object_info_struct {
	char hash[33];
	char name[SWIFT_MAX_URL_SIZE];
	size_t bytes;
};

struct container_list_struct {
	size_t content_length;
	size_t content_bufsize;
	char *content_json;
	size_t object_count;
	size_t idx;
	object_info *objects;
	bool final;
};

enum {SWIFT, S3};
const char *storage_names[] =
{ "SWIFT", "S3", NullS};

static my_bool opt_verbose = 0;
static ulong opt_storage = SWIFT;
static const char *opt_swift_user = NULL; 
static const char *opt_swift_user_id = NULL; 
static const char *opt_swift_password = NULL; 
static const char *opt_swift_tenant = NULL; 
static const char *opt_swift_tenant_id = NULL; 
static const char *opt_swift_project = NULL; 
static const char *opt_swift_project_id = NULL; 
static const char *opt_swift_domain = NULL; 
static const char *opt_swift_domain_id = NULL; 
static const char *opt_swift_region = NULL; 
static const char *opt_swift_container = NULL;
static const char *opt_swift_storage_url = NULL;
static const char *opt_swift_auth_url = NULL;
static const char *opt_swift_key = NULL;
static const char *opt_swift_auth_version = NULL;
static const char *opt_name = NULL;
static const char *opt_cacert = NULL;
static ulong opt_parallel = 1;
static my_bool opt_insecure = 0;
static enum {MODE_GET, MODE_PUT, MODE_DELETE} opt_mode;

static char **file_list = NULL;
static int file_list_size = 0;

TYPELIB storage_typelib =
{array_elements(storage_names)-1, "", storage_names, NULL};

enum {
	OPT_STORAGE = 256,
	OPT_SWIFT_CONTAINER,
	OPT_SWIFT_AUTH_URL,
	OPT_SWIFT_KEY,
	OPT_SWIFT_USER,
	OPT_SWIFT_USER_ID,
	OPT_SWIFT_PASSWORD,
	OPT_SWIFT_TENANT,
	OPT_SWIFT_TENANT_ID,
	OPT_SWIFT_PROJECT,
	OPT_SWIFT_PROJECT_ID,
	OPT_SWIFT_DOMAIN,
	OPT_SWIFT_DOMAIN_ID,
	OPT_SWIFT_REGION,
	OPT_SWIFT_STORAGE_URL,
	OPT_SWIFT_AUTH_VERSION,
	OPT_PARALLEL,
	OPT_CACERT,
	OPT_INSECURE,
	OPT_VERBOSE
};


static struct my_option my_long_options[] =
{
	{"help", '?', "Display this help and exit.",
	 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},

	{"storage", OPT_STORAGE, "Specify storage type S3/SWIFT.",
	 &opt_storage, &opt_storage, &storage_typelib,
	 GET_ENUM, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

	{"swift-auth-version", OPT_SWIFT_AUTH_VERSION,
	 "Swift authentication verison to use.",
	 &opt_swift_auth_version, &opt_swift_auth_version, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-container", OPT_SWIFT_CONTAINER,
	 "Swift container to store backups into.",
	 &opt_swift_container, &opt_swift_container, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-user", OPT_SWIFT_USER,
	 "Swift user name.",
	 &opt_swift_user, &opt_swift_user, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-user-id", OPT_SWIFT_USER_ID,
	 "Swift user ID.",
	 &opt_swift_user_id, &opt_swift_user_id, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-auth-url", OPT_SWIFT_AUTH_URL,
	 "Base URL of SWIFT authentication service.",
	 &opt_swift_auth_url, &opt_swift_auth_url, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-storage-url", OPT_SWIFT_STORAGE_URL,
	 "URL of object-store endpoint. Usually received from authentication "
	 "service. Specify to override this value.",
	 &opt_swift_storage_url, &opt_swift_storage_url, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-key", OPT_SWIFT_KEY,
	 "Swift key.",
	 &opt_swift_key, &opt_swift_key, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-tenant", OPT_SWIFT_TENANT,
	 "The tenant name. Both the --swift-tenant and --swift-tenant-id "
	 "options are optional, but should not be specified together.",
	 &opt_swift_tenant, &opt_swift_tenant, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-tenant-id", OPT_SWIFT_TENANT_ID,
	 "The tenant ID. Both the --swift-tenant and --swift-tenant-id "
	 "options are optional, but should not be specified together.",
	 &opt_swift_tenant_id, &opt_swift_tenant_id, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-project", OPT_SWIFT_PROJECT,
	 "The project name.",
	 &opt_swift_project, &opt_swift_project, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-project-id", OPT_SWIFT_PROJECT_ID,
	 "The project ID.",
	 &opt_swift_project_id, &opt_swift_project_id, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-domain", OPT_SWIFT_DOMAIN,
	 "The domain name.",
	 &opt_swift_domain, &opt_swift_domain, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-domain-id", OPT_SWIFT_DOMAIN_ID,
	 "The domain ID.",
	 &opt_swift_domain_id, &opt_swift_domain_id, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-password", OPT_SWIFT_PASSWORD,
	 "The password of the user.",
	 &opt_swift_password, &opt_swift_password, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"swift-region", OPT_SWIFT_REGION,
	 "The region object-store endpoint.",
	 &opt_swift_region, &opt_swift_region, 0,
	 GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"parallel", OPT_PARALLEL,
	 "Number of parallel chunk uploads.",
	 &opt_parallel, &opt_parallel, 0, GET_ULONG, REQUIRED_ARG,
	 1, 0, 0, 0, 0, 0},

	{"cacert", OPT_CACERT,
	 "CA certificate file.",
	 &opt_cacert, &opt_cacert, 0, GET_STR_ALLOC, REQUIRED_ARG,
	 0, 0, 0, 0, 0, 0},

	{"insecure", OPT_INSECURE,
	 "Do not verify server SSL certificate.",
	 &opt_insecure, &opt_insecure, 0, GET_BOOL, NO_ARG,
	 0, 0, 0, 0, 0, 0},

	{"verbose", OPT_VERBOSE,
	 "Turn ON cURL tracing.",
	 &opt_verbose, &opt_verbose, 0, GET_BOOL, NO_ARG,
	 0, 0, 0, 0, 0, 0},

	{0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

/* The values of these arguments should be masked
   on the command line */
static const char * const masked_args[] = {
        "--swift-password",
        "--swift-key",
        "--swift-auth-url",
        "--swift-storage-url",
        "--swift-container",
        "--swift-user",
        "--swift-tenant",
        "--swift-user-id",
        "--swift-tenant-id",
        0
};

static map<string, ulonglong> file_chunk_count;

static
void
print_version()
{
	printf("%s  Ver %s for %s (%s)\n", my_progname, XBCLOUD_VERSION,
	       SYSTEM_TYPE, MACHINE_TYPE);
}

static
void
usage()
{
	print_version();
	puts("Copyright (C) 2015 Percona LLC and/or its affiliates.");
	puts("This software comes with ABSOLUTELY NO WARRANTY. "
	     "This is free software,\nand you are welcome to modify and "
	     "redistribute it under the GPL license.\n");

	puts("Manage backups on Cloud services.\n");

	puts("Usage: ");
	printf("  %s -c put [OPTIONS...] <NAME> upload backup from STDIN into "
	       "the cloud service with given name.\n", my_progname);
	printf("  %s -c get [OPTIONS...] <NAME> [FILES...] stream specified "
	       "backup or individual files from cloud service into STDOUT.\n",
	       my_progname);

	puts("\nOptions:");
	my_print_help(my_long_options);
}

static
my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument __attribute__((unused)))
{
	switch (optid) {
	case '?':
		usage();
		exit(0);
	}

	return(FALSE);
}

static const char *load_default_groups[]=
	{ "xbcloud", 0 };

/*********************************************************************//**
mask sensitive values on the command line */
static
void
mask_args(int argc, char **argv)
{
        int i;
        for (i = 0; i < argc-1; i++) {
                int j = 0;
                if (argv[i]) while (masked_args[j]) {
                        char *p;
                        if ((p = strstr(argv[i], masked_args[j]))) {
                                p += strlen(masked_args[j]);
                                while (*p && *p != '=') {
                                        p++;
                                }
                                if (*p == '=') {
                                        p++;
                                        while (*p) {
                                                *p++ = 'x';
                                        }
                                }
                        }
                        j++;
                }
        }
}

static
int parse_args(int argc, char **argv)
{
	const char *command;

	if (argc < 2) {
		fprintf(stderr, "Command isn't specified. "
			"Supported commands are put and get\n");
		usage();
		exit(EXIT_FAILURE);
	}

	command = argv[1];
	argc--; argv++;

	if (strcasecmp(command, "put") == 0) {
		opt_mode = MODE_PUT;
	} else if (strcasecmp(command, "get") == 0) {
		opt_mode = MODE_GET;
	} else if (strcasecmp(command, "delete") == 0) {
		opt_mode = MODE_DELETE;
	} else {
		fprintf(stderr, "Unknown command %s. "
			"Supported commands are put and get\n", command);
		usage();
		exit(EXIT_FAILURE);
	}

	load_defaults_or_exit("my", load_default_groups, &argc, &argv);

	if (handle_options(&argc, &argv, my_long_options, get_one_option)) {
		exit(EXIT_FAILURE);
	}

	/* make sure name is specified */
	if (argc < 1) {
		fprintf(stderr, "Backup name is required argument\n");
		exit(EXIT_FAILURE);
	}
	opt_name = argv[0];
	argc--; argv++;

	/* validate arguments */
	if (opt_storage == SWIFT) {
		if (opt_swift_user == NULL) {
			fprintf(stderr, "Swift user is not specified\n");
			exit(EXIT_FAILURE);
		}
		if (opt_swift_container == NULL) {
			fprintf(stderr,
				"Swift container is not specified\n");
			exit(EXIT_FAILURE);
		}
		if (opt_swift_auth_url == NULL) {
			fprintf(stderr, "Swift auth URL is not specified\n");
			exit(EXIT_FAILURE);
		}
	} else {
		fprintf(stderr, "Swift is only supported storage API\n");
	}

	if (argc > 0) {
		file_list = argv;
		file_list_size = argc;
	}

	return(0);
}

static char *hex_md5(const unsigned char *hash, char *out)
{
	enum { hash_len = 16 };
	char *p;
	int i;

	for (i = 0, p = out; i < hash_len; i++, p+=2) {
		sprintf(p, "%02x", hash[i]);
	}

	return out;
}

/* If header starts with prefix it's value will be copied into output buffer */
static
int get_http_header(const char *prefix, const char *buffer,
		    char *out, size_t out_size)
{
	const char *beg, *end;
	size_t len, prefix_len;

	prefix_len = strlen(prefix);

	if (strncasecmp(buffer, prefix, prefix_len) == 0) {
		beg = buffer + prefix_len;
		end = strchr(beg, '\r');

		len = min<size_t>(end - beg, out_size - 1);

		strncpy(out, beg, len);

		out[len] = 0;

		return 1;
	}

	return 0;
}

static
size_t swift_auth_header_read_cb(char *ptr, size_t size, size_t nmemb,
				 void *data)
{
	swift_auth_info *info = (swift_auth_info*)(data);

	get_http_header("X-Storage-Url: ", ptr,
			info->url, array_elements(info->url));
	get_http_header("X-Auth-Token: ", ptr,
			info->token, array_elements(info->token));

	return nmemb * size;
}

/*********************************************************************//**
Authenticate against Swift TempAuth. Fills swift_auth_info struct.
Uses creadentials privided as global variables.
@returns true if access is granted and token received. */
static
bool
swift_temp_auth(const char *auth_url, swift_auth_info *info)
{
	CURL *curl;
	CURLcode res;
	long http_code;
	char *hdr_buf = NULL;
	struct curl_slist *slist = NULL;

	if (opt_swift_user == NULL) {
		fprintf(stderr, "Swift user must be specified for TempAuth.\n");
		return(false);
	}

	if (opt_swift_key == NULL) {
		fprintf(stderr, "Swift key must be specified for TempAuth.\n");
		return(false);
	}

	curl = curl_easy_init();

	if (curl != NULL) {

		hdr_buf = (char *)(calloc(14 + max(strlen(opt_swift_user),
						   strlen(opt_swift_key)), 1));

		if (!hdr_buf) {
			res = CURLE_FAILED_INIT;
			goto cleanup;
		}

		sprintf(hdr_buf, "X-Auth-User: %s", opt_swift_user);
		slist = curl_slist_append(slist, hdr_buf);

		sprintf(hdr_buf, "X-Auth-Key: %s", opt_swift_key);
		slist = curl_slist_append(slist, hdr_buf);

		curl_easy_setopt(curl, CURLOPT_VERBOSE, opt_verbose);
		curl_easy_setopt(curl, CURLOPT_URL, auth_url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
				 swift_auth_header_read_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, info);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
		if (opt_cacert != NULL)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opt_cacert);
		if (opt_insecure)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			fprintf(stderr, "error: authentication failed: "
				"curl_easy_perform(): %s\n",
				curl_easy_strerror(res));
			goto cleanup;
		}
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 200 &&
		    http_code != 204) {
			fprintf(stderr, "error: authentication failed "
				"with response code: %ld\n", http_code);
			res = CURLE_LOGIN_DENIED;
			goto cleanup;
		}
	} else {
		res = CURLE_FAILED_INIT;
		fprintf(stderr, "error: curl_easy_init() failed\n");
		goto cleanup;
	}

cleanup:
	if (hdr_buf) {
		free(hdr_buf);
	}
	if (slist) {
		curl_slist_free_all(slist);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}

	if (res == CURLE_OK) {
		/* check that we received token and storage URL */
		if (*info->url == 0) {
			fprintf(stderr, "error: malformed response: "
					"X-Storage-Url is missing\n");
			return(false);
		}
		if (*info->token == 0) {
			fprintf(stderr, "error: malformed response: "
					"X-Auth-Token is missing\n");
			return(false);
		}
		return(true);
	}

	return(false);
}

static
size_t
write_null_cb(char *buffer, size_t size, size_t nmemb, void *stream)
{
	return fwrite(buffer, size, nmemb, stderr);
}


static
size_t
read_null_cb(char *ptr, size_t size, size_t nmemb, void *data)
{
	return 0;
}


static
int
swift_create_container(swift_auth_info *info, const char *name)
{
	char url[SWIFT_MAX_URL_SIZE];
	char auth_token[SWIFT_MAX_HDR_SIZE];
	CURLcode res;
	long http_code;
	CURL *curl;
	struct curl_slist *slist = NULL;

	snprintf(url, array_elements(url), "%s/%s", info->url, name);
	snprintf(auth_token, array_elements(auth_token), "X-Auth-Token: %s",
		 info->token);

	curl = curl_easy_init();

	if (curl != NULL) {
		slist = curl_slist_append(slist, auth_token);
		slist = curl_slist_append(slist, "Content-Length: 0");

		curl_easy_setopt(curl, CURLOPT_VERBOSE, opt_verbose);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_null_cb);
		curl_easy_setopt(curl, CURLOPT_READFUNCTION, read_null_cb);
		curl_easy_setopt(curl, CURLOPT_INFILESIZE, 0L);
		curl_easy_setopt(curl, CURLOPT_PUT, 1L);
		if (opt_cacert != NULL)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opt_cacert);
		if (opt_insecure)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			fprintf(stderr,
				"error: curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
			goto cleanup;
		}
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 201 && /* created */
		    http_code != 202    /* accepted (already exists) */) {
			fprintf(stderr, "error: request failed "
				"with response code: %ld\n", http_code);
			res = CURLE_LOGIN_DENIED;
			goto cleanup;
		}
	} else {
		res = CURLE_FAILED_INIT;
		fprintf(stderr, "error: curl_easy_init() failed\n");
		goto cleanup;
	}

cleanup:
	if (slist) {
		curl_slist_free_all(slist);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}

	return res;
}


/*********************************************************************//**
Delete object with given url.
@returns true if object deleted successfully. */
static
bool
swift_delete_object(swift_auth_info *info, const char *url)
{
	char auth_token[SWIFT_MAX_HDR_SIZE];
	CURLcode res;
	long http_code;
	CURL *curl;
	struct curl_slist *slist = NULL;
	bool ret = false;

	snprintf(auth_token, array_elements(auth_token), "X-Auth-Token: %s",
		 info->token);

	curl = curl_easy_init();

	if (curl != NULL) {
		slist = curl_slist_append(slist, auth_token);

		curl_easy_setopt(curl, CURLOPT_VERBOSE, opt_verbose);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE"); 
		if (opt_cacert != NULL)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opt_cacert);
		if (opt_insecure)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			fprintf(stderr,
				"error: curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
			goto cleanup;
		}
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code != 200 && /* OK */
		    http_code != 204    /* no content */) {
			fprintf(stderr, "error: request failed "
				"with response code: %ld\n", http_code);
			goto cleanup;
		}
		ret = true;
	} else {
		fprintf(stderr, "error: curl_easy_init() failed\n");
		goto cleanup;
	}

cleanup:
	if (slist) {
		curl_slist_free_all(slist);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}

	return ret;
}

static int conn_upload_init(connection_info *conn);
static void conn_buffer_updated(connection_info *conn);
static connection_info *conn_new(global_io_info *global, ulong global_idx);
static void conn_cleanup(connection_info *conn);
static void conn_upload_retry(connection_info *conn);

/* Check for completed transfers, and remove their easy handles */
static void check_multi_info(global_io_info *g)
{
	char *eff_url;
	CURLMsg *msg;
	int msgs_left;
	connection_info *conn;
	CURL *easy;

	while ((msg = curl_multi_info_read(g->multi, &msgs_left))) {
		if (msg->msg == CURLMSG_DONE) {
			easy = msg->easy_handle;
			curl_easy_getinfo(easy, CURLINFO_PRIVATE, &conn);
			curl_easy_getinfo(easy, CURLINFO_EFFECTIVE_URL,
					  &eff_url);
			curl_multi_remove_handle(g->multi, easy);
			curl_easy_cleanup(easy);
			conn->easy = NULL;
			if (conn->chunk_acked) {
				conn->chunk_uploaded = true;
				fprintf(stderr, "%s is done\n", conn->hash);
			} else {
				fprintf(stderr, "error: chunk %zu '%s' %s "
					"is not uploaded, but socket closed "
					"(%zu bytes of %zu left to upload)\n",
					conn->chunk_no,
					conn->name,
					conn->hash,
					conn->chunk_size - conn->upload_size,
					conn->chunk_size);
				conn_upload_retry(conn);
			}
		}
	}
}

/* Die if we get a bad CURLMcode somewhere */ 
static void mcode_or_die(const char *where, CURLMcode code)
{
	if (code != CURLM_OK)
	{
		const char *s;
		switch (code)
		{
		case CURLM_BAD_HANDLE:
			s = "CURLM_BAD_HANDLE";
			break;
		case CURLM_BAD_EASY_HANDLE:
			s = "CURLM_BAD_EASY_HANDLE";
			break;
		case CURLM_OUT_OF_MEMORY:
			s = "CURLM_OUT_OF_MEMORY";
			break;
		case CURLM_INTERNAL_ERROR:
			s = "CURLM_INTERNAL_ERROR";
			break;
		case CURLM_UNKNOWN_OPTION:
			s = "CURLM_UNKNOWN_OPTION";
			break;
		case CURLM_LAST:
			s = "CURLM_LAST";
			break;
		default:
			s = "CURLM_unknown";
			break;
		case CURLM_BAD_SOCKET:
			s = "CURLM_BAD_SOCKET";
			fprintf(stderr, "error: %s returns (%d) %s\n",
				where, code, s);
			/* ignore this error */
			return;
		}
		fprintf(stderr, "error: %s returns (%d) %s\n",
			where, code, s);
		assert(0);
	}
}

/* Called by libev when we get action on a multi socket */ 
static void event_cb(EV_P_ struct ev_io *w, int revents)
{
	global_io_info *global = (global_io_info*)(w->data);
	CURLMcode rc;

#if !(OLD_CURL_MULTI)
	int action = (revents & EV_READ  ? CURL_POLL_IN  : 0) |
		(revents & EV_WRITE ? CURL_POLL_OUT : 0);

	do {
		rc = curl_multi_socket_action(global->multi, w->fd, action,
					      &global->still_running);
	} while (rc == CURLM_CALL_MULTI_PERFORM);
#else
	do {
		rc = curl_multi_socket(global->multi, w->fd,
				       &global->still_running);
	} while (rc == CURLM_CALL_MULTI_PERFORM);
#endif
	mcode_or_die("error: event_cb: curl_multi_socket_action", rc);
	check_multi_info(global);
	if (global->still_running <= 0) {
		ev_timer_stop(global->loop, &global->timer_event);
	}
}

static void remsock(curl_socket_t s, socket_info *fdp, global_io_info *global)
{
	if (fdp) {
		if (fdp->evset) {
			ev_io_stop(global->loop, &fdp->ev);
		}
		free(fdp);
	}
}

static void setsock(socket_info *fdp, curl_socket_t s, CURL *easy, int action,
		    global_io_info *global)
{
	int kind = (action & CURL_POLL_IN  ? (int)(EV_READ)  : 0) |
		(action & CURL_POLL_OUT ? (int)(EV_WRITE) : 0);

	fdp->sockfd = s;
	fdp->action = action;
	fdp->easy = easy;
	if (fdp->evset)
		ev_io_stop(global->loop, &fdp->ev);
	ev_io_init(&fdp->ev, event_cb, fdp->sockfd, kind);
	fdp->ev.data = global;
	fdp->evset = 1;
	ev_io_start(global->loop, &fdp->ev);
}

static void addsock(curl_socket_t s, CURL *easy, int action,
		    global_io_info *global)
{
	socket_info *fdp = (socket_info *)(calloc(sizeof(socket_info), 1));

	fdp->global = global;
	setsock(fdp, s, easy, action, global);
	curl_multi_assign(global->multi, s, fdp);
}

static int sock_cb(CURL *easy, curl_socket_t s, int what, void *cbp,
		   void *sockp)
{
	global_io_info *global = (global_io_info*)(cbp);
	socket_info *fdp = (socket_info*)(sockp);

	if (what == CURL_POLL_REMOVE) {
		remsock(s, fdp, global);
	} else {
		if (!fdp) {
			addsock(s, easy, what, global);
		} else {
			setsock(fdp, s, easy, what, global);
		}
	}
	return 0;
}

/* Called by libev when our timeout expires */
static void timer_cb(EV_P_ struct ev_timer *w, int revents)
{
	global_io_info *io_global = (global_io_info*)(w->data);
	CURLMcode rc;

#if !(OLD_CURL_MULTI)
	do {
		rc = curl_multi_socket_action(io_global->multi,
					      CURL_SOCKET_TIMEOUT, 0,
					      &io_global->still_running);
	} while (rc == CURLM_CALL_MULTI_PERFORM);
#else
	do {
		rc = curl_multi_socket_all(io_global->multi,
					   &io_global->still_running);
	} while (rc == CURLM_CALL_MULTI_PERFORM);
#endif
	mcode_or_die("timer_cb: curl_multi_socket_action", rc);
	check_multi_info(io_global);
}

static connection_info *get_current_connection(global_io_info *global)
{
	connection_info *conn = global->current_connection;
	ulong i;

	if (conn && conn->filled_size < conn->chunk_size)
		return conn;

	for (i = 0; i < opt_parallel; i++) {
		conn = global->connections[i];
		if (conn->chunk_uploaded || conn->filled_size == 0) {
			global->current_connection = conn;
			conn_upload_init(conn);
			return conn;
		}
	}

	return NULL;
}

/* This gets called whenever data is received from the input */ 
static void input_cb(EV_P_ struct ev_io *w, int revents)
{
	global_io_info *io_global = (global_io_info *)(w->data);
	connection_info *conn = get_current_connection(io_global);

	if (conn == NULL)
		return;

	if (conn->filled_size < conn->chunk_size) {
		if (revents & EV_READ) {
			ssize_t nbytes = read(io_global->input_fd,
					      conn->buffer + conn->filled_size,
					      conn->chunk_size -
					      conn->filled_size);
			if (nbytes > 0) {
				conn->filled_size += nbytes;
				conn_buffer_updated(conn);
			} else if (nbytes < 0) {
				if (errno != EAGAIN && errno != EINTR) {
					char error[200];
					my_strerror(error, sizeof(error),
						    errno);
					fprintf(stderr, "error: failed to read "
						"input stream (%s)\n", error);
					/* failed to read input */
					exit(1);
				}
			} else {
				io_global->eof = 1;
				ev_io_stop(io_global->loop, w);
			}
		}
	}

	assert(conn->filled_size <= conn->chunk_size);
}

static int swift_upload_read_cb(char *ptr, size_t size, size_t nmemb,
				void *data)
{
	size_t realsize;

	connection_info *conn = (connection_info*)(data);

	if (conn->filled_size == conn->upload_size &&
	    conn->upload_size < conn->chunk_size && !conn->global->eof) {
		ssize_t nbytes;
		assert(conn->global->current_connection == conn);
		do {
			nbytes = read(conn->global->input_fd,
				      conn->buffer + conn->filled_size,
				      conn->chunk_size - conn->filled_size);
		} while (nbytes == -1 && errno == EAGAIN);
		if (nbytes > 0) {
			conn->filled_size += nbytes;
			conn_buffer_updated(conn);
		} else {
			conn->global->eof = 1;
		}
	}

	realsize = min(size * nmemb, conn->filled_size - conn->upload_size);

	memcpy(ptr, conn->buffer + conn->upload_size, realsize);
	conn->upload_size += realsize;

	assert(conn->filled_size <= conn->chunk_size);
	assert(conn->upload_size <= conn->filled_size);

	return realsize;
}

static
size_t upload_header_read_cb(char *ptr, size_t size, size_t nmemb,
			     void *data)
{
	connection_info *conn = (connection_info *)(data);
	char etag[33];

	if (get_http_header("Etag: ", ptr, etag, array_elements(etag))) {
		if (strcmp(conn->hash, etag) != 0) {
			fprintf(stderr, "error: ETag mismatch\n");
			exit(EXIT_FAILURE);
		}
		fprintf(stderr, "acked chunk %s\n", etag);
		conn->chunk_acked = true;
	}

	return nmemb * size;
}

static int conn_upload_init(connection_info *conn)
{
	conn->filled_size = 0;
	conn->upload_size = 0;
	conn->chunk_uploaded = false;
	conn->chunk_acked = false;
	conn->chunk_size = CHUNK_HEADER_CONSTANT_LEN;
	conn->magic_verified = false;
	conn->chunk_path_len = 0;
	conn->chunk_type = XB_CHUNK_TYPE_UNKNOWN;
	conn->payload_size = 0;
	conn->upload_started = false;
	conn->retry_count = 0;
	if (conn->name != NULL) {
		conn->name[0] = 0;
	}

	if (conn->easy != NULL) {
		conn->easy = 0;
	}

	if (conn->slist != NULL) {
		curl_slist_free_all(conn->slist);
		conn->slist = NULL;
	}

	return 0;
}

static void conn_upload_prepare(connection_info *conn)
{
	gcry_md_hd_t md5;

	gcry_md_open(&md5, GCRY_MD_MD5, 0);
	gcry_md_write(md5, conn->buffer, conn->chunk_size);
	hex_md5(gcry_md_read(md5, GCRY_MD_MD5), conn->hash);
	gcry_md_close(md5);
}

static int conn_upload_start(connection_info *conn)
{
	char token_header[SWIFT_MAX_HDR_SIZE];
	char object_url[SWIFT_MAX_URL_SIZE];
	char content_len[200], etag[200];
	global_io_info *global;
	CURLMcode rc;

	global = conn->global;

	fprintf(stderr, "uploading chunk %s/%s/%s.%020zu "
			"(md5: %s, size: %zu)\n",
			global->container, global->backup_name, conn->name,
			conn->chunk_no, conn->hash, conn->chunk_size);

	snprintf(object_url, array_elements(object_url), "%s/%s/%s/%s.%020zu",
		 global->url, global->container, global->backup_name,
		 conn->name, conn->chunk_no);

	snprintf(content_len, sizeof(content_len), "Content-Length: %lu",
		(ulong)(conn->chunk_size));

	snprintf(etag, sizeof(etag), "ETag: %s", conn->hash);

	snprintf(token_header, array_elements(token_header),
		 "X-Auth-Token: %s", global->token);

	conn->slist = curl_slist_append(conn->slist, token_header);
	conn->slist = curl_slist_append(conn->slist,
					"Connection: keep-alive");
	conn->slist = curl_slist_append(conn->slist,
					"Content-Type: "
					"application/octet-stream");
	conn->slist = curl_slist_append(conn->slist, content_len);
	conn->slist = curl_slist_append(conn->slist, etag);

	conn->easy = curl_easy_init();
	if (!conn->easy) {
		fprintf(stderr, "error: curl_easy_init() failed\n");
		return 1;
	}
	curl_easy_setopt(conn->easy, CURLOPT_URL, object_url);
	curl_easy_setopt(conn->easy, CURLOPT_READFUNCTION,
				     swift_upload_read_cb);
	curl_easy_setopt(conn->easy, CURLOPT_READDATA, conn);
	curl_easy_setopt(conn->easy, CURLOPT_VERBOSE, opt_verbose);
	curl_easy_setopt(conn->easy, CURLOPT_ERRORBUFFER, conn->error);
	curl_easy_setopt(conn->easy, CURLOPT_PRIVATE, conn);
	curl_easy_setopt(conn->easy, CURLOPT_NOPROGRESS, 1L);
	curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_TIME, 5L);
	curl_easy_setopt(conn->easy, CURLOPT_LOW_SPEED_LIMIT, 1024L);
	curl_easy_setopt(conn->easy, CURLOPT_PUT, 1L);
	curl_easy_setopt(conn->easy, CURLOPT_HTTPHEADER, conn->slist);
	curl_easy_setopt(conn->easy, CURLOPT_HEADERFUNCTION,
				     upload_header_read_cb);
	curl_easy_setopt(conn->easy, CURLOPT_HEADERDATA, conn);
	curl_easy_setopt(conn->easy, CURLOPT_INFILESIZE,
				     (long) conn->chunk_size);
	if (opt_cacert != NULL)
		curl_easy_setopt(conn->easy, CURLOPT_CAINFO, opt_cacert);
	if (opt_insecure)
		curl_easy_setopt(conn->easy, CURLOPT_SSL_VERIFYPEER, FALSE);

	rc = curl_multi_add_handle(conn->global->multi, conn->easy);
	mcode_or_die("conn_upload_init: curl_multi_add_handle", rc);

#if (OLD_CURL_MULTI)
	do {
		rc = curl_multi_socket_all(global->multi,
					   &global->still_running);
	} while(rc == CURLM_CALL_MULTI_PERFORM);
#endif

	conn->upload_started = true;

	return 0;
}

static void conn_cleanup(connection_info *conn)
{
	if (conn) {
		free(conn->name);
		free(conn->buffer);
		if (conn->slist) {
			curl_slist_free_all(conn->slist);
			conn->slist = NULL;
		}
		if (conn->easy) {
			curl_easy_cleanup(conn->easy);
			conn->easy = NULL;
		}
	}
	free(conn);
}

static void conn_upload_retry(connection_info *conn)
{
	/* already closed by cURL */
	conn->easy = NULL;

	if (conn->slist != NULL) {
		curl_slist_free_all(conn->slist);
		conn->slist = NULL;
	}

	if (conn->retry_count++ > 3) {
		fprintf(stderr, "error: retry count limit reached\n");
		exit(EXIT_FAILURE);
	}

	fprintf(stderr, "warning: retrying to upload chunk %zu of '%s'\n",
		conn->chunk_no, conn->name);

	conn->upload_size = 0;

	conn_upload_start(conn);
}

static connection_info *conn_new(global_io_info *global, ulong global_idx)
{
	connection_info *conn;

	conn = (connection_info *)(calloc(1, sizeof(connection_info)));
	if (conn == NULL) {
		goto error;
	}

	conn->global = global;
	conn->global_idx = global_idx;
	conn->buffer_size = SWIFT_CHUNK_SIZE;
	if ((conn->buffer = (char *)(calloc(conn->buffer_size, 1))) ==
	    NULL) {
		goto error;
	}

	return conn;

error:
	if (conn != NULL) {
		conn_cleanup(conn);
	}

	fprintf(stderr, "error: out of memory\n");
	exit(EXIT_FAILURE);

	return NULL;
}

/*********************************************************************//**
Handle input buffer updates. Parse chunk header and set appropriate
buffer size. */
static
void
conn_buffer_updated(connection_info *conn)
{
	bool ready_for_upload = false;

	/* chunk header */
	if (!conn->magic_verified &&
	    conn->filled_size >= CHUNK_HEADER_CONSTANT_LEN) {
		if (strncmp(XB_STREAM_CHUNK_MAGIC, conn->buffer,
			sizeof(XB_STREAM_CHUNK_MAGIC) - 1) != 0) {

			fprintf(stderr, "Error: magic expected\n");
			exit(EXIT_FAILURE);
		}
		conn->magic_verified = true;
		conn->chunk_path_len = uint4korr(conn->buffer
						 + PATH_LENGTH_OFFSET);
		conn->chunk_type = (xb_chunk_type_t)
					(conn->buffer[CHUNK_TYPE_OFFSET]);
		conn->chunk_size = CHUNK_HEADER_CONSTANT_LEN +
					conn->chunk_path_len;
		if (conn->chunk_type != XB_CHUNK_TYPE_EOF) {
			conn->chunk_size += 16;
		}
	}

	/* ordinary chunk */
	if (conn->magic_verified &&
	    conn->payload_size == 0 &&
	    conn->chunk_type != XB_CHUNK_TYPE_EOF &&
	    conn->filled_size >= CHUNK_HEADER_CONSTANT_LEN
					+ conn->chunk_path_len + 16) {

		conn->payload_size = uint8korr(conn->buffer +
					CHUNK_HEADER_CONSTANT_LEN + 
					conn->chunk_path_len);

		conn->chunk_size = conn->payload_size + 4 + 16 +
					conn->chunk_path_len +
					CHUNK_HEADER_CONSTANT_LEN;

		if (conn->name == NULL) {
			conn->name = (char*)(malloc(conn->chunk_path_len + 1));
		} else if (conn->name_len < conn->chunk_path_len + 1) {
			conn->name = (char*)(realloc(conn->name,
						conn->chunk_path_len + 1));
		}
		conn->name_len = conn->chunk_path_len + 1;

		memcpy(conn->name, conn->buffer + CHUNK_HEADER_CONSTANT_LEN,
			conn->chunk_path_len);
		conn->name[conn->chunk_path_len] = 0;

		if (conn->buffer_size < conn->chunk_size) {
			conn->buffer =
			      (char *)(realloc(conn->buffer, conn->chunk_size));
			conn->buffer_size = conn->chunk_size;
		}
	}

	/* EOF chunk has no payload */
	if (conn->magic_verified &&
	    conn->chunk_type == XB_CHUNK_TYPE_EOF &&
	    conn->filled_size >= CHUNK_HEADER_CONSTANT_LEN
					+ conn->chunk_path_len) {

		if (conn->name == NULL) {
			conn->name = (char*)(malloc(conn->chunk_path_len + 1));
		} else if (conn->name_len < conn->chunk_path_len + 1) {
			conn->name = (char*)(realloc(conn->name,
						conn->chunk_path_len + 1));
		}
		conn->name_len = conn->chunk_path_len + 1;

		memcpy(conn->name, conn->buffer + CHUNK_HEADER_CONSTANT_LEN,
			conn->chunk_path_len);
		conn->name[conn->chunk_path_len] = 0;
	}

	if (conn->filled_size > 0 && conn->filled_size == conn->chunk_size) {
		ready_for_upload = true;
	}

	/* start upload once recieved the size of the chunk */
	if (!conn->upload_started && ready_for_upload) {
		conn->chunk_no = file_chunk_count[conn->name]++;
		conn_upload_prepare(conn);
		conn_upload_start(conn);
	}
}

static int init_input(global_io_info *io_global)
{
	ev_io_init(&io_global->input_event, input_cb, STDIN_FILENO, EV_READ);
	io_global->input_event.data = io_global;
	ev_io_start(io_global->loop, &io_global->input_event);

	return 0;
}

/* Update the event timer after curl_multi library calls */ 
static int multi_timer_cb(CURLM *multi, long timeout_ms, global_io_info *global)
{
	ev_timer_stop(global->loop, &global->timer_event);
	if (timeout_ms > 0) {
		double  t = timeout_ms / 1000.0;
		ev_timer_init(&global->timer_event, timer_cb, t, 0.);
		ev_timer_start(global->loop, &global->timer_event);
	} else {
		timer_cb(global->loop, &global->timer_event, 0);
	}
	return 0;
}

static
int swift_upload_parts(swift_auth_info *auth, const char *container,
		       const char *name)
{
	global_io_info io_global;
	ulong i;
#if (OLD_CURL_MULTI)
	long timeout;
#endif
	CURLMcode rc;
	int n_dirty_buffers;

	memset(&io_global, 0, sizeof(io_global));

	io_global.loop = ev_default_loop(0);
	init_input(&io_global);
	io_global.multi = curl_multi_init();
	ev_timer_init(&io_global.timer_event, timer_cb, 0., 0.);
	io_global.timer_event.data = &io_global;
	io_global.connections = (connection_info **)
		(calloc(opt_parallel, sizeof(connection_info)));
	io_global.url = auth->url;
	io_global.container = container;
	io_global.backup_name = name;
	io_global.token = auth->token;
	for (i = 0; i < opt_parallel; i++) {
		io_global.connections[i] = conn_new(&io_global, i);
	}

	/* setup the generic multi interface options we want */
	curl_multi_setopt(io_global.multi, CURLMOPT_SOCKETFUNCTION, sock_cb);
	curl_multi_setopt(io_global.multi, CURLMOPT_SOCKETDATA, &io_global);
#if !(OLD_CURL_MULTI)
	curl_multi_setopt(io_global.multi, CURLMOPT_TIMERFUNCTION, multi_timer_cb);
	curl_multi_setopt(io_global.multi, CURLMOPT_TIMERDATA, &io_global);
	do {
		rc = curl_multi_socket_action(io_global.multi,
					      CURL_SOCKET_TIMEOUT, 0,
					      &io_global.still_running);
	} while (rc == CURLM_CALL_MULTI_PERFORM);
#else
	curl_multi_timeout(io_global.multi, &timeout);
	if (timeout >= 0) {
		multi_timer_cb(io_global.multi, timeout, &io_global);
	}
	do {
		rc = curl_multi_socket_all(io_global.multi, &io_global.still_running);
	} while(rc == CURLM_CALL_MULTI_PERFORM);
#endif

	ev_loop(io_global.loop, 0);
	check_multi_info(&io_global);
	curl_multi_cleanup(io_global.multi);

	n_dirty_buffers = 0;
	for (i = 0; i < opt_parallel; i++) {
		connection_info *conn = io_global.connections[i];
		if (conn && conn->upload_size != conn->filled_size) {
			fprintf(stderr, "error: upload failed: %lu bytes left "
				"in the buffer %s (uploaded = %d)\n",
				(ulong)(conn->filled_size - conn->upload_size),
				conn->name, conn->chunk_uploaded);
			++n_dirty_buffers;
		}
	}

	for (i = 0; i < opt_parallel; i++) {
		if (io_global.connections[i] != NULL) {
			conn_cleanup(io_global.connections[i]);
		}
	}
	free(io_global.connections);

	if (n_dirty_buffers > 0) {
		return(EXIT_FAILURE);
	}

	return 0;
}

struct download_buffer_info {
	off_t offset;
	size_t size;
	size_t result_len;
	char *buf;
	curl_read_callback custom_header_callback;
	void *custom_header_callback_data;
};

/*********************************************************************//**
Callback to parse header of GET request on swift contaier. */
static
size_t fetch_buffer_header_cb(char *ptr, size_t size, size_t nmemb,
				      void *data)
{
	download_buffer_info *buffer_info = (download_buffer_info*)(data);
	size_t buf_size;
	char content_length_str[100];
	char *endptr;

	if (get_http_header("Content-Length: ", ptr,
			content_length_str, sizeof(content_length_str))) {

		buf_size = strtoull(content_length_str, &endptr, 10);

		if (buffer_info->buf == NULL) {
			buffer_info->buf = (char*)(malloc(buf_size));
			buffer_info->size = buf_size;
		}

		if (buf_size > buffer_info->size) {
			buffer_info->buf = (char*)
				(realloc(buffer_info->buf, buf_size));
			buffer_info->size = buf_size;
		}

		buffer_info->result_len = buf_size;
	}

	if (buffer_info->custom_header_callback) {
		buffer_info->custom_header_callback(ptr, size, nmemb,
				buffer_info->custom_header_callback_data);
	}

	return nmemb * size;
}

/*********************************************************************//**
Write contents into string buffer */
static
size_t
fetch_buffer_cb(char *buffer, size_t size, size_t nmemb, void *out_buffer)
{
	download_buffer_info *buffer_info = (download_buffer_info*)(out_buffer);

	assert(buffer_info->size >= buffer_info->offset + size * nmemb);

	memcpy(buffer_info->buf + buffer_info->offset, buffer, size * nmemb);
	buffer_info->offset += size * nmemb;

	return size * nmemb;
}


/*********************************************************************//**
Downloads contents of URL into buffer. Caller is responsible for
deallocating the buffer.
@return	pointer to a buffer or NULL */
static
char *
swift_fetch_into_buffer(swift_auth_info *auth, const char *url,
			char **buf, size_t *buf_size, size_t *result_len,
			curl_read_callback header_callback,
			void *header_callback_data)
{
	char auth_token[SWIFT_MAX_HDR_SIZE];
	download_buffer_info buffer_info;
	struct curl_slist *slist = NULL;
	long http_code;
	CURL *curl;
	CURLcode res;

	memset(&buffer_info, 0, sizeof(buffer_info));
	buffer_info.buf = *buf;
	buffer_info.size = *buf_size;
	buffer_info.custom_header_callback = header_callback;
	buffer_info.custom_header_callback_data = header_callback_data;

	snprintf(auth_token, array_elements(auth_token), "X-Auth-Token: %s",
		 auth->token);

	curl = curl_easy_init();

	if (curl != NULL) {
		slist = curl_slist_append(slist, auth_token);

		curl_easy_setopt(curl, CURLOPT_VERBOSE, opt_verbose);
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_buffer_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer_info);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
				 fetch_buffer_header_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA,
				 &buffer_info);
		if (opt_cacert != NULL)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opt_cacert);
		if (opt_insecure)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			fprintf(stderr,
				"error: curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
			goto cleanup;
		}
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code < 200 || http_code >= 300) {
			fprintf(stderr, "error: request failed "
				"with response code: %ld\n", http_code);
			res = CURLE_LOGIN_DENIED;
			goto cleanup;
		}
	} else {
		res = CURLE_FAILED_INIT;
		fprintf(stderr, "error: curl_easy_init() failed\n");
		goto cleanup;
	}

cleanup:
	if (slist) {
		curl_slist_free_all(slist);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}

	if (res == CURLE_OK) {
		*buf = buffer_info.buf;
		*buf_size = buffer_info.size;
		*result_len = buffer_info.result_len;
		return(buffer_info.buf);
	}

	free(buffer_info.buf);
	*buf = NULL;
	*buf_size = 0;
	*result_len = 0;

	return(NULL);
}

static
container_list *
container_list_new()
{
	container_list *list =
			(container_list *)(calloc(1, sizeof(container_list)));

	list->object_count = 1000;
	list->objects = (object_info*)
		(calloc(list->object_count, sizeof(object_info)));

	if (list->objects == NULL) {
		fprintf(stderr, "error: out of memory\n");
		free(list);
		return(NULL);
	}

	return(list);
}

static
void
container_list_free(container_list *list)
{
	free(list->content_json);
	free(list->objects);
	free(list);
}

static
void
container_list_add_object(container_list *list, const char *name,
			  const char *hash, size_t bytes)
{
	const size_t object_count_step = 1000;

	if (list->idx >= list->object_count) {
		list->objects = (object_info*)
			realloc(list->objects,
				(list->object_count + object_count_step) *
					sizeof(object_info));
		memset(list->objects + list->object_count, 0,
		       object_count_step * sizeof(object_info));
		list->object_count += object_count_step;
	}
	assert(list->idx <= list->object_count);
	strcpy(list->objects[list->idx].name, name);
	strcpy(list->objects[list->idx].hash, hash);
	list->objects[list->idx].bytes = bytes;
	++list->idx;
}


/*********************************************************************//**
Tokenize json string. Return array of tokens. Caller is responsoble for
deallocating the array. */
jsmntok_t *
json_tokenise(char *json, size_t len, int initial_tokens)
{
	jsmn_parser parser;
	jsmn_init(&parser);

	unsigned int n = initial_tokens;
	jsmntok_t *tokens = (jsmntok_t *)(malloc(sizeof(jsmntok_t) * n));

	int ret = jsmn_parse(&parser, json, len, tokens, n);

	while (ret == JSMN_ERROR_NOMEM)
	{
		n = n * 2 + 1;
		tokens = (jsmntok_t*)(realloc(tokens, sizeof(jsmntok_t) * n));
		ret = jsmn_parse(&parser, json, len, tokens, n);
	}

	if (ret == JSMN_ERROR_INVAL) {
		fprintf(stderr, "error: invalid JSON string\n");

	}
	if (ret == JSMN_ERROR_PART) {
		fprintf(stderr, "error: truncated JSON string\n");
	}

	return tokens;
}

/*********************************************************************//**
Return true if token representation equal to given string. */
static
bool
json_token_eq(const char *buf, jsmntok_t *t, const char *s)
{
	size_t len = strlen(s);

	assert(t->end > t->start);

	return((size_t)(t->end - t->start) == len &&
		(strncmp(buf + t->start, s, len) == 0));
}

/*********************************************************************//**
Copy given token as string. */
static
bool
json_token_str(const char *buf, jsmntok_t *t, char *out, int out_size)
{
	size_t len = min(t->end - t->start, out_size - 1);

	memcpy(out, buf + t->start, len);
	out[len] = 0;

	return(true);
}

/*********************************************************************//**
Parse SWIFT container list response and fill output array with values
sorted by object name. */
static
bool
swift_parse_container_list(container_list *list)
{
	enum {MAX_DEPTH=20};
	enum label_t {NONE, OBJECT};

	char name[SWIFT_MAX_URL_SIZE];
	char hash[33];
	char bytes[30];
	char *response = list->content_json;

	struct stack_t {
		jsmntok_t *t;
		int n_items;
		label_t label;
	};

	stack_t stack[MAX_DEPTH];
	jsmntok_t *tokens;
	int level;
	size_t count = 0;

	tokens = json_tokenise(list->content_json, list->content_length, 200);

	stack[0].t = &tokens[0];
	stack[0].label = NONE;
	stack[0].n_items = 1;
	level = 0;

	for (size_t i = 0, j = 1; j > 0; i++, j--) {
		jsmntok_t *t = &tokens[i];

		assert(t->start != -1 && t->end != -1);
		assert(level >= 0);

		--stack[level].n_items;

		switch (t->type) {
		case JSMN_ARRAY:
		case JSMN_OBJECT:
			if (level < MAX_DEPTH - 1) {
				level++;
			}
			stack[level].t = t;
			stack[level].label = NONE;
			if (t->type == JSMN_ARRAY) {
				stack[level].n_items = t->size;
				j += t->size;
			} else {
				stack[level].n_items = t->size * 2;
				j += t->size * 2;
			}
			break;
		case JSMN_PRIMITIVE:
		case JSMN_STRING:
			if (stack[level].t->type == JSMN_OBJECT &&
			    stack[level].n_items % 2 == 1) {
				/* key */
				if (json_token_eq(response, t, "name")) {
					json_token_str(response, &tokens[i + 1],
							name, sizeof(name));
				}
				if (json_token_eq(response, t, "hash")) {
					json_token_str(response, &tokens[i + 1],
							hash, sizeof(hash));
				}
				if (json_token_eq(response, t, "bytes")) {
					json_token_str(response, &tokens[i + 1],
							bytes, sizeof(bytes));
				}
			}
			break;
		}

		while (stack[level].n_items == 0 && level > 0) {
			if (stack[level].t->type == JSMN_OBJECT
			    && level == 2) {
				char *endptr;
				container_list_add_object(list, name, hash,
						strtoull(bytes, &endptr, 10));
				++count;
			}
			--level;
		}
	}

	if (count == 0) {
		list->final = true;
	}

	free(tokens);

	return(true);
}

/*********************************************************************//**
List swift container with given name. Return list of objects sorted by
object name. */
static
container_list *
swift_list(swift_auth_info *auth, const char *container, const char *path)
{
	container_list *list;
	char url[SWIFT_MAX_URL_SIZE];

	list = container_list_new();

	while (!list->final) {

		/* download the list in json format */
		snprintf(url, array_elements(url),
			 "%s/%s?format=json&limit=1000%s%s%s%s",
			 auth->url, container, path ? "&prefix=" : "",
			 path ? path : "", list->idx > 0 ? "&marker=" : "",
			 list->idx > 0 ?
			 	list->objects[list->idx - 1].name : "");

		list->content_json = swift_fetch_into_buffer(auth, url,
				&list->content_json, &list->content_bufsize,
				&list->content_length, NULL, NULL);

		if (list->content_json == NULL) {
			container_list_free(list);
			return(NULL);
		}

		/* parse downloaded list */
		if (!swift_parse_container_list(list)) {
			fprintf(stderr, "error: unable to parse "
					"container list\n");
			container_list_free(list);
			return(NULL);
		}
	}

	return(list);
}


/*********************************************************************//**
Return true if chunk is a part of backup with given name. */
static
bool
chunk_belongs_to(const char *chunk_name, const char *backup_name)
{
	size_t backup_name_len = strlen(backup_name);

	return((strlen(chunk_name) > backup_name_len)
		&& (chunk_name[backup_name_len] == '/')
		&& strncmp(chunk_name, backup_name, backup_name_len) == 0);
}

/*********************************************************************//**
Return true if chunk is in given list. */
static
bool
chunk_in_list(const char *chunk_name, char **list, int list_size)
{
	size_t chunk_name_len;

	if (list_size == 0) {
		return(true);
	}

	chunk_name_len = strlen(chunk_name);
	if (chunk_name_len < 20) {
		return(false);
	}

	for (int i = 0; i < list_size; i++) {
		size_t item_len = strlen(list[i]);

		if ((strncmp(chunk_name - item_len + chunk_name_len - 21,
			     list[i], item_len) == 0)
		    && (chunk_name[chunk_name_len - 21] == '.')
		    && (chunk_name[chunk_name_len - item_len - 22] == '/')) {
			return(true);
		}
	}

	return(false);
}

static
int swift_download(swift_auth_info *auth, const char *container,
		   const char *name)
{
	container_list *list;
	char *buf = NULL;
	size_t buf_size = 0;
	size_t result_len = 0;

	if ((list = swift_list(auth, container, name)) == NULL) {
		return(CURLE_FAILED_INIT);
	}

	for (size_t i = 0; i < list->idx; i++) {
		const char *chunk_name = list->objects[i].name;

		if (chunk_belongs_to(chunk_name, name)
		    && chunk_in_list(chunk_name, file_list, file_list_size)) {
			char url[SWIFT_MAX_URL_SIZE];

			snprintf(url, sizeof(url), "%s/%s/%s",
				 auth->url, container, chunk_name);

			if ((buf = swift_fetch_into_buffer(
					auth, url, &buf, &buf_size, &result_len,
					NULL, NULL)) == NULL) {
				fprintf(stderr, "error: failed to download "
						"chunk %s\n", chunk_name);
				container_list_free(list);
				return(CURLE_FAILED_INIT);
			}

			fwrite(buf, 1, result_len, stdout);
		}
	}

	free(buf);

	container_list_free(list);

	return(CURLE_OK);
}


/*********************************************************************//**
Delete backup with given name from given container.
@return	true if backup deleted successfully */
static
bool swift_delete(swift_auth_info *auth, const char *container,
		  const char *name)
{
	container_list *list;

	if ((list = swift_list(auth, container, name)) == NULL) {
		return(CURLE_FAILED_INIT);
	}

	for (size_t i = 0; i < list->object_count; i++) {
		const char *chunk_name = list->objects[i].name;

		if (chunk_belongs_to(chunk_name, name)) {
			char url[SWIFT_MAX_URL_SIZE];

			snprintf(url, sizeof(url), "%s/%s/%s",
				 auth->url, container, chunk_name);

			fprintf(stderr, "delete %s\n", chunk_name);
			if (!swift_delete_object(auth, url)) {
				fprintf(stderr, "error: failed to delete "
						"chunk %s\n", chunk_name);
				container_list_free(list);
				return(CURLE_FAILED_INIT);
			}
		}
	}

	container_list_free(list);

	return(CURLE_OK);
}

/*********************************************************************//**
Check if backup with given name exists.
@return	true if backup exists */
static
bool swift_backup_exists(swift_auth_info *auth, const char *container,
			 const char *backup_name)
{
	container_list *list;

	if ((list = swift_list(auth, container, backup_name)) == NULL) {
		fprintf(stderr, "error: unable to list container %s\n",
			container);
		exit(EXIT_FAILURE);
	}

	for (size_t i = 0; i < list->object_count; i++) {
		if (chunk_belongs_to(list->objects[i].name, backup_name)) {
			container_list_free(list);
			return(true);
		}
	}

	container_list_free(list);

	return(false);
}

/*********************************************************************//**
Fills auth_info with response from keystone response.
@return	true is response parsed successfully */
static
bool
swift_parse_keystone_response_v2(char *response, size_t response_length,
				swift_auth_info *auth_info)
{
	enum {MAX_DEPTH=20};
	enum label_t {NONE, ACCESS, CATALOG, ENDPOINTS, TOKEN};

	char filtered_url[SWIFT_MAX_URL_SIZE];
	char public_url[SWIFT_MAX_URL_SIZE];
	char region[SWIFT_MAX_URL_SIZE];
	char id[SWIFT_MAX_URL_SIZE];
	char token_id[SWIFT_MAX_URL_SIZE];
	char type[SWIFT_MAX_URL_SIZE];

	struct stack_t {
		jsmntok_t *t;
		int n_items;
		label_t label;
	};

	stack_t stack[MAX_DEPTH];
	jsmntok_t *tokens;
	int level;

	tokens = json_tokenise(response, response_length, 200);

	stack[0].t = &tokens[0];
	stack[0].label = NONE;
	stack[0].n_items = 1;
	level = 0;

	for (size_t i = 0, j = 1; j > 0; i++, j--) {
		jsmntok_t *t = &tokens[i];

		assert(t->start != -1 && t->end != -1);
		assert(level >= 0);

		--stack[level].n_items;

		switch (t->type) {
		case JSMN_ARRAY:
		case JSMN_OBJECT:
			if (level < MAX_DEPTH - 1) {
				level++;
			}
			stack[level].t = t;
			stack[level].label = NONE;
			if (t->type == JSMN_ARRAY) {
				stack[level].n_items = t->size;
				j += t->size;
			} else {
				stack[level].n_items = t->size * 2;
				j += t->size * 2;
			}
			break;
		case JSMN_PRIMITIVE:
		case JSMN_STRING:
			if (stack[level].t->type == JSMN_OBJECT &&
			    stack[level].n_items % 2 == 1) {
				/* key */
				if (json_token_eq(response, t, "access")) {
					stack[level].label = ACCESS;
				}
				if (json_token_eq(response, t,
							"serviceCatalog")) {
					stack[level].label = CATALOG;
				}
				if (json_token_eq(response, t, "endpoints")) {
					stack[level].label = ENDPOINTS;
				}
				if (json_token_eq(response, t, "token")) {
					stack[level].label = TOKEN;
				}
				if (json_token_eq(response, t, "id")) {
					json_token_str(response, &tokens[i + 1],
							id, sizeof(id));
				}
				if (json_token_eq(response, t, "id")
				    && stack[level - 1].label == TOKEN) {
					json_token_str(response, &tokens[i + 1],
						token_id, sizeof(token_id));
				}
				if (json_token_eq(response, t, "region")) {
					json_token_str(response, &tokens[i + 1],
						region, sizeof(region));
				}
				if (json_token_eq(response, t, "publicURL")) {
					json_token_str(response, &tokens[i + 1],
						public_url, sizeof(public_url));
				}
				if (json_token_eq(response, t, "type")) {
					json_token_str(response, &tokens[i + 1],
						type, sizeof(type));
				}
			}
			break;
		}

		while (stack[level].n_items == 0 && level > 0) {
			if (stack[level].t->type == JSMN_OBJECT
			    && level == 6
			    && stack[level - 1].t->type == JSMN_ARRAY
			    && stack[level - 2].label == ENDPOINTS) {
				if (opt_swift_region == NULL
				    || strcmp(opt_swift_region, region) == 0) {
					strncpy(filtered_url, public_url,
						sizeof(filtered_url));
				}
			}
			if (stack[level].t->type == JSMN_OBJECT &&
				level == 4 &&
				stack[level - 1].t->type == JSMN_ARRAY &&
				stack[level - 2].label == CATALOG) {
				if (strcmp(type, "object-store") == 0) {
					strncpy(auth_info->url, filtered_url,
						sizeof(auth_info->url));
				}
			}
			--level;
		}
	}

	free(tokens);

	strncpy(auth_info->token, token_id, sizeof(auth_info->token));

	assert(level == 0);

	if (*auth_info->token == 0) {
		fprintf(stderr, "error: can not receive token from response\n");
		return(false);
	}

	if (*auth_info->url == 0) {
		fprintf(stderr, "error: can not get URL from response\n");
		return(false);
	}

	return(true);
}

/*********************************************************************//**
Authenticate against Swift TempAuth. Fills swift_auth_info struct.
Uses creadentials privided as global variables.
@returns true if access is granted and token received. */
static
bool
swift_keystone_auth_v2(const char *auth_url, swift_auth_info *info)
{
	char tenant_arg[SWIFT_MAX_URL_SIZE];
	char payload[SWIFT_MAX_URL_SIZE];
	struct curl_slist *slist = NULL;
	download_buffer_info buf_info;
	long http_code;
	CURLcode res;
	CURL *curl;
	bool auth_res = false;

	memset(&buf_info, 0, sizeof(buf_info));

	if (opt_swift_user == NULL) {
		fprintf(stderr, "error: both --swift-user is required "
			"for keystone authentication.\n");
		return(false);
	}

	if (opt_swift_password == NULL) {
		fprintf(stderr, "error: both --swift-password is required "
			"for keystone authentication.\n");
		return(false);
	}

	if (opt_swift_tenant != NULL && opt_swift_tenant_id != NULL) {
		fprintf(stderr, "error: both --swift-tenant and "
			"--swift-tenant-id specified for keystone "
			"authentication.\n");
		return(false);
	}

	if (opt_swift_tenant != NULL) {
		snprintf(tenant_arg, sizeof(tenant_arg), ",\"%s\":\"%s\"",
			"tenantName", opt_swift_tenant);
	} else if (opt_swift_tenant_id != NULL) {
		snprintf(tenant_arg, sizeof(tenant_arg), ",\"%s\":\"%s\"",
			"tenantId", opt_swift_tenant_id);
	} else {
		*tenant_arg = 0;
	}

	snprintf(payload, sizeof(payload), "{\"auth\": "
		"{\"passwordCredentials\": {\"username\":\"%s\","
		"\"password\":\"%s\"}%s}}",
		opt_swift_user, opt_swift_password, tenant_arg);

	curl = curl_easy_init();

	if (curl != NULL) {

		slist = curl_slist_append(slist,
					  "Content-Type: application/json");
		slist = curl_slist_append(slist,
					  "Accept: application/json");

		curl_easy_setopt(curl, CURLOPT_VERBOSE, opt_verbose);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_URL, auth_url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_buffer_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf_info);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
				 fetch_buffer_header_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA,
				 &buf_info);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

		if (opt_cacert != NULL)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opt_cacert);
		if (opt_insecure)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			fprintf(stderr,
				"error: curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
			goto cleanup;
		}
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code < 200 || http_code >= 300) {
			fprintf(stderr, "error: request failed "
				"with response code: %ld\n", http_code);
			res = CURLE_LOGIN_DENIED;
			goto cleanup;
		}
	} else {
		res = CURLE_FAILED_INIT;
		fprintf(stderr, "error: curl_easy_init() failed\n");
		goto cleanup;
	}

	if (!swift_parse_keystone_response_v2(buf_info.buf,
				buf_info.size, info)) {
		goto cleanup;
	}

	auth_res = true;

cleanup:
	if (slist) {
		curl_slist_free_all(slist);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}

	free(buf_info.buf);

	return(auth_res);
}


/*********************************************************************//**
Fills auth_info with response from keystone response.
@return	true is response parsed successfully */
static
bool
swift_parse_keystone_response_v3(char *response, size_t response_length,
				swift_auth_info *auth_info)
{
	enum {MAX_DEPTH=20};
	enum label_t {NONE, TOKEN, CATALOG, ENDPOINTS};

	char url[SWIFT_MAX_URL_SIZE];
	char filtered_url[SWIFT_MAX_URL_SIZE];
	char region[SWIFT_MAX_URL_SIZE];
	char interface[SWIFT_MAX_URL_SIZE];
	char type[SWIFT_MAX_URL_SIZE];

	struct stack_t {
		jsmntok_t *t;
		int n_items;
		label_t label;
	};

	stack_t stack[MAX_DEPTH];
	jsmntok_t *tokens;
	int level;

	tokens = json_tokenise(response, response_length, 200);

	stack[0].t = &tokens[0];
	stack[0].label = NONE;
	stack[0].n_items = 1;
	level = 0;

	for (size_t i = 0, j = 1; j > 0; i++, j--) {
		jsmntok_t *t = &tokens[i];

		assert(t->start != -1 && t->end != -1);
		assert(level >= 0);

		--stack[level].n_items;

		switch (t->type) {
		case JSMN_ARRAY:
		case JSMN_OBJECT:
			if (level < MAX_DEPTH - 1) {
				level++;
			}
			stack[level].t = t;
			stack[level].label = NONE;
			if (t->type == JSMN_ARRAY) {
				stack[level].n_items = t->size;
				j += t->size;
			} else {
				stack[level].n_items = t->size * 2;
				j += t->size * 2;
			}
			break;
		case JSMN_PRIMITIVE:
		case JSMN_STRING:
			if (stack[level].t->type == JSMN_OBJECT &&
			    stack[level].n_items % 2 == 1) {
				/* key */
				if (json_token_eq(response, t, "token")) {
					stack[level].label = TOKEN;
					fprintf(stderr, "token\n");
				}
				if (json_token_eq(response, t,
							"catalog")) {
					stack[level].label = CATALOG;
					fprintf(stderr, "catalog\n");
				}
				if (json_token_eq(response, t, "endpoints")) {
					stack[level].label = ENDPOINTS;
				}
				if (json_token_eq(response, t, "region")) {
					json_token_str(response, &tokens[i + 1],
						region, sizeof(region));
				}
				if (json_token_eq(response, t, "url")) {
					json_token_str(response, &tokens[i + 1],
						url, sizeof(url));
				}
				if (json_token_eq(response, t, "interface")) {
					json_token_str(response, &tokens[i + 1],
						interface, sizeof(interface));
				}
				if (json_token_eq(response, t, "type")) {
					json_token_str(response, &tokens[i + 1],
						type, sizeof(type));
				}
			}
			break;
		}

		while (stack[level].n_items == 0 && level > 0) {
			if (stack[level].t->type == JSMN_OBJECT
			    && level == 6
			    && stack[level - 1].t->type == JSMN_ARRAY
			    && stack[level - 2].label == ENDPOINTS) {
				if ((opt_swift_region == NULL
				     || strcmp(opt_swift_region, region) == 0)
				    && strcmp(interface, "public") == 0) {
					strncpy(filtered_url, url,
						sizeof(filtered_url));
				}
			}
			if (stack[level].t->type == JSMN_OBJECT &&
				level == 4 &&
				stack[level - 1].t->type == JSMN_ARRAY &&
				stack[level - 2].label == CATALOG) {
				if (strcmp(type, "object-store") == 0) {
					strncpy(auth_info->url, filtered_url,
						sizeof(auth_info->url));
				}
			}
			--level;
		}
	}

	free(tokens);

	assert(level == 0);

	if (*auth_info->url == 0) {
		fprintf(stderr, "error: can not get URL from response\n");
		return(false);
	}

	return(true);
}

/*********************************************************************//**
Captures X-Subject-Token header. */
static
size_t keystone_v3_header_cb(char *ptr, size_t size, size_t nmemb, void *data)
{
	swift_auth_info *info = (swift_auth_info*)(data);

	get_http_header("X-Subject-Token: ", ptr,
			info->token, array_elements(info->token));

	return nmemb * size;
}

/*********************************************************************//**
Authenticate against Swift TempAuth. Fills swift_auth_info struct.
Uses creadentials privided as global variables.
@returns true if access is granted and token received. */
static
bool
swift_keystone_auth_v3(const char *auth_url, swift_auth_info *info)
{
	char scope[SWIFT_MAX_URL_SIZE];
	char domain[SWIFT_MAX_URL_SIZE];
	char payload[SWIFT_MAX_URL_SIZE];
	struct curl_slist *slist = NULL;
	download_buffer_info buf_info;
	long http_code;
	CURLcode res;
	CURL *curl;
	bool auth_res = false;

	memset(&buf_info, 0, sizeof(buf_info));
	buf_info.custom_header_callback = keystone_v3_header_cb;
	buf_info.custom_header_callback_data = info;

	if (opt_swift_user == NULL) {
		fprintf(stderr, "error: both --swift-user is required "
			"for keystone authentication.\n");
		return(false);
	}

	if (opt_swift_password == NULL) {
		fprintf(stderr, "error: both --swift-password is required "
			"for keystone authentication.\n");
		return(false);
	}

	if (opt_swift_project_id != NULL && opt_swift_project != NULL) {
		fprintf(stderr, "error: both --swift-project and "
			"--swift-project-id specified for keystone "
			"authentication.\n");
		return(false);
	}

	if (opt_swift_domain_id != NULL && opt_swift_domain != NULL) {
		fprintf(stderr, "error: both --swift-domain and "
			"--swift-domain-id specified for keystone "
			"authentication.\n");
		return(false);
	}

	if (opt_swift_project_id != NULL && opt_swift_domain != NULL) {
		fprintf(stderr, "error: both --swift-project-id and "
			"--swift-domain specified for keystone "
			"authentication.\n");
		return(false);
	}

	if (opt_swift_project_id != NULL && opt_swift_domain_id != NULL) {
		fprintf(stderr, "error: both --swift-project-id and "
			"--swift-domain-id specified for keystone "
			"authentication.\n");
		return(false);
	}

	scope[0] = 0; domain[0] = 0;

	if (opt_swift_domain != NULL) {
		snprintf(domain, sizeof(domain),
			",{\"domain\":{\"name\":\"%s\"}}",
			opt_swift_domain);
	} else if (opt_swift_domain_id != NULL) {
		snprintf(domain, sizeof(domain),
			",{\"domain\":{\"id\":\"%s\"}}",
			opt_swift_domain_id);
	}

	if (opt_swift_project_id != NULL) {
		snprintf(scope, sizeof(scope),
			",\"scope\":{\"project\":{\"id\":\"%s\"}}",
			opt_swift_project_id);
	} else if (opt_swift_project != NULL) {
		snprintf(scope, sizeof(scope),
			",\"scope\":{\"project\":{\"name\":\"%s\"%s}}",
			opt_swift_project_id, domain);
	}

	snprintf(payload, sizeof(payload), "{\"auth\":{\"identity\":"
		"{\"methods\":[\"password\"],\"password\":{\"user\":"
		"{\"name\":\"%s\",\"password\":\"%s\"%s}}}%s}}",
		opt_swift_user, opt_swift_password,
		*scope ? "" : ",\"domain\":{\"id\":\"default\"}",
		scope);

	curl = curl_easy_init();

	if (curl != NULL) {

		slist = curl_slist_append(slist,
					  "Content-Type: application/json");
		slist = curl_slist_append(slist,
					  "Accept: application/json");

		curl_easy_setopt(curl, CURLOPT_VERBOSE, opt_verbose);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_URL, auth_url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fetch_buffer_cb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf_info);
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
				 fetch_buffer_header_cb);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA,
				 &buf_info);
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);

		if (opt_cacert != NULL)
			curl_easy_setopt(curl, CURLOPT_CAINFO, opt_cacert);
		if (opt_insecure)
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, FALSE);

		res = curl_easy_perform(curl);

		if (res != CURLE_OK) {
			fprintf(stderr,
				"error: curl_easy_perform() failed: %s\n",
				curl_easy_strerror(res));
			goto cleanup;
		}
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
		if (http_code < 200 || http_code >= 300) {
			fprintf(stderr, "error: request failed "
				"with response code: %ld\n", http_code);
			res = CURLE_LOGIN_DENIED;
			goto cleanup;
		}
	} else {
		res = CURLE_FAILED_INIT;
		fprintf(stderr, "error: curl_easy_init() failed\n");
		goto cleanup;
	}

	if (!swift_parse_keystone_response_v3(buf_info.buf,
				buf_info.size, info)) {
		goto cleanup;
	}

	auth_res = true;

cleanup:
	if (slist) {
		curl_slist_free_all(slist);
	}
	if (curl) {
		curl_easy_cleanup(curl);
	}

	free(buf_info.buf);

	return(auth_res);
}

int main(int argc, char **argv)
{
	swift_auth_info info;
	char auth_url[SWIFT_MAX_URL_SIZE];

	MY_INIT(argv[0]);

        /* handle_options in parse_args is destructive so
         * we make a copy of our argument pointers so we can
         * mask the sensitive values afterwards */
        char **mask_argv = (char **)malloc(sizeof(char *) * (argc - 1));
        memcpy(mask_argv, argv + 1, sizeof(char *) * (argc - 1));

	if (parse_args(argc, argv)) {
		return(EXIT_FAILURE);
	}

        mask_args(argc, mask_argv);  /* mask args on cmdline */

	curl_global_init(CURL_GLOBAL_ALL);

	if (opt_swift_auth_version == NULL || *opt_swift_auth_version == '1') {
		/* TempAuth */
		snprintf(auth_url, SWIFT_MAX_URL_SIZE, "%sauth/v%s/",
			 opt_swift_auth_url, opt_swift_auth_version ?
			 			opt_swift_auth_version : "1.0");

		if (!swift_temp_auth(auth_url, &info)) {
			fprintf(stderr, "error: failed to authenticate\n");
			return(EXIT_FAILURE);
		}

	} else if (*opt_swift_auth_version == '2') {
		/* Keystone v2 */
		snprintf(auth_url, SWIFT_MAX_URL_SIZE, "%sv%s/tokens",
			 opt_swift_auth_url, opt_swift_auth_version);

		if (!swift_keystone_auth_v2(auth_url, &info)) {
			fprintf(stderr, "error: failed to authenticate\n");
			return(EXIT_FAILURE);
		}

	} else if (*opt_swift_auth_version == '3') {
		/* Keystone v3 */
		snprintf(auth_url, SWIFT_MAX_URL_SIZE, "%sv%s/auth/tokens",
			 opt_swift_auth_url, opt_swift_auth_version);

		if (!swift_keystone_auth_v3(auth_url, &info)) {
			fprintf(stderr, "error: failed to authenticate\n");
			exit(EXIT_FAILURE);
		}

	}

	if (opt_swift_storage_url != NULL) {
		snprintf(info.url, sizeof(info.url), "%s",
			 opt_swift_storage_url);
	}

	fprintf(stderr, "Object store URL: %s\n", info.url);

	if (opt_mode == MODE_PUT) {

		if (swift_create_container(&info, opt_swift_container) != 0) {
			fprintf(stderr, "error: failed to create "
				"container %s\n",
				opt_swift_container);
			return(EXIT_FAILURE);
		}

		if (swift_backup_exists(&info, opt_swift_container, opt_name)) {
			fprintf(stderr, "error: backup named '%s' "
				"already exists!\n",
				opt_name);
			return(EXIT_FAILURE);
		}

		if (swift_upload_parts(&info, opt_swift_container,
					opt_name) != 0) {
			fprintf(stderr, "error: upload failed\n");
			return(EXIT_FAILURE);
		}

	} else if (opt_mode == MODE_GET) {

		if (swift_download(&info, opt_swift_container, opt_name)
				   != CURLE_OK) {
			fprintf(stderr, "error: download failed\n");
			return(EXIT_FAILURE);
		}

	} else if (opt_mode == MODE_DELETE) {

		if (swift_delete(&info, opt_swift_container, opt_name)
				   != CURLE_OK) {
			fprintf(stderr, "error: delete failed\n");
			return(EXIT_FAILURE);
		}

	} else {
		fprintf(stderr, "Unknown command supplied.\n");
		exit(EXIT_FAILURE);
	}

	curl_global_cleanup();

	return(EXIT_SUCCESS);
}
