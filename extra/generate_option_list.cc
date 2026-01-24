/* Copyright (c) 2024, Väinö Mäkelä
   With some small changes by Michael Widenius

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA */

#include <my_global.h>
#include <my_sys.h>
#include <my_dir.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#define PCRE2_STATIC 1 /* Important on Windows */
#include "pcre2.h"

struct parsed
{
  std::set<std::string> options;
  std::map<std::string, std::vector<std::string>> enums;
  std::map<std::string, std::vector<std::string>> sets;
};

#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
     __attribute__((no_sanitize("memory"))) /* we may run with uninstrumented std::string */
#  endif
#endif
void escape_command(std::ostringstream &out, const char *command)
{
  out << '\"';
  while (*command)
  {
    switch (*command) {
    case '\"':
    case '\\':
    case '$':
      out << '\\';
      /* falls through */
    default:
      out << *command;
    }
    command++;
  }
  out << '\"';
}

std::string read_output(FILE *f)
{
  char buf[4096];
  size_t read_bytes;
  std::string output;
  do
  {
    read_bytes= std::fread(buf, 1, sizeof buf, f);
    output.append(buf, read_bytes);
  } while (read_bytes > 0);
  return output;
}


/* Link all plugins to a temporary directory so that MariaDB can use them */

void link_plugins(const char *tmpdir, const char *plugin_dir, const char *path,
                  std::ostringstream *command)
{
  struct fileinfo *ptr, *end;
  uint first= 0;
  char tmp_name[FN_REFLEN], org_name[FN_REFLEN], rel_path[FN_REFLEN];
  char dir_sep[2];
  MY_DIR *dir;

  if (!(dir=my_dir(plugin_dir, MYF(MY_WME | MY_WANT_STAT))))
    return;

  dir_sep[0]= FN_LIBCHAR; dir_sep[1]= 0;

  end= dir->dir_entry + dir->number_of_files;
  for (ptr= dir->dir_entry ; ptr < end ; ptr++)
  {
    const char *ext;
    if (ptr->mystat->st_mode & S_IFDIR)
    {
      strxnmov(tmp_name, sizeof(tmp_name)-1, plugin_dir, dir_sep, ptr->name,
               NullS);
      strxnmov(rel_path, sizeof(rel_path)-1, path, dir_sep, ptr->name, NullS);
      link_plugins(tmpdir, tmp_name, rel_path, command);
      continue;
    }

    ext= fn_ext(ptr->name);
    if (!strcmp(ext, ".so") || !strcmp(ext, ".ddl"))
    {
      fn_format(tmp_name, ptr->name, tmpdir, "", MYF(MY_REPLACE_DIR));
      fn_format(org_name, ptr->name, path, "", MYF(MY_REPLACE_DIR));

      /* plugin found, symlink it to plugin_dir */
      my_delete(tmp_name, MYF(MY_NOSYMLINKS));
      my_symlink(org_name, tmp_name, MYF(0));

      if (first++)
        (*command) << ";";
      (*command) << ptr->name;
    }
  }
  my_dirend(dir);
}


#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
     __attribute__((no_sanitize("memory"))) /* we may run with uninstrumented std::string */
#  endif
#endif


std::string call_mariadbd(const char *mariadbd_path)
{
  std::string output;
  std::ostringstream command;
  char plugin_dir[FN_REFLEN], plugin_tmp_dir[FN_REFLEN+20], *end;
  size_t dir_length;

  // command << "valgrind --leak-check=full ";
  escape_command(command, mariadbd_path);
  dirname_part(plugin_dir, mariadbd_path, &dir_length);

  if (dir_length <= sizeof(plugin_dir)-20)
  {
    end= strmov(plugin_dir + dir_length, "..");
    *end++= FN_LIBCHAR;
    end= strmov(end, "plugin");
  }
  end= strmov(plugin_tmp_dir, plugin_dir);
  *end++= FN_LIBCHAR;
  strmov(end, "tmp");

  my_rmtree(plugin_tmp_dir, MYF(MY_NOSYMLINKS));
  my_mkdir(plugin_tmp_dir, 0777, MYF(MY_WME));

  command << " --no-defaults"
             " --silent-startup"
             " --plugin-maturity=unknown"
             " --plugin-dir=";
  command << plugin_tmp_dir;
  command << " --plugin-load=\"";

  link_plugins(plugin_tmp_dir, plugin_dir, "..", &command);

  command << "\""
             " --verbose"
             " --help";

  FILE *f= my_popen(command.str().c_str(), "r");
  if (!f)
  {
    perror("failed to read mariadbd output");
    my_pclose(f);
    my_rmtree(plugin_tmp_dir, MYF(MY_NOSYMLINKS | MY_WME));
    exit(1);
  }
  output= read_output(f);
  my_pclose(f);
  my_rmtree(plugin_tmp_dir, MYF(MY_WME));
  return output;
}

