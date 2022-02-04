let $is_win = `select convert(@@version_compile_os using latin1) IN ("Win32","Win64","Windows")`;
#echo is_win: $is_win;
if ($is_win)
{
  if(!$MYSQL_EMBEDDED)
  {
    skip Not supported with embedded on windows;
  }
}
