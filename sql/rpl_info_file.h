/*
  Copyright (c) 2025 MariaDB

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details.

  You should have received a copy of the GNU General Public License along
  with this program; if not, write to the Free Software Foundation, Inc.,
  51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA.
*/

#ifndef RPL_INFO_FILE_H
#define RPL_INFO_FILE_H

#include <my_sys.h>   // IO_CACHE, FN_REFLEN, ...


/*
  Read one '\n'-terminated decimal unsigned integer from file.
  Returns false on success, true on EOF / parse error / out of range.
*/
bool uint_from_chars(IO_CACHE *file, ulonglong max_value, ulonglong *result);

/* Write an unsigned integer as decimal digits (no '\n'). */
void uint_to_chars(IO_CACHE *file, ulonglong value);

/*
  Common superclass of Master_info_file and Relay_log_info_file. Provides
  load and save code for their MySQL line-based sections. As only
  Master_info_file has a MariaDB key=value section with a mix of explicit
  and DEFAULT-able values, the code for those is in Master_info_file.

  Each value is an instance of an implementation of the Info_file::Value
  interface. The interface uses an explicit has_value flag (in the base)
  to track whether the value was explicitly set, instead of per-class
  sentinels. set_default() copies the default into value so reading the
  value never needs to check is_default().

  It's an old inconsistency that @ref Master_info_file's line count includes
  the line-count "value" while @ref Relay_log_info_file's excludes it.
  This is reason that Info_file::load_from_file() and
  Info_file::save_to_file() defines the line-count lines differently -
  It is so the two subclasses can implement either interpretation
  on top of the indifferent common code in this superclass.

  Multi-line method bodies live in rpl_info_file.cc.
*/
struct Info_file
{
  IO_CACHE file;

  /*
    Persistence interface for an unspecified item.

    has_value semantics:
      true:  the field holds an explicitly-set value (loaded from file
             or assigned by code).
      false: the field holds its DEFAULT (copied in by set_default()),
             or has not been initialized.
  */
  struct Value
  {
    virtual ~Value()= default;
    Value(const char *name)
      :variable_name(name)
      {}
    const char *variable_name;
    bool has_value= false;
    /* True if the value is the DEFAULT (i.e. has_value is false). */
    bool is_default() const { return !has_value; }
    /*
      Copy the DEFAULT into the value field and clear has_value. Returns
      true if no DEFAULT exists (i.e. the value is mandatory). Subclasses
      with a DEFAULT override this to copy in their default and return
      false. The return value is used only in
      Master_info_file::load_from_file(), to detect a bare key=value line
      whose value type has no DEFAULT (a malformed input).
    */
    virtual bool set_default() { has_value= false; return true; }
    /*
      Set the value by reading a line from the IO and consuming the '\n'.
      Sets has_value to true on success. Returns false on success,
      true on error.
    */
    virtual bool load_from(IO_CACHE *file)= 0;
    /*
      Write the value to the file without a '\n'.
      This is only called in the case is_default() is false.
    */
    virtual void save_to(IO_CACHE *file)= 0;
  };

  /* Unsigned 32-bit integer value (mandatory). */
  struct Uint_value: Value
  {
    Uint_value(const char *name): Value(name) {}
    uint value;
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };

  /* Unsigned 64-bit integer value (mandatory). */
  struct Ulonglong_value: Value
  {
    Ulonglong_value(const char *name): Value(name) {}
    ulonglong value;
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };

  /*
    Null-terminated string value (mandatory). The buffer is owned by the
    parent struct and passed in via the constructor.
  */
  struct String_value: Value
  {
    char *buf;
    size_t buf_size;
    String_value(const char *name, char *buf, size_t size):
      Value(name), buf(buf), buf_size(size) {}
    bool load_from(IO_CACHE *file) override;
    void save_to(IO_CACHE *file) override;
  };

  virtual ~Info_file()= default;
  virtual bool load_from_file()= 0;
  virtual void save_to_file()= 0;

protected:

  /*
    (Re)load the MySQL line-based section from the file.

    @param values  Array of pointers to value subobjects of *this. nullptr
                   marks a line to skip (MySQL-only fields MariaDB ignores).
                   The first element must be a String_value, to be
                   unambiguous with the line count line.
    @param size    Number of elements in values.
    @param default_line_count
      Some MySQL/MariaDB versions generate more lines than needed.
      Starting with MySQL/MariaDB 4.1.x for Master_info_file and 5.6.x
      for Relay_log_info_file, the first line is the number of value
      lines, including the line count itself. This parameter specifies
      the effective line count before those versions (where the first
      line is a filename rather than an integer).
    @return false on success, true on error.
  */
  bool load_from_file(Value *const *values, size_t size,
                      size_t default_line_count= 0,
                      int value_counter_correction= 0);

  /*
    Flush the MySQL line-based section to the file.

    @param values             Array of pointers to value subobjects.
                              nullptr entries write an empty line.
    @param size               Number of elements in values.
    @param total_line_count   Number of lines to declare on the first line.
                              If larger than size, the file is padded with
                              empty lines. This reservation provides
                              compatibility with MySQL, which has added
                              more old-style lines.
  */
  void save_to_file(Value *const *values, size_t size,
                    size_t total_line_count);
};

#endif
