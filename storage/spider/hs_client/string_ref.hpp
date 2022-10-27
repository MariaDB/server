
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_STRING_REF_HPP
#define DENA_STRING_REF_HPP

namespace dena {

struct string_wref {
  typedef char value_type;
  char *begin() const { return start; }
  char *end() const { return start + length; }
  size_t size() const { return length; }
 private:
  char *start;
  size_t length;
 public:
  string_wref(char *s = 0, size_t len = 0) : start(s), length(len) { }
};

struct string_ref {
  typedef const char value_type;
  const char *begin() const { return start; }
  const char *end() const { return start + length; }
  size_t size() const { return length; }
  void set(const char *s, size_t len) { start = s; length = len; }
  void set(const char *s, const char *f) { start = s; length = f - s; }
 private:
  const char *start;
  size_t length;
 public:
  string_ref(const char *s = 0, size_t len = 0) : start(s), length(len) { }
  string_ref(const char *s, const char *f) : start(s), length(f - s) { }
  string_ref(const string_wref& w) : start(w.begin()), length(w.size()) { }
};

template <size_t N> inline bool
operator ==(const string_ref& x, const char (& y)[N]) {
  return (x.size() == N - 1) && (::memcmp(x.begin(), y, N - 1) == 0);
}

inline bool
operator ==(const string_ref& x, const string_ref& y) {
  return (x.size() == y.size()) &&
    (::memcmp(x.begin(), y.begin(), x.size()) == 0);
}

inline bool
operator !=(const string_ref& x, const string_ref& y) {
  return (x.size() != y.size()) ||
    (::memcmp(x.begin(), y.begin(), x.size()) != 0);
}

struct string_ref_list_wrap {
  string_ref_list_wrap() {
    if (SPD_INIT_DYNAMIC_ARRAY2(&string_ref_list, sizeof(string_ref),
      NULL, 16, 16, MYF(MY_WME)))
      string_ref_list_init = FALSE;
    else
      string_ref_list_init = TRUE;
  }
  virtual ~string_ref_list_wrap() {
    if (string_ref_list_init) delete_dynamic(&string_ref_list); }
  void clear() {
    if (string_ref_list_init) string_ref_list.elements = 0; }
  void push_back(string_ref &e) {
    if (string_ref_list_init) insert_dynamic(&string_ref_list, (uchar*) &e);
    return; }
  size_t size() {
    return string_ref_list_init ? string_ref_list.elements : 0; }
  bool resize(size_t new_size) {
    if (string_ref_list_init) {
      if (string_ref_list.max_element < new_size && allocate_dynamic(
        &string_ref_list, new_size)) return TRUE;
      string_ref_list.elements = new_size;
      return FALSE;
    }
    return TRUE;
  }
  bool empty() {
    return string_ref_list_init ? string_ref_list.elements ?
      FALSE : TRUE : TRUE; }
  string_ref &operator [](size_t n) {
    return ((string_ref *) (string_ref_list.buffer +
      string_ref_list.size_of_element * n))[0]; }
  bool string_ref_list_init;
  DYNAMIC_ARRAY string_ref_list;
};

inline String *
q_append_str(String *str, const char *p) {
  uint32 p_len = strlen(p);
  if (str->reserve(p_len)) return NULL;
  str->q_append(p, p_len); return str;
}

};

#endif

