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

#include <cstdint>    // uintN_t
#include <charconv>   // std::from/to_chars and other helpers
#include <functional> // superclass of Info_file::Mem_fn
#include <my_sys.h>   // IO_CACHE, FN_REFLEN, ...


/** Helpers for reading and writing integers to and from @ref IO_CACHE
  TODO: Other components, if you find these useful,
    feel free to move these out of this Replication module.
*/
namespace Int_IO_CACHE
{
  /** Number of fully-utilized decimal digits plus
    * the partially-utilized digit (e.g., the 2's place in "2147483647")
    * The sign, if signed (:
  */
  template<typename I> static constexpr size_t BUF_SIZE=
    std::numeric_limits<I>::digits10 + 1 + std::numeric_limits<I>::is_signed;
  static constexpr auto ERRC_OK= std::errc();

  /**
    @ref IO_CACHE (reading one line with the `\n`) version of std::from_chars()
    @tparam I integer type
    @return `false` if the line has parsed successfully or `true` if error
  */
  template<typename I> static bool from_chars(IO_CACHE *file, I &value)
  {
    /**
      +2 for the terminating `\n\0` (They are ignored by
      std::from_chars(), but my_b_gets() includes them.)
    */
    char buf[BUF_SIZE<I> + 2];
    /// includes the `\n` but excludes the `\0`
    size_t length= my_b_gets(file, buf, sizeof(buf));
    if (!length) // EOF
      return true;
    // SFINAE if `I` is not a numeric type
    std::from_chars_result result= std::from_chars(buf, &(buf[length]), value);
    // Return `true` if the conversion failed or if the number ended early
    return result.ec != ERRC_OK || *(result.ptr) != '\n';
  }
  /**
    Convenience overload of from_chars(IO_CACHE *, I &) for `operator=` types
    @tparam I inner integer type
    @tparam T wrapper type
  */
  template<typename I, class T> static bool from_chars(IO_CACHE *file, T *self)
  {
    I value;
    if (from_chars(file, value))
      return true;
    (*self)= value;
    return false;
  }

  /**
    @ref IO_CACHE (writing *without* a `\n`) version of std::to_chars()
    @tparam I (inner) integer type
  */
  template<typename I> static void to_chars(IO_CACHE *file, I value)
  {
    /**
      my_b_printf() uses a buffer too,
      so we might as well save on format parsing and buffer resizing
    */
    char buf[BUF_SIZE<I>];
    std::to_chars_result result= std::to_chars(buf, &buf[sizeof(buf)], value);
    DBUG_ASSERT(result.ec == ERRC_OK);
    my_b_write(file, reinterpret_cast<const uchar *>(buf), result.ptr - buf);
  }
};


/**
  This common superclass of @ref Master_info_file and
  @ref Relay_log_info_file provides them common code for saving
  and loading values in their MySQL line-based sections.
  As only the @ref Master_info_file has a MariaDB `key=value`
  section with a mix of explicit and `DEFAULT`-able values,
  code for those are in @ref Master_info_file instead.

  Each value is an instance of an implementation of the
  @ref Info_file::Persistent interface. For convenience, they also have
  assignment and implicit conversion operators for their underlying types.

  C++ templates enables code reuse for those implementation structs, but
  templates are not suitable for the conventional header/implementation split.
  Thus, this and derived files are header-only units (methods are `inline`).
  Other files may include these files directly.
  [C++20 modules](https://en.cppreference.com/w/cpp/language/modules.html)
  can supercede the header-only design as well as headers' `#include` guards.
*/
struct Info_file
{
  IO_CACHE file;


  /// Persistence interface for an unspecified item
  struct Persistent
  {
    virtual ~Persistent()= default;
    // for save_to_file()
    virtual bool is_default() { return false; }
    /// @return `true` if the item is mandatory and couldn't provide a default
    virtual bool set_default() { return true; }
    /** set the value by reading a line from the IO and consume the `\n`
      @return `false` if the line has parsed successfully or `true` if error
      @post is_default() is `false`
    */
    virtual bool load_from(IO_CACHE *file)= 0;
    /** write the *effective* value to the IO **without** a `\n`
      (The caller will separately determine how
      to represent using the default value.)
    */
    virtual void save_to(IO_CACHE *file)= 0;
  };

  /** Integer Value
    @tparam I signed or unsigned integer type
    @see Master_info_file::Optional_int_value
      version with `DEFAULT` (not a subclass)
  */
  template<typename I> struct Int_value: Persistent
  {
    I value;
    operator I() { return value; }
    auto &operator=(I value)
    {
      this->value= value;
      return *this;
    }
    virtual bool load_from(IO_CACHE *file) override
    { return Int_IO_CACHE::from_chars(file, value); }
    virtual void save_to(IO_CACHE *file) override
    { return Int_IO_CACHE::to_chars(file, value); }
  };

  /// Null-Terminated String (usually file name) Value
  template<size_t size= FN_REFLEN> struct String_value: Persistent
  {
    char buf[size];
    /**
      Reads should consider this an immutable '\0'-terminated string (especially
      with @ref Optional_path_value where a `DEFAULT` may substitute the value).
      Writes may prefers to directly address the underlying @ref buf.
    */
    virtual operator const char *() { return buf; }
    /// @param other non-`nullptr` `\0`-terminated string
    auto &operator=(const char *other)
    {
      strmake(buf, other, size-1);
      return *this;
    }
    virtual bool load_from(IO_CACHE *file) override
    {
      size_t length= my_b_gets(file, buf, size);
      if (!length) // EOF
        return true;
      /// If we stopped on a newline, kill it.
      char &last_char= buf[length-1];
      if (last_char == '\n')
      {
        last_char= '\0';
        return false;
      }
      /*
        Consume the lost line break,
        or error if the line overflows the @ref buf.
      */
      return my_b_get(file) != '\n';
    }
    virtual void save_to(IO_CACHE *file) override
    {
      const char *buf= *this;
      my_b_write(file, reinterpret_cast<const uchar *>(buf), strlen(buf));
    }
  };


