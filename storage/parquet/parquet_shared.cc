#define MYSQL_SERVER 1

#include "parquet_shared.h"

#include "log.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <limits.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace {

std::string trim_copy(std::string value)
{
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::string strip_matching_quotes(std::string value)
{
  if (value.size() >= 2) {
    const char first = value.front();
    const char last = value.back();
    if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
      return value.substr(1, value.size() - 2);
  }
  return value;
}

std::string dirname_copy(const std::string &path)
{
  const size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? std::string() : path.substr(0, pos);
}

std::string join_path(const std::string &left, const std::string &right)
{
  if (left.empty())
    return right;
  if (left.back() == '/')
    return left + right;
  return left + "/" + right;
}

bool file_exists(const std::string &path)
{
  std::ifstream input(path);
  return input.good();
}

std::string current_working_dir()
{
  char buffer[PATH_MAX];
  if (getcwd(buffer, sizeof(buffer)) != nullptr)
    return buffer;
  return "";
}

std::string executable_dir()
{
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size) == 0)
    return dirname_copy(std::string(path.c_str()));
#elif defined(__linux__)
  char buffer[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (len > 0) {
    buffer[len] = '\0';
    return dirname_copy(buffer);
  }
#endif
  return "";
}

std::vector<std::string> parquet_env_candidate_paths()
{
  std::vector<std::string> candidates;

  if (const char *explicit_path = std::getenv("PARQUET_ENV_FILE")) {
    if (*explicit_path)
      candidates.emplace_back(explicit_path);
  }

  const std::string cwd = current_working_dir();
  if (!cwd.empty()) {
    candidates.push_back(join_path(cwd, ".env"));
    candidates.push_back(join_path(dirname_copy(cwd), ".env"));
    candidates.push_back(join_path(dirname_copy(dirname_copy(cwd)), ".env"));
  }

  const std::string exe_dir = executable_dir();
  if (!exe_dir.empty()) {
    candidates.push_back(join_path(exe_dir, ".env"));
    candidates.push_back(join_path(dirname_copy(exe_dir), ".env"));
    candidates.push_back(join_path(dirname_copy(dirname_copy(exe_dir)), ".env"));
  }

  std::vector<std::string> unique_candidates;
  std::unordered_set<std::string> seen;
  for (const std::string &candidate : candidates) {
    if (!candidate.empty() && seen.insert(candidate).second)
      unique_candidates.push_back(candidate);
  }

  return unique_candidates;
}

void load_env_file(const std::string &path,
                   std::unordered_map<std::string, std::string> *values)
{
  if (!values)
    return;

  std::ifstream input(path);
  if (!input)
    return;

  std::string line;
  while (std::getline(input, line)) {
    line = trim_copy(line);
    if (line.empty() || line[0] == '#')
      continue;
    if (line.rfind("export ", 0) == 0)
      line = trim_copy(line.substr(7));

    const size_t eq_pos = line.find('=');
    if (eq_pos == std::string::npos)
      continue;

    std::string key = trim_copy(line.substr(0, eq_pos));
    std::string value = trim_copy(line.substr(eq_pos + 1));
    if (key.empty())
      continue;

    const size_t comment_pos = value.find(" #");
    if (comment_pos != std::string::npos)
      value = trim_copy(value.substr(0, comment_pos));

    (*values)[key] = strip_matching_quotes(value);
  }
}

std::string ensure_trailing_slash(const std::string &value)
{
  if (value.empty() || value.back() == '/')
    return value;
  return value + "/";
}

std::string trim_slashes_copy(std::string value)
{
  while (!value.empty() && value.front() == '/')
    value.erase(value.begin());
  while (!value.empty() && value.back() == '/')
    value.pop_back();
  return value;
}

std::string single_line_copy(std::string value)
{
  for (char &ch : value) {
    if (ch == '\n' || ch == '\r' || ch == '\t')
      ch = ' ';
  }
  return trim_copy(value);
}

size_t curl_write_to_string(char *ptr, size_t size, size_t nmemb,
                            void *userdata)
{
  auto *body = static_cast<std::string *>(userdata);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

} // namespace

const ParquetRuntimeConfig &parquet_runtime_config()
{
  static ParquetRuntimeConfig config{
      "http://localhost:8181/catalog/v1/",
      "9a3b8dae-3bab-11f1-aa80-237d1c73ff26",
      "default",
      "mariadb-parquet-demo",
      "data",
      "us-east-2",
      "PLACEHOLDER",
      "PLACEHOLDER"};
  static std::once_flag once;

  std::call_once(once, [] {
    std::unordered_map<std::string, std::string> values;
    for (const std::string &candidate : parquet_env_candidate_paths()) {
      if (file_exists(candidate)) {
        load_env_file(candidate, &values);
        break;
      }
    }

    auto apply_value = [&](const char *key, std::string *target,
                           bool ensure_slash = false) {
      auto it = values.find(key);
      if (it != values.end() && !it->second.empty())
        *target = it->second;
      if (const char *env_value = std::getenv(key)) {
        if (*env_value)
          *target = env_value;
      }
      if (ensure_slash)
        *target = ensure_trailing_slash(*target);
    };

    apply_value("PARQUET_LAKEKEEPER_BASE_URL", &config.lakekeeper_base_url, true);
    apply_value("PARQUET_LAKEKEEPER_WAREHOUSE_ID",
                &config.lakekeeper_warehouse_id);
    apply_value("PARQUET_LAKEKEEPER_NAMESPACE", &config.lakekeeper_namespace);
    apply_value("PARQUET_S3_BUCKET", &config.s3_bucket);
    apply_value("PARQUET_S3_DATA_PREFIX", &config.s3_data_prefix);
    apply_value("PARQUET_S3_REGION", &config.s3_region);
    apply_value("PARQUET_S3_ACCESS_KEY_ID", &config.s3_access_key_id);
    apply_value("PARQUET_S3_SECRET_ACCESS_KEY", &config.s3_secret_access_key);

    config.s3_data_prefix = trim_slashes_copy(config.s3_data_prefix);
  });

  return config;
}

