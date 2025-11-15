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

#ifndef RPL_INFO_FILE_HH
#define RPL_INFO_FILE_HH

#include <cstdint>     // uintN_t
#include <charconv>    // std::from/to_chars and other helpers
#include <functional>  // superclass of Info_file::Mem_fn
#include <my_sys.h>    // IO_CACHE, FN_REFLEN, ...


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
    std::from_chars_result result= std::from_chars(buf, &buf[length], value);
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
    my_b_write(file, (const uchar *)buf, result.ptr - buf);
  }
};


/**
  This common superclass of @ref Master_info_file and
  @ref Relay_log_info_file provides them common code for saving
  and loading fields in their MySQL line-based sections.
  As only the @ref Master_info_file has a MariaDB `key=value`
  section with a mix of explicit and `DEFAULT`-able fields,
  code for those are in @ref Master_info_file instead.

  Each field is an instance of an implementation
  of the @ref Info_file::Persistent interface.
  C++ templates enables code reuse for those implementation structs, but
  templates are not suitable for the conventional header/implementation split.
  Thus, this and derived files are header-only units (methods are `inline`).
  Other files may include these files directly,
  though headers should include this set under their `#include` guards.
  [C++20 modules](https://en.cppreference.com/w/cpp/language/modules.html)
  can supercede headers and their `#include` guards.
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

  /** Integer Field
    @tparam I signed or unsigned integer type
    @see Master_info_file::Optional_int_field
      version with `DEFAULT` (not a subclass)
  */
  template<typename I> struct Int_field: Persistent
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

  /// Null-Terminated String (usually file name) Field
  template<size_t size= FN_REFLEN> struct String_field: Persistent
  {
    char buf[size];
    virtual operator const char *() { return buf; }
    /// @param other not `nullptr`
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
      my_b_write(file, (const uchar *)buf, strlen(buf));
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
    using List= const std::initializer_list<Mem_fn>;
    /// Null Constructor
    Mem_fn(nullptr_t null= nullptr):
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
    @param fields
      List of wrapped member pointers to fields. The first element must be a
      file name @ref String_field to be unambiguous with the line count line.
    @param default_lines
      We cannot simply read lines until EOF as all versions
      of MySQL/MariaDB may generate more lines than needed.
      Therefore, starting with MySQL/MariaDB 4.1.x for @ref Master_info_file and
      5.6.x for @ref Relay_log_info_file, the first line of the file is number
      of one-line-per-field lines in the file, including this line count itself.
      This parameter specifies the number of effective lines before those
      versions (i.e., not counting the line count line if it was to have one),
      where the first line is a filename with extension
      (either contains a `.` or is entirely empty) rather than an integer.
    @return `false` if the file has parsed successfully or `true` if error
  */
  bool load_from_file(Mem_fn::List fields, size_t default_lines)
  {
    /**
      The first row is temporarily stored in the first field. If it is a line
      count and not a log name (new format), the second row will overwrite it.
    */
    auto &field1= dynamic_cast<String_field<> &>((*(fields.begin()))(this));
    if (field1.load_from(&file))
      return true;
    size_t lines;
    std::from_chars_result result= std::from_chars(
      field1.buf, &field1.buf[sizeof(field1.buf)], lines);
    // Skip the first field in the for loop if that line was not a line count.
    size_t i= result.ec != Int_IO_CACHE::ERRC_OK || *(result.ptr) != '\0';
    /**
      Set the default after parsing: While std::from_chars() does not replace
      the output if it failed, it does replace if the line is not fully spent.
    */
    if (i)
      lines= default_lines;
    for (; i < lines; ++i)
    {
      int c;
      if (i < fields.size()) // line known in the ` list
      {
        const Mem_fn &pm= fields.begin()[i];
        if (static_cast<bool>(pm))
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

  /**
    Flush the MySQL line-based section to the @ref file
    @param fields List of wrapped member pointers to fields.
    @param lines
      The number of lines to describe the file as on the first line of the file.
      If this is larger than `fields.size()`, suffix the file with empty lines
      until the "field" count (including the line count line) is this many.
      This reservation provides some compatibility
      should MySQL adds more line-based fields.
  */
  void save_to_file(Mem_fn::List fields, size_t lines)
  {
    DBUG_ASSERT(lines >= fields.size());
    my_b_seek(&file, 0);
    /*
      If the new contents take less space than the previous file contents,
      then this code would write the file with unerased trailing garbage lines.
      But these garbage don't matter thanks to the number
      of effective lines in the first line of the file.
    */
    Int_IO_CACHE::to_chars(&file, lines);
    my_b_write_byte(&file, '\n');
    for (const Mem_fn &pm: fields)
    {
      if (static_cast<bool>(pm))
        pm(this).save_to(&file);
      my_b_write_byte(&file, '\n');
    }
    /*
      Pad additional reserved lines:
      (1 for the line count line + field count) inclusive -> max line inclusive
       = field count exclusive <- max line inclusive
    */
    for (; lines > fields.size(); --lines)
      my_b_write_byte(&file, '\n');
  }

};

#endif
