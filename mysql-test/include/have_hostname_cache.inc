# Ensure that host_cache is enabled

if (`SELECT @@skip_name_resolve != "OFF"`)
{
  skip No hostname_cache;
}
