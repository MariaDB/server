//! "show variables" and "system variables"

use std::cell::UnsafeCell;
use std::ffi::{c_double, c_int, c_long, c_longlong, c_ulong, c_ulonglong, c_void, CStr};
use std::marker::PhantomPinned;
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_uint};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicPtr, Ordering};
use std::sync::Mutex;

use bindings::THD;
use log::trace;
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
pub trait SysVarWrap: Sync {
    /// Type for interfacing with the main server
    // type CType: SysVarCType;

    const C_FLAGS: i32;
    /// C struct
    type CStType;
    /// Type shared between `check` and `update`
    type Intermediate;

    /// For the check function, validate the arguments in `value` and write them to `dest`.
    ///
    /// - `thd`: thread pointer
    /// - `var`: pointer to the c struct
    /// - `save`: a temporary place to stash anything until it gets to update
    /// - `value`: cli value
    unsafe fn check(
        thd: *const c_void,
        var: &mut Self::CStType,
        save: &mut Self::Intermediate,
        value: &CliMysqlValue,
    ) -> c_int;
    /// Store the result of the `check` function.
    ///
    /// - `thd`: thread pointer
    /// - `var`: pointer to the c struct
    /// - `var_ptr`: where to stash the value
    /// - `save`: stash from the `check` function
    unsafe fn update(
        thd: *const c_void,
        var: &mut Self::CStType,
        var_ptr: &mut Self,
        save: &mut Self::Intermediate,
    );
}

/// Possible flags for plugin variables
#[repr(i32)]
#[non_exhaustive]
#[derive(Clone, Copy, PartialEq, Eq)]
#[allow(clippy::cast_possible_wrap)]
pub enum SysVarOpt {
    // ThdLocal = bindings::PLUGIN_VAR_THDLOCAL as i32,
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

impl SysVarOpt {
    pub const fn as_plugin_var_info(self) -> i32 {
        self as i32
    }
}

/// A string system variable
///
/// Consider this very unstable because I don't 100% understand what the SQL
/// side of things does with the malloc / const options
///
/// Bug: it seems like after updating, the SQL server cannot read the
/// variable... but we can? Do we need to do more in our `update` function?
#[repr(transparent)]
pub struct SysVarString(AtomicPtr<c_char>);

// unsafe impl Sync for SysVarString {}

impl SysVarString {
    pub const fn new() -> Self {
        Self(AtomicPtr::new(std::ptr::null_mut()))
    }

    /// Get the current value of this variable. This isn't very efficient since
    /// it copies the string, but fixes will come later
    pub fn get(&self) -> String {
        let ptr = self.0.load(Ordering::SeqCst);
        let cs = unsafe { CStr::from_ptr(ptr) };
        cs.to_str()
            .unwrap_or_else(|_| panic!("got non-UTF8 string like {}", cs.to_string_lossy()))
            .to_owned()
    }
}

pub trait SysvarWrap: Sized {
    type CStType;
    type Intermediate;
    /// Store the result of the `check` function.
    ///
    /// - `thd`: thread pointer
    /// - `var`: pointer to the c struct
    /// - `var_ptr`: where to stash the value
    /// - `save`: stash from the `check` function
    unsafe extern "C" fn update_wrap(
        thd: *mut THD,
        var: *mut bindings::st_mysql_sys_var,
        target: *mut c_void,
        save: *const c_void,
    ) {
        Self::update(&*var.cast(), &*target.cast(), &*save.cast());
    }

    unsafe fn update(var: &Self::CStType, target: &Self, save: &Self::Intermediate) {}
}

impl SysvarWrap for SysVarString {
    type CStType = bindings::sysvar_str_t;
    type Intermediate = *mut c_char;

    unsafe fn update(var: &Self::CStType, target: &Self, save: &Self::Intermediate) {
        target.0.store(*save, Ordering::SeqCst);
        trace!("updated system variable to '{}'", target.get());
    }
}