std::string quote_string_literal(const std::string &value)
{
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'')
      quoted += "''";
    else
      quoted += ch;
  }
  quoted += "'";
  return quoted;
}

std::string parquet_log_preview(const std::string &value, size_t max_length)
{
  std::string normalized = single_line_copy(value);
  if (normalized.size() <= max_length)
    return normalized;

  return normalized.substr(0, max_length) + "...<truncated>";
}

void parquet_log_info(const std::string &message)
{
  sql_print_information("Parquet: %s", message.c_str());
}

void parquet_log_warning(const std::string &message)
{
  sql_print_warning("Parquet: %s", message.c_str());
}

std::string parquet_s3_object_path(const std::string &object_name)
{
  const auto &config = parquet_runtime_config();
  std::string path = "s3://" + config.s3_bucket + "/";
  if (!config.s3_data_prefix.empty())
    path += config.s3_data_prefix + "/";
  path += object_name;
  return path;
}

void configure_duckdb_s3(duckdb::Connection *con)
{
  const auto &config = parquet_runtime_config();
  parquet_log_info("DuckDB configuring S3 region='" + config.s3_region +
                   "' bucket='" + config.s3_bucket + "' prefix='" +
                   config.s3_data_prefix + "'");
  con->Query("SET s3_region=" + quote_string_literal(config.s3_region));
  con->Query("SET s3_access_key_id=" +
             quote_string_literal(config.s3_access_key_id));
  con->Query("SET s3_secret_access_key=" +
             quote_string_literal(config.s3_secret_access_key));
}

std::string lakekeeper_table_collection_url()
{
  const auto &config = parquet_runtime_config();
  return config.lakekeeper_base_url + config.lakekeeper_warehouse_id +
         "/namespaces/" + config.lakekeeper_namespace + "/tables";
}

std::string lakekeeper_table_url(const std::string &table_name)
{
  return lakekeeper_table_collection_url() + "/" + table_name;
}

std::string lakekeeper_transaction_commit_url()
{
  const auto &config = parquet_runtime_config();
  return config.lakekeeper_base_url + config.lakekeeper_warehouse_id +
         "/transactions/commit";
}

std::string table_name_from_path(const std::string &table_path)
{
  size_t pos = table_path.find_last_of("/\\");
  return (pos == std::string::npos) ? table_path : table_path.substr(pos + 1);
}

std::vector<std::string> extract_manifest_paths(const std::string &response_body)
{
  std::vector<std::string> s3_files;
  std::unordered_set<std::string> seen_s3_files;
  size_t pos = 0;
  while ((pos = response_body.find("\"manifest-list\"", pos)) != std::string::npos) {
    size_t colon = response_body.find(':', pos);
    if (colon == std::string::npos)
      break;
    size_t value_start = response_body.find('"', colon + 1);
    if (value_start == std::string::npos)
      break;
    size_t value_end = response_body.find('"', value_start + 1);
    if (value_end == std::string::npos)
      break;

    std::string path = response_body.substr(value_start + 1,
                                            value_end - value_start - 1);
    if (path.rfind("s3://", 0) == 0 && seen_s3_files.insert(path).second)
      s3_files.push_back(path);

    pos = value_end + 1;
  }
  return s3_files;
}

bool fetch_lakekeeper_table_metadata(const std::string &table_name,
                                     std::string *response_body,
                                     long *http_code)
{
  if (!response_body || !http_code)
    return false;

  response_body->clear();
  *http_code = 0;

  const std::string lakekeeper_url = lakekeeper_table_url(table_name);
  parquet_log_info("LakeKeeper load table metadata table='" + table_name +
                   "' url='" + lakekeeper_url + "'");

  CURL *curl = curl_easy_init();
  if (!curl)
    return false;

  curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +curl_write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_body);
  CURLcode res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK) {
    parquet_log_info("LakeKeeper load table metadata complete table='" +
                     table_name + "' http_status=" +
                     std::to_string(*http_code) + " response=" +
                     parquet_log_preview(*response_body));
  } else {
    parquet_log_warning("LakeKeeper load table metadata failed table='" +
                        table_name + "' error='" +
                        std::string(curl_easy_strerror(res)) + "'");
  }

  return res == CURLE_OK;
}

bool resolve_parquet_data_files(const std::string &table_name,
                                std::vector<std::string> *s3_files,
                                long *http_code)
{
  if (!s3_files)
    return false;

  std::string response_body;
  long local_http_code = 0;
  if (!fetch_lakekeeper_table_metadata(table_name, &response_body, &local_http_code)) {
    if (http_code)
      *http_code = local_http_code;
    return false;
  }

  if (http_code)
    *http_code = local_http_code;

  if (local_http_code != 200) {
    s3_files->clear();
    return true;
  }

  *s3_files = extract_manifest_paths(response_body);
  return true;
}

std::string fetch_current_snapshot_data_file(const std::string &table_name)
{
  std::vector<std::string> s3_files;
  long http_code = 0;
  if (!resolve_parquet_data_files(table_name, &s3_files, &http_code))
    return "";
  if (http_code != 200 || s3_files.empty())
    return "";
  return s3_files.front();
}
