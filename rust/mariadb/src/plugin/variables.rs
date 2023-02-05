//! "show variables" and "system variables"
//! 
//! 

use std::mem::ManuallyDrop;
use std::ptr;

use bindings::st_mysql_sys_var_basic;
use mariadb_sys as bindings;

/// Basicallly, a system variable will 
#[repr(C)]
union SysVar<T> {
    basic: ManuallyDrop<bindings::st_mysql_sys_var_basic<T>>,
    basic_const: ManuallyDrop<bindings::st_mysql_sys_var_const_basic<T>>,
    simple: ManuallyDrop<bindings::st_mysql_sys_var_simple<T>>,
    typelib: ManuallyDrop<bindings::st_mysql_sys_var_typelib<T>>,
    thdvar_basic: ManuallyDrop<bindings::st_mysql_sys_var_thd_basic<T>>,
    thdvar_simple: ManuallyDrop<bindings::st_mysql_sys_var_thd_simple<T>>,
    thdvar_typelib: ManuallyDrop<bindings::st_mysql_sys_var_thd_typelib<T>>,
}

#[repr(transparent)]
pub struct SysVarAtomic<T>(SysVar<T>);

#[doc(hidden)]
impl<T> SysVarAtomic<T> {
    pub const fn as_ptr(&self) -> *const bindings::st_mysql_sys_var {
        ptr::addr_of!(self.0).cast()
    }

    pub fn as_mut_ptr(&mut self) -> *mut bindings::st_mysql_sys_var {
        ptr::addr_of_mut!(self.0).cast()
    }

    // These functions are all unsafe because preconditions are needed to ensure
    // Send and Sync hold true
    pub const unsafe fn new_basic(val: bindings::st_mysql_sys_var_basic<T>) -> Self {
        Self(SysVar {
            basic: ManuallyDrop::new(val),
        })
    }
    pub const unsafe fn new_basic_const(val: bindings::st_mysql_sys_var_const_basic<T>) -> Self {
        Self(SysVar {
            basic_const: ManuallyDrop::new(val),
        })
    }
    pub const unsafe fn new_simple(val: bindings::st_mysql_sys_var_simple<T>) -> Self {
        Self(SysVar {
            simple: ManuallyDrop::new(val),
        })
    }
    pub const unsafe fn new_typelib(val: bindings::st_mysql_sys_var_typelib<T>) -> Self {
        Self(SysVar {
            typelib: ManuallyDrop::new(val),
        })
    }
    pub const unsafe fn new_thdvar_basic(val: bindings::st_mysql_sys_var_thd_basic<T>) -> Self {
        Self(SysVar {
            thdvar_basic: ManuallyDrop::new(val),
        })
    }
    pub const unsafe fn new_thdvar_simple(val: bindings::st_mysql_sys_var_thd_simple<T>) -> Self {
        Self(SysVar {
            thdvar_simple: ManuallyDrop::new(val),
        })
    }
    pub const unsafe fn new_thdvar_typelib(val: bindings::st_mysql_sys_var_thd_typelib<T>) -> Self {
        Self(SysVar {
            thdvar_typelib: ManuallyDrop::new(val),
        })
    }
}

// SAFETY: totally reliant on the server's calls here
unsafe impl<T> Send for SysVarAtomic<T> {}
unsafe impl<T> Sync for SysVarAtomic<T> {}

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
