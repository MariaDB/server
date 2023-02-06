//! "show variables" and "system variables"

use std::cell::UnsafeCell;
use std::ffi::{c_double, c_int, c_long, c_longlong, c_ulong, c_ulonglong, c_void};
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_uint};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32};
use std::sync::Mutex;

use mariadb_sys as bindings;

use super::variables_parse::{CliMysqlValue, CliValue};

type SVInfoInner<T> = ManuallyDrop<UnsafeCell<T>>;

/// Basicallly, a system variable will be one of these types, which are dynamic
/// structures on C. Kind of yucky to work with but I think the generic union is
/// a lot more clear.
#[repr(C)]
pub union SysVarInfoU {
    bool_t: SVInfoInner<bindings::sysvar_bool_t>,
    str_t: SVInfoInner<bindings::sysvar_str_t>,
    enum_t: SVInfoInner<bindings::sysvar_enum_t>,
    set_t: SVInfoInner<bindings::sysvar_set_t>,
    int_t: SVInfoInner<bindings::sysvar_int_t>,
    long_t: SVInfoInner<bindings::sysvar_long_t>,
    longlong_t: SVInfoInner<bindings::sysvar_longlong_t>,
    uint_t: SVInfoInner<bindings::sysvar_uint_t>,
    ulong_t: SVInfoInner<bindings::sysvar_ulong_t>,
    ulonglong_t: SVInfoInner<bindings::sysvar_ulonglong_t>,
    double_t: SVInfoInner<bindings::sysvar_double_t>,
    // THD types have a function `resolve` that takes a thread pointer and an
    // offset (also a field)
}

/// Internal trait
trait SysVar: Sync {
    /// Type for interfacing with the main server
    // type CType: SysVarCType;

    const C_FLAGS: i32;
    /// C struct
    type CStType;
    /// Type shared between `check` and `update`
    type Intermediate;

    /// For the check function, validate the arguments in `value` and write them to `dest`.
    ///
    /// - thd: thread pointer
    /// - var: pointer to the c struct
    /// - save: a temporary place to stash anything until it gets to update
    /// - value: cli value
    unsafe fn check(
        thd: *const c_void,
        var: &mut Self::CStType,
        save: &mut Self::Intermediate,
        value: &CliMysqlValue,
    ) -> c_int;
    /// Store the result of the `check` function.
    ///
    /// - thd: thread pointer
    /// - var: pointer to the c struct
    /// - var_ptr: where to stash the value
    /// - save: stash from the `check` function
    unsafe fn update(
        thd: *const c_void,
        var: &mut Self::CStType,
        var_ptr: &mut Self,
        save: &mut Self::Intermediate,
    );
}

/// Intermediate type is a &String
impl SysVar for Mutex<String> {
    const C_FLAGS: i32 = (bindings::PLUGIN_VAR_STR) as i32;
    type CStType = bindings::sysvar_str_t;
    type Intermediate = Box<Option<String>>;
    unsafe fn check(
        thd: *const c_void,
        var: &mut Self::CStType,
        save: &mut Self::Intermediate,
        value: &CliMysqlValue,
    ) -> c_int {
        crate::log::warn!("entering sysvar mutex string check");
        let CliValue::String(Some(s))  = value.value() else {
            panic!("got wrong string value {:?}", value.value());
        };
        dbg!(&s);
        *save = Box::new(Some(s));
        0
    }
    unsafe fn update(
        thd: *const c_void,
        var: &mut Self::CStType,
        var_ptr: &mut Self,
        save: &mut Self::Intermediate,
    ) {
        crate::log::warn!("entering sysvar mutex string update");
        let s = save.take().unwrap();
        dbg!(&s);
        *var_ptr.lock().expect("failed to lock mutex") = s;
    }
}

impl SysVarInfoU {
    // /// Determine the version based on flags
    // fn determine_version(&self) -> &SysVarInfo<T> {
    //     // SAFETY: the flags field is always in the same position
    //     let flags = unsafe { self.basic.get().flags };

    // }
}

