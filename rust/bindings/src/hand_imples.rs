use std::ffi::{c_char, c_double, c_int, c_long, c_longlong, c_uint, c_ulong, c_ulonglong};

use super::{mysql_var_check_func, mysql_var_update_func, TYPELIB};

// Defined in service_encryption.h but not imported because of tilde syntax
pub const ENCRYPTION_KEY_VERSION_INVALID: c_uint = !0;

// We hand write these stucts because the definition is tricky, not all fields are
// always present

// no support for THD yet
macro_rules! declare_sysvar_type {
    (@common $name:ident: $(#[$doc:meta] $fname:ident: $fty:ty),+ $(,)*) => {
        // Common implementation
        #[repr(C)]
        #[derive(Debug)]
        pub struct $name {
            /// Variable flags
            pub flags: c_int,
            /// Name of the variable
            pub name: *const c_char,
            /// Variable description
            pub comment: *const c_char,
            /// Function for getting the variable
            pub check: mysql_var_check_func,
            /// Function for setting the variable
            pub update: mysql_var_update_func,

            // Repeated fields
            $(
                #[$doc]
                pub $fname: $fty
            ),+
        }
    };
    (basic: $name:ident, $ty:ty) => {
        // A "basic" sysvar
        declare_sysvar_type!{
            @common $name:
            #[doc = "Pointer to the value"]
            value: *mut $ty,
            #[doc = "Default value"]
            def_val: $ty,
        }
    };
    (const basic: $name:ident, $ty:ty) => {
        // A "basic" sysvar
        declare_sysvar_type!{
            @common $name:
            #[doc = "Pointer to the value"]
            value: *const $ty,
            #[doc = "Default value"]
            def_val: $ty,
        }
    };
    (simple: $name:ident, $ty:ty) => {
        // A "simple" sysvar, with minimum maximum and block size
        declare_sysvar_type!{
            @common $name:
            #[doc = "Pointer to the value"]
            value: *mut $ty,
            #[doc = "Default value"]
            def_val: $ty,
            #[doc = "Min value"]
            min_val: $ty,
            #[doc = "Max value"]
            max_val: $ty,
            #[doc = "Block size"]
            blk_sz: $ty,
        }
    };
    (typelib: $name:ident, $ty:ty) => {
        // A "typelib" sysvar
        declare_sysvar_type!{
            @common $name:
            #[doc = "Pointer to the value"]
            value: *mut $ty,
            #[doc = "Default value"]
            def_val: $ty,
            #[doc = "Typelib"]
            typelib: *const TYPELIB
        }
    };


    // (typelib: $name:ident, $ty:ty) => {

    // };
    // (thd: $name:ident, $ty:ty) => {

    // };
}

declare_sysvar_type!(basic: sysvar_bool_t, bool);
declare_sysvar_type!(basic: sysvar_str_t, *mut c_char);
declare_sysvar_type!(typelib: sysvar_enum_t, c_ulong);
declare_sysvar_type!(typelib: sysvar_set_t, c_ulonglong);
declare_sysvar_type!(simple: sysvar_int_t, c_int);
declare_sysvar_type!(simple: sysvar_long_t, c_long);
declare_sysvar_type!(simple: sysvar_longlong_t, c_longlong);
declare_sysvar_type!(simple: sysvar_uint_t, c_uint);
declare_sysvar_type!(simple: sysvar_ulong_t, c_ulong);
declare_sysvar_type!(simple: sysvar_ulonglong_t, c_ulonglong);
declare_sysvar_type!(simple: sysvar_double_t, c_double);

// declare_sysvar_type!(thdbasic: thdvar_bool_t, bool);
// declare_sysvar_type!(thdbasic: thdvar_str_t, *mut c_char);
// declare_sysvar_type!(typelib: sysvar_enum_t, c_ulong);
// declare_sysvar_type!(typelib: sysvar_set_t, c_ulonglong);

// type THDVAR_FUNC<T> = Option<unsafe extern "C" fn(thd: *const c_void, offset: c_int) -> *mut T>;
