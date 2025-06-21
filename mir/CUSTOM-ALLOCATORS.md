# Custom Allocators

In some environments, memory cannot / should not directly be managed by calls to `malloc`, `free` etc. for various reasons. To support this use case, MIR lets you provide user defined allocators. These can be supplied during context creation by calling `MIR_context_t MIR_init2 (MIR_alloc_t alloc, MIR_code_alloc_t code_alloc)`.

Calling `MIR_context_t MIR_init (void)` instead without passing custom allocators will default to using the standard functions `malloc`, `free`, ..., as well as the operating systems default routines for memory mapping and protection.

## User Guide

The following sections are intended for users of MIR as a library. If you want to contribute to MIR directly, take a look at [the developer guide](#developer-guide).

### General Purpose Allocators

`MIR_alloc` is the general purpose allocator type defined by MIR, used for most allocations. Users wishing to provide a general prupose allocator need to define the following functions:

- `void *malloc (size_t size, void *user_data)`
- `void *calloc (size_t num, size_t size, void *user_data)`
- `void *realloc (void *ptr, size_t old_size, size_t new_size, void *user_data)`
- `void free (void *ptr, void *user_data)`

These functions should follow the same semantics as the standard C functions of the same name. This includes the platform's alignment guarantees.

> [!IMPORTANT]
> The `realloc` function required by `MIR_alloc` slightly differs from its standard C counterpart in that it takes an additional parameter `old_size`, which denotes the size of the allocation `realloc` is invoked on.
> This was introduced to support allocators that do not provide `realloc` natively, as shown in [the example below](#example).
> Allocators that do support `realloc` out of the box can ignore this parameter or use it for validation purposes.

> [!IMPORTANT]
> Some allocator implementations (such as `std::pmr::(un)synchronized_pool_resource` in libstd++ / libc++) require users to provide the exact size of the allocation to calls of their deallocation function.
> This approach turns out to be largely infeasible for MIR as there are countless allocations whose size is dynamically determined, which would (in contrast to the `realloc` compromise outlined above) require a lot of additional bookkeeping on MIR's part.
> Users wishing to use such an allocator with MIR may need to implement this additional bookkeeping themselves.

Apart from the pointers and sizes one would expected, all functions additionally accept a `user_data` parameter. This can be used to pass additional context as outlined in [the example below](#example).

> [!WARNING]
> The `MIR_alloc` instance passed to `MIR_init2` must have a lifetime greater or equal to the resulting `MIR_context`, i.e. live at least as long as the subsequent call to `MIR_finish`.
> The `MIR_alloc` instance being destroyed or going out of scope beforehand may result in undefined behavior.

### Executable Code Allocators

`MIR_code_alloc` is the executable code related allocator type defined by MIR. It is used to map and unmap pages of memory, as well as manipulate their protection. Users wishing to provide an executable code allocator need to define the following functions:

- `void *(*mem_map) (size_t len, void *user_data)`: allocate and zero `len` bytes of memory (see `mmap` / `VirtualAlloc`)
- `int (*mem_unmap) (void *ptr, size_t len, void *user_data)`: free `len` bytes of memory at `ptr`, previously allocated by a call to `mem_map` (see `munmap` / `VirtualFree`)
- `int (*mem_protect) (void *ptr, size_t len, MIR_mem_protect_t prot, void *user_data)`: change the protection of memory identified by `ptr` and `len` according to the flags specified in `prot` (see `mprotect` / `VirtualProtect`)

Possible values for `prot` are contained in enum `MIR_mem_protect_t` (`PROT_READ_EXEC` and `PROT_WRITE_EXEC`).

Similar to `MIR_alloc`, `MIR_code_alloc` lets users pass `user_data` to the different functions.

MIR will not try to directly write to or execute memory returned by `mem_map`, but will instead call `mem_protect` with appropriate flags beforehand.

> [!WARNING]
> The `MIR_code_alloc` instance passed to `MIR_init2` must have a lifetime greater or equal to the resulting `MIR_context`, i.e. live at least as long as the subsequent call to `MIR_finish`.
> The `MIR_code_alloc` instance being destroyed or going out of scope beforehand may result in undefined behavior.

### Thread Safety

Users intending to use custom allocators while calling MIR functions from different threads need to ensure that their provided functions are thread safe.

### Example

This example showcases an approach to wrap a given stateful allocator interface, `my_allocator`, for use with MIR.

It uses some C++11/14 features, but can be easily adapted to work with older C++ standards.

```cpp
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mir.h"

template<typename T>
inline constexpr T align(T value, uint64_t alignment)
{
    // sadly `std::align` is only useful for very specific use cases,
    // hence we roll our own alignment routine:
    return (T) ((((uint64_t) value) + alignment - 1) & ~(alignment - 1));
}

class my_allocator
{
public:
    virtual ~my_allocator() = default;
    void *allocate(size_t size) = 0;
    void deallocate(void *ptr) = 0;
};

class context
{
public:
    context(my_allocator &allocator)
        : _allocator{allocator}
        , _mir_alloc{&context::do_malloc,
                     &context::do_calloc,
                     &context::do_realloc,
                     &context::do_free,
                     this} // user_data
        , _mir_context{MIR_init2(&_mir_alloc, nullptr)}
    {
    }

    ~context()
    {
        if (_mir_context != nullptr)
        {
            MIR_finish(_mir_context);
        }
    }

    // ...

private:
    static context &context_from_user_data(void *user_data)
    {
        return *static_cast<context *>(user_data);
    }

    static void *do_malloc(size_t size, void *user_data)
    {
        auto &self = context_from_user_data(user_data);
        return self._allocator.allocate(size);
    }

    static void *do_calloc(size_t num, size_t size, void *user_data)
    {
        auto &self = context_from_user_data(user_data);
        const size_t aligned_size = align(size, alignof(std::max_align_t));
        const size_t total_size = aligned_size * num;
        void *const ptr = self._allocator.allocate(total_size);
        std::memset(ptr, 0, total_size);
        return ptr;
    }

    static void *do_realloc(void *ptr, size_t old_size, size_t new_size, void *user_data)
    {
        auto &self = context_from_user_data(user_data);
        void *const new_ptr = self._allocator.allocate(size);
        // if the `my_alloctor` interface supports a `realloc` method natively,
        // we could simply call it here;
        // instead, for the purpose of this example, we have to rely on the size
        // of the previous allocation to be able to translate `realloc` into
        // `allocate` - `memcpy` - `deallocate`:
        std::memcpy(new_ptr, ptr, old_size);
        self._allocator.deallocate(ptr);
        return new_ptr;
    }

    static void do_free (void *ptr, void *user_data)
    {
        if (ptr == nullptr)
        {
            return;
        }
        auto &self = context_from_user_data(user_data);
        self._allocator.deallocate(ptr);
    }

private:
    my_allocator &_allocator;
    MIR_alloc _mir_alloc;
    MIR_context_t _mir_context;
};
```

## Developer Guide

The following sections are intended for contributors to MIR.

### Overview

Pointers to allocators are stored in fields `alloc` and `code_alloc` of struct `MIR_context`. These pointers are always valid, even if the user did not provide any or only some allocators explicitly (in this case, default allocators are used where needed).

Passing the executable code allocator only to `MIR_gen_init` may seem conceptually more sound, but does not seem to work in practice as the interpreter relies on some of the code generation infrastructure as well.

The vector implementation in [`mir-varr.h`](mir-varr.h) keeps an additional pointer to the allocator it was created with. While this slightly increases its memory footprint, the alternative (passing a `MIR_alloc_t` to each and every of its operations) made for a very verbose API.

### Executables shipped with MIR

Custom allocators are mostly relevant for uses of MIR as a library in some other project. In case you are working on some executable specific part of MIR, e.g. tests, you can mostly ignore custom allocators and simply call `MIR_init` instead of `MIR_init2` as before.

In case you are testing / using some of the lower level APIs that require you to explicitly pass an allocator, such as the [`VARR`](mir-varr.h) or [`HTAB`](mir-htab.h) implementations, you can include [`mir-alloc-default.c`](mir-alloc-default.c) into your translation unit and simply pass `&default_alloc` where required. The same goes for code allocators and [`mir-code-alloc-default.c`](mir-code-alloc-default.c) / `&default_code_alloc` respectively.

### MIR as a Library

Code shipped as part of the main MIR library should avoid calling standard memory management routines such as `malloc`, `free`, `mmap`, ... directly and instead use the following allocator aware replacements (located in [`mir-alloc.h`](mir-alloc.h) and [`mir-code-alloc.h`](mir-code-alloc.h) respectively):

- `void *MIR_malloc (MIR_alloc_t alloc, size_t size)`
- `void *MIR_calloc (MIR_alloc_t alloc, size_t num, size_t size)`
- `void *MIR_realloc (MIR_alloc_t alloc,  void *ptr, size_t old_size, size_t new_size)`
- `void MIR_free (MIR_alloc_t alloc, void *ptr)`
- `void *MIR_mem_map (MIR_code_alloc_t code_alloc, size_t len)`
- `int MIR_mem_unmap (MIR_code_alloc_t code_alloc, void *ptr, size_t len)`
- `int MIR_mem_protect (MIR_code_alloc_t code_alloc, void *ptr, size_t len, MIR_mem_protect_t prot)`

Suitable allocators can usually be obtained directly from the `MIR_context` (fields `alloc` and `code_alloc`), or by calling `MIR_alloc_t MIR_get_alloc (MIR_context_t ctx)`.

In case no `MIR_context` is available in a function where you require an allocator (neither directly nor indirectly through other sub-contexts such as `gen_ctx_t`), consider taking a `MIR_alloc_t` (or `MIR_code_alloc_t`) as a parameter.