/// Create a system variable from an atomic
///
/// Supported types are currently `AtomicBool`, `AtomicI32`, `AtomicI64`, and
/// their unsigned versions.
#[macro_export]
macro_rules! sysvar_atomic {
    // Match the types manually
    // (@var_type bool) => {bindings::PLUGIN_VAR_BOOL};
    (@var_type i32) => {bindings::PLUGIN_VAR_INT};
    (@var_type u32) => {bindings::PLUGIN_VAR_INT | bindings::PLUGIN_VAR_UNSIGNED};
    (@var_type i64) => {bindings::PLUGIN_VAR_LONGLONG};
    (@var_type u64) => {bindings::PLUGIN_VAR_LONGLONG | bindings::PLUGIN_VAR_UNSIGNED};
    (@def $default:expr, ) => {$default};
    (@def $default:expr, $replace:expr) => {$replace};

    (
        ty: $ty:tt,
        name: $name:expr,
        var: $var:ident
        $(, comment: $comment:expr)?
        $(, flags: [$($flag:expr),+ $(,)?])?
        $(, default: $default:expr)?
        $(, minimum: $minimum:expr)?
        $(, maximum: $maximum:expr)?
        $(, multiplier: $multiplier:expr)?
        $(,)? // trailing comma
    ) => {
        {
            use ::std::ffi::{c_void, c_int, c_char};
            use ::std::mem::ManuallyDrop;
            use ::std::ptr;

            use $crate::bindings;
            use $crate::cstr;

            // Just make syntax cleaner
            type IntTy = $ty;

            unsafe extern "C" fn check_val(
                _thd: *mut bindings::THD,
                self_: *mut bindings::st_mysql_sys_var,
                save: *mut c_void,
                _mysqld_values: *mut bindings::st_mysql_value
            ) -> c_int {
                // SAFETY: caller ensures save points to a valid location of the correct type
                *save.cast() = $var.load(::std::sync::atomic::Ordering::Relaxed);
                0
            }

            unsafe extern "C" fn update_val(
                _thd: *mut bindings::THD,
                self_: *mut bindings::st_mysql_sys_var,
                var_ptr: *mut c_void,
                save: *const c_void
            ) {
                // SAFETY: `safe` points to a caller-validated value, `var_ptr` is properly sized
                *var_ptr.cast() = $var.swap(*save.cast(), ::std::sync::atomic::Ordering::Relaxed);
            }

            // Defaults
            const FLAGS: i32 = sysvar_atomic!(@var_type $ty) as i32 $( $(| ($flag as i32))+ )?;

            const REF_STRUCT: bindings::st_mysql_sys_var_simple<$ty> =
                bindings::st_mysql_sys_var_simple::<$ty> {
                flags: FLAGS,
                name: cstr::cstr!($name).as_ptr(),
                comment: sysvar_atomic!(
                    @def
                    ptr::null(),
                    $(cstr::cstr!($comment).as_ptr())?
                ),
                check: Some(check_val),
                update: Some(update_val),
                value: ptr::null_mut(),
                def_val: sysvar_atomic!(@def 0, $($default)?),
                min_val: sysvar_atomic!(@def IntTy::MIN, $($minimum)?),
                max_val: sysvar_atomic!(@def IntTy::MAX, $($maximum)?),
                blk_sz: sysvar_atomic!(@def 1, $($multiplier)?),
            };

            unsafe { SysVarAtomic::new_simple(REF_STRUCT) }
        }

    };
}

/// Possible flags for plugin variables
#[repr(i32)]
#[non_exhaustive]
#[allow(clippy::cast_possible_wrap)]
pub enum PluginVarInfo {
    ThdLocal = bindings::PLUGIN_VAR_THDLOCAL as i32,
    /// Variable is read only
    ReadOnly = bindings::PLUGIN_VAR_READONLY as i32,
    /// Variable is not a server variable
    NoSysVar = bindings::PLUGIN_VAR_NOSYSVAR as i32,
    /// No command line option
    NoCmdOpt = bindings::PLUGIN_VAR_NOCMDOPT as i32,
    /// No argument for the command line
    NoCmdArg = bindings::PLUGIN_VAR_NOCMDARG as i32,
    /// Required CLI argument
    ReqCmdArg = bindings::PLUGIN_VAR_RQCMDARG as i32,
    /// Optional CLI argument
    OptCmdArd = bindings::PLUGIN_VAR_OPCMDARG as i32,
    /// Variable is deprecated
    Deprecated = bindings::PLUGIN_VAR_DEPRECATED as i32,
    // String needs memory allocation
    //  MemAlloc= bindings::PLUGIN_VAR_MEMALLOC,
}

#[cfg(test)]
mod tests {
    use std::ffi::c_uint;
    use std::mem::size_of;
    use std::sync::atomic::AtomicU32;

    use super::*;

    #[test]
    fn test_sizes() {
        assert_eq!(size_of::<u32>(), size_of::<c_uint>())
    }

    #[test]
    fn test_macro() {
        static X: AtomicU32 = AtomicU32::new(0);
        let x = sysvar_atomic! {
            ty: u32,
            name: "sql_name",
            var: X,
        };
        // dbg!(x);
        let x = sysvar_atomic! {
            ty: u32,
            name: "sql_name",
            var: X,
            comment: "this is a comment",
            flags: [PluginVarInfo::ReadOnly],
            maximum: 40,
            multiplier: 2
        };
        // dbg!(x);
    }
}