std::string remove_paretheses(std::string &str)
{
  std::string result;
  bool in_parens= false;
  for (auto it= str.begin(); it != str.end(); ++it)
  {
    if (*it == '(')
    {
      in_parens= true;
    }
    else if (in_parens && *it == ')')
    {
      in_parens= false;
    }
    else if (!in_parens)
    {
      result.push_back(*it);
    }
  }
  return result;
}

std::vector<std::string> split_list(std::string &str)
{
  std::vector<std::string> result;
  std::string current;
  std::string parens_removed= remove_paretheses(str);
  bool seen_dot= false;

  for (auto it= parens_removed.begin(); it != parens_removed.end(); ++it)
  {
    switch (*it) {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case ',':
      seen_dot= false;
      if (!current.empty())
        result.push_back(current);
      current.clear();
      break;
    case '.':
      seen_dot= true;
      break;
    default:
      if (seen_dot)
        current.push_back('.');
      current.push_back(*it);
    }
  }
  if (!current.empty())
    result.push_back(current);
  return result;
}

std::string capture_group(std::string &str, PCRE2_SIZE *ovector, int group)
{
  return str.substr(ovector[2 * group],
                    ovector[2 * group + 1] - ovector[2 * group]);
}


/*
  Ignore some special mariadb options from using enum/set
*/

bool ignore_option(std::string *option)
{
  /*
    wsrep_provider must not be threated as an enum as in mariadbd --help is both
    a string and an enum.
  */
  if (!strcmp(option->c_str(), "wsrep_provider"))
    return 1;                                   // Ignore
  return 0;
}


struct parsed parse_output(std::string &output)
{
  struct parsed result= {};
  const char *pattern=
      "# Consider all lines that start with '  --' or '  -x, --'as options.\n"
      "^\\ \\ (?:-.,\\ )?--([^\\ =\\[]+)\n"
      "(?:\n"
      "  # Check for possible enum or set values until we hit\n"
      "  # '  -' at the start of a line. This won't work for\n"
      "  # the last option but should work for most ones.\n"
      "  (?:(?<!^\\ \\ -).)*\n"
      "  (?:(?:One\\s+of:(.*?))|(?:Any\\s+combination\\s+of:(.*?)))\n"
      "  # Sets end with \"  Use 'ALL'...\"\n"
      "  (?=^\\ \\ (?:-|Use))\n"
      ")?";
  pcre2_match_data *match_data;
  PCRE2_SIZE *ovector;
  PCRE2_SIZE offset= 0;
  int errornumber;
  PCRE2_SIZE erroroffset;
  pcre2_code *re=
      pcre2_compile((PCRE2_SPTR8) pattern, PCRE2_ZERO_TERMINATED,
                    PCRE2_EXTENDED | PCRE2_MULTILINE | PCRE2_DOTALL,
                    &errornumber, &erroroffset, nullptr);
  if (!re)
  {
    PCRE2_UCHAR buffer[256];
    pcre2_get_error_message(errornumber, buffer, sizeof(buffer));
    std::cerr << "PCRE2 compilation failed at offset " << erroroffset << ": "
              << buffer << '\n';
    exit(1);
  }

  match_data= pcre2_match_data_create_from_pattern(re, nullptr);
  ovector= pcre2_get_ovector_pointer(match_data);