  virtual ~Info_file()= default;
  virtual bool load_from_file()= 0;
  virtual void save_to_file()= 0;

protected:

  /**
    std::Mem_fn()-like nullable replacement for
    [member pointer upcasting](https://wg21.link/P0149R3)
  */
  struct Mem_fn: std::function<Persistent &(Info_file *self)>
  {
    /// Null Constructor
    Mem_fn(std::nullptr_t null= nullptr):
      std::function<Persistent &(Info_file *)>(null) {}
    /** Non-Null Constructor
      @tparam T CRTP subclass of Info_file
      @tparam M @ref Persistent subclass of the member
      @param pm member pointer
    */
    template<class T, typename M> Mem_fn(M T::* pm):
      std::function<Persistent &(Info_file *)>(
        [pm](Info_file *self) -> Persistent &
        { return self->*static_cast<M Info_file::*>(pm); }
      ) {}
  };

  /**
    (Re)load the MySQL line-based section from the @ref file
    @param value_list
      List of wrapped member pointers to values. The first element must be a
      file name @ref String_value to be unambiguous with the line count line.
    @param default_line_count
      We cannot simply read lines until EOF as all versions
      of MySQL/MariaDB may generate more lines than needed.
      Therefore, starting with MySQL/MariaDB 4.1.x for @ref Master_info_file and
      5.6.x for @ref Relay_log_info_file, the first line of the file is number
      of one-line-per-value lines in the file, including this line count itself.
      This parameter specifies the number of effective lines before those
      versions (i.e., not counting the line count line if it was to have one),
      where the first line is a filename with extension
      (either contains a `.` or is entirely empty) rather than an integer.
    @return `false` if the file has parsed successfully or `true` if error
  */
  template<size_t size> bool load_from_file(
    const Mem_fn (&value_list)[size],
    size_t default_line_count= 0
  ) { return load_from_file(value_list, size, default_line_count); }
  /**
    Flush the MySQL line-based section to the @ref file
    @param value_list List of wrapped member pointers to values.
    @param total_line_count
      The number of lines to describe the file as on the first line of the file.
      If this is larger than `value_list.size()`, suffix the file with empty
      lines until the line count (including the line count line) is this many.
      This reservation provides compatibility with MySQL,
      who has added more old-style lines while MariaDB innovated.
  */
  template<size_t size> void save_to_file(
    const Mem_fn (&value_list)[size],
    size_t total_line_count= size + /* line count line */ 1
  ) { return save_to_file(value_list, size, total_line_count); }

private:
  bool
  load_from_file(const Mem_fn *values, size_t size, size_t default_line_count)
  {
    /**
      The first row is temporarily stored in the first value. If it is a line
      count and not a log name (new format), the second row will overwrite it.
    */
    auto &line1= dynamic_cast<String_value<> &>(values[0](this));
    if (line1.load_from(&file))
      return true;
    size_t line_count;
    std::from_chars_result result= std::from_chars(
      line1.buf, &line1.buf[sizeof(line1.buf)], line_count);
    /**
      If this first line was not a number - the line count,
      then it was the first value for real,
      so the for loop should then skip over it, the index 0 of the list.
    */
    size_t i= result.ec != Int_IO_CACHE::ERRC_OK || *(result.ptr) != '\0';
    /*
      Set the default after parsing: While std::from_chars() does not replace
      the output if it failed, it does replace if the line is not fully spent.
    */
    if (i)
      line_count= default_line_count;
    for (; i < line_count; ++i)
    {
      int c;
      if (i < size) // line known in the `value_list`
      {
        const Mem_fn &pm= values[i];
        if (pm)
        {
          if (pm(this).load_from(&file))
            return true;
          continue;
        }
      }
      /*
        Count and discard unrecognized lines.
        This is especially to prepare for @ref Master_info_file for MariaDB 10.0+,
        which reserves a bunch of lines before its unique `key=value` section
        to accomodate any future line-based (old-style) additions in MySQL.
        (This will make moving from MariaDB to MySQL easier by not
        requiring MySQL to recognize MariaDB `key=value` lines.)
      */
      while ((c= my_b_get(&file)) != '\n')
        if (c == my_b_EOF)
          return true; // EOF already?
    }
    return false;
  }

  void save_to_file(const Mem_fn *values, size_t size, size_t total_line_count)
  {
    DBUG_ASSERT(total_line_count > size);
    my_b_seek(&file, 0);
    /*
      If the new contents take less space than the previous file contents,
      then this code would write the file with unerased trailing garbage lines.
      But these garbage don't matter thanks to the number
      of effective lines in the first line of the file.
    */
    Int_IO_CACHE::to_chars(&file, total_line_count);
    my_b_write_byte(&file, '\n');
    for (size_t i= 0; i < size; ++i)
    {
      const Mem_fn &pm= values[i];
      if (pm)
        pm(this).save_to(&file);
      my_b_write_byte(&file, '\n');
    }
    /*
      Pad additional reserved lines:
      (1 for the line count line + line count) inclusive -> max line inclusive
       = line count exclusive <- max line inclusive
    */
    for (; total_line_count > size; --total_line_count)
      my_b_write_byte(&file, '\n');
  }

};

#endif
