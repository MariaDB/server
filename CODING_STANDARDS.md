# Coding Standards

This document outlines the coding standard for this repository.

It can be overwritten or amended by `CODING_STANDARDS.md` documents in
individual subdirectories.

This is a concise agent-oriented version, which highlights differences
between "default" coding style that an agent would use anyway and MariaDB.
See human-oriented version at https://mariadb.org/about/coding-style/

## Match the surrounding codebase

**This is the primary rule.** When writing code for MariaDB:

- Keep the code consistent.
- Use the same terminology, naming conventions, types, helpers, and data
  structures as the existing code in the area you are touching.
- Before writing a new utility function or container, grep `mysys/` and
  `include/` — it very likely already exists.
- Do not introduce STL containers or other constructs that duplicate
  things already in the codebase.

## Formatting

### Assignment spacing

No space before `=`, one space after — **non-standard but grep-friendly**:

```cpp
a= 1;   // correct
a = 1;  // wrong
```

### Naming

- Variables and functions: `snake_case`
- Classes and structs: `Upper_snake_case` — first letter uppercase, rest
  snake_case:

```cpp
class Buffered_logs { ... };
```

### Pointer declarator

`*` binds to the name, not the type:

```cpp
void my_function(THD *thd);
char *buf;
```

### Line length

80 characters max. When breaking a long line:
- Leave binary operators at the **end** of the broken line.
- Use `()` to guide alignment:

```cpp
rows= tab->table->file->multi_range_read_info(tab->ref.key, 10, 20,
                                              tab->ref.key_parts,
                                              &bufsz, &flags, &cost);
```

### Comments

- Single-line / inline: `//`
- Multi-line: `/*` / `*/` with 2-space indent on the body:

```cpp
/*
  This explains a non-obvious invariant.
  Second line.
*/
```

### Whitespace

- No trailing whitespace on any line.
- No trailing blank lines at end of file.
- Line endings: LF (`\n`), not CRLF.
- No tabs

## Types

Avoid `long` and `unsigned long` (and the `ulong` alias) — `long` is 32-bit
on Windows but 64-bit on Linux/macOS, creating portability issues.
Absolutely avoid them in anything user-visible, configurable variables
and command-line options, no exceptions here.

Special-purpose:
- `File` — integer file descriptor
- `my_socket` — integer socket descriptor
- `size_t` / `ptrdiff_t` — buffer sizes and pointer arithmetic

## Memory allocation

Choose the allocator by lifetime:

- **Query-scoped (short-lived):** use `MEM_ROOT` — the entire pool is freed
  at once at query end. Preferred forms:
  ```cpp
  new (thd->mem_root) T(...)
  alloc_root(mem_root, n)
  thd->strmake(str, len)
  ```
  pay attention to the `MEM_ROOT` life time.
- **Long-lived or large:** use `my_malloc` / `my_free` or plain `new` / `delete`:
  ```cpp
  my_malloc(PSI_INSTRUMENT_ME, n, MYF(MY_WME))
  ```
- **Copying bytes/strings:** `my_memdup()` / `my_strdup()` / `my_strndup()`
  instead of malloc+memcpy/strcpy.
- **Multiple objects in one call:** `my_multi_malloc()`.

## Strings

- `LEX_CSTRING` / `LEX_STRING` — a `(const char *, length)` pair; use for
  charset-less constant strings or where a charset is always the same,
  as in Lex_ident* family.
- `String` — dynamic, charset-aware string class; use when charset matters
  or the string grows.
- use `StringBuffer<N>` template.

String helper functions (prefer over stdlib equivalents):

| Use | Instead of |
|-----|-----------|
| `strnmov(dst, src)` | `strcpy` (returns end pointer, chains well) |
| `strmake(dst, src, len)` | `strncpy` (always null-terminates) |
| `strxnmov(dst, s1, s2, ..., NullS)` | manual multi-part concatenation |

## Error handling

- Functions that can fail return `bool`: **`false` = success, `true` = error.**
- Integer error codes: **0 = success, non-zero = error code**


```cpp
if (do_something(thd))   // true means error
  goto err;
```

- Report errors with `my_error(ER_CODE, MYF(0), ...)`.
- Check for a prior error with `thd->is_error()`.
- Use `goto err` with a cleanup label for failure paths.

## Data structures

Prefer these over STL equivalents:

- `List<T>` / `List_iterator<T>` — doubly-linked list
- `Dynamic_array<T>` / `DYNAMIC_ARRAY`— growable array
- `Hash<T>` / `HASH` — hash table
- `TREE` — ordered binary search tree (with built-in `MEM_ROOT` pooling)
- `Queue<T>` / `QUEUE` — priority queue
- `IO_CACHE` — buffered sequential file I/O

## Git

### Commit messages

```
MDEV-12345 exactly issue summary as in Jira

Body wrapped at 72 characters per line. Subject is normally
50 characters max, but may exceed that when it is the exact
MDEV ticket title.
```

- JIRA ticket number (if any) must be the first thing on the subject line.
- Body is rendered as Markdown.

### Target branch

- New features → `main` branch.
- Bug fixes → oldest maintained branch that reproduces the bug, but not
  older than three years since its first GA release.