  for (;;)
  {
    std::string option;
    std::string enum_part;
    std::string set_part;
    std::vector<std::string> enum_options;
    std::vector<std::string> set_options;
    int rc= pcre2_match(re, (PCRE2_SPTR8) output.c_str(), output.length(),
                        offset, 0, match_data, nullptr);
    if (rc == PCRE2_ERROR_NOMATCH)
    {
      break;
    }
    if (rc < 0)
    {
      std::cerr << "Matching error " << rc << '\n';
      exit(1);
    }

    offset= ovector[1];
    option= capture_group(output, ovector, 1);
    std::replace(option.begin(), option.end(), '-', '_');
    if (rc > 2 && ovector[2 * 2] != PCRE2_UNSET)
    {
      enum_part= capture_group(output, ovector, 2);
      enum_options= split_list(enum_part);
    }
    if (rc > 3 && ovector[2 * 3] != PCRE2_UNSET)
    {
      set_part= capture_group(output, ovector, 3);
      set_options= split_list(set_part);
    }

    result.options.insert(option);
    if (ignore_option(&option))
      continue;

    if (!enum_options.empty())
    {
      result.enums.emplace(option, enum_options);
    }
    if (!set_options.empty())
    {
      result.sets.emplace(option, set_options);
    }
  }

  pcre2_match_data_free(match_data);
  pcre2_code_free(re);
  return result;
}

void write_typelibs(std::ostream &out,
                    std::map<std::string, std::vector<std::string>> &map)
{
  for (auto option= map.begin(); option != map.end(); ++option)
  {
    const std::string &option_name= (*option).first;
    const std::vector<std::string> &values= (*option).second;
    out << "\nstatic const char *valid_" << option_name << "_values[] = {\n";
    for (auto value= values.begin(); value != values.end(); ++value)
    {
      out << "\"" << *value << "\",\n";
    }
    out << "0\n};\n";
    out << "static TYPELIB valid_" << option_name << "_values_typelib = {\n"
        << "array_elements(valid_" << option_name << "_values)-1,\n"
        << "\"\", valid_" << option_name << "_values, 0, 0};\n";
  }
}

void write_typelib_map(std::ostream &out, std::string name,
                       std::map<std::string, std::vector<std::string>> &map)
{
  out << "\nstatic const char *mariadbd_" << name << "_options[] = {\n";
  for (auto option= map.begin(); option != map.end(); ++option)
  {
    out << "\"" << (*option).first << "\",\n";
  }
  out << "};\n"
      << "\nstatic TYPELIB *mariadbd_" << name << "_typelibs[] = {\n";
  for (auto option= map.begin(); option != map.end(); ++option)
  {
    out << "&valid_" << (*option).first << "_values_typelib,\n";
  }
  out << "};\n";
}

void write_output(const char *path, struct parsed &parsed_output)
{
  std::ofstream out(path);
  out << "/* Automatically generated by generate_option_list */\n\n";
  out << "#ifndef _mariadbd_options_h\n";
  out << "#define _mariadbd_options_h\n";
  out << "static const char *mariadbd_valid_options[]= {\n";
  for (auto option= parsed_output.options.begin();
       option != parsed_output.options.end(); ++option)
  {
    out << '\"' << *option << "\",\n";
  }
  out << "};\n";

  write_typelibs(out, parsed_output.enums);
  write_typelib_map(out, "enum", parsed_output.enums);

  write_typelibs(out, parsed_output.sets);
  write_typelib_map(out, "set", parsed_output.sets);

  out << "#endif /* _mariadbd_options_h */\n";
}

int main(int argc, const char *argv[])
{
  std::string mariadbd_output;
  struct parsed parsed_output;

  if (argc != 3)
  {
    std::cerr << "usage: " << argv[0] << " <mariadbd_path> <output_path>\n";
    return 1;
  }
  MY_INIT(argv[0]);

  mariadbd_output= call_mariadbd(argv[1]);
  parsed_output= parse_output(mariadbd_output);
  write_output(argv[2], parsed_output);

  my_end(0);
  exit(0);
}
