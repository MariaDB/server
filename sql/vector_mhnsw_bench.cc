void mhnsw_run_benchmark();
struct st_mysql_plugin;
st_mysql_plugin *mysql_mandatory_plugins[] = {nullptr};
st_mysql_plugin *mysql_optional_plugins[] = {nullptr};

int main()
{
  mhnsw_run_benchmark();
  return 0;
}