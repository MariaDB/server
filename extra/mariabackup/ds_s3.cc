#include "common.h"
#include "ds_s3.h"
#include <string>
#include "maria_def.h"
#include "s3_func.h"

struct ds_s3_ctx_t {
	ms3_st *client;
	const char *bucket;
	const char *path;
	uint64_t seq_num;
};

static ds_ctxt_t *s3_init(const void *s3_client);
static ds_file_t *s3_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *mystat);
static int s3_write(ds_file_t *file, const uchar *buf, size_t len);
static int s3_close(ds_file_t *file);
static void s3_deinit(ds_ctxt_t *ctxt);

datasink_t datasink_s3 = {
	&s3_init,
	&s3_open,
	&s3_write,
	&s3_close,
	&s3_deinit
};

static
ds_ctxt_t *
s3_init(const void *args_void)
{
	const ds_s3_args *args = static_cast<const ds_s3_args *>(args_void);

	s3_init_library();

	S3_INFO info;
	info.protocol_version= (uint8_t) args->protocol_version;
	lex_string_set(&info.host_name,  args->host_name);
	lex_string_set(&info.access_key, args->access_key);
	lex_string_set(&info.secret_key, args->secret_key);
	lex_string_set(&info.region,     args->region);
	lex_string_set(&info.bucket,     args->bucket);
	ms3_st *s3_client;
	if (!(s3_client= s3_open_connection(&info)))
		die("Can't open connection to S3, error: %d %s", errno, ms3_error(errno));

	ms3_status_st status;
	if (!ms3_status(s3_client, args->bucket, args->path, &status))
		die("Can't stream to s3://%s%s as it already exists",
			args->bucket, args->path);
	ms3_list_st *list = nullptr;
	if (!ms3_list_dir(s3_client, args->bucket, args->path, &list) && list) {
		ms3_list_free(list);
		die("Can't stream to s3://%s%s as it already exists",
			args->bucket, args->path);
	}

	ds_ctxt_t *ctxt;
	ctxt = (ds_ctxt_t *)my_malloc(sizeof(ds_ctxt_t) + sizeof(ds_s3_ctx_t),
			MYF(MY_FAE));
	ds_s3_ctx_t *s3_ctx = reinterpret_cast<ds_s3_ctx_t *>(ctxt + 1);
	s3_ctx->client = s3_client;
	s3_ctx->bucket = args->bucket;
	s3_ctx->path = args->path;
	s3_ctx->seq_num = 0;
	ctxt->ptr = s3_ctx;

	return ctxt;
}

static
ds_file_t *
s3_open(ds_ctxt_t *ctxt,
		const char *path __attribute__((unused)),
		MY_STAT *mystat __attribute__((unused)))
{
	static char ds_s3_file_path[] = "s3";
	ds_file_t		*file;
	file = (ds_file_t *) my_malloc(sizeof(ds_file_t), MYF(MY_FAE));
	file->ptr = ctxt->ptr;
	file->path = ds_s3_file_path;
	return file;
}

static
int
s3_write(ds_file_t *file, const uchar *buf, size_t len)
{
	ds_s3_ctx_t *ctx = static_cast<ds_s3_ctx_t *>(file->ptr);
	std::string block_path(ctx->path);
	block_path.append("/").append(std::to_string(ctx->seq_num++));
	return s3_put_object(ctx->client, ctx->bucket, block_path.c_str(),
		const_cast<uchar *>(buf), len, 0);
}

static
int
s3_close(ds_file_t *file)
{
	my_free(file);
	return 1;
}

static
void
s3_deinit(ds_ctxt_t *ctxt)
{
	ms3_deinit(static_cast<ds_s3_ctx_t *>(ctxt->ptr)->client);
	s3_deinit_library();
	my_free(ctxt);
}
