//! "show variables" and "system variables"

use std::cell::UnsafeCell;
use std::ffi::{c_double, c_int, c_long, c_longlong, c_ulong, c_ulonglong, c_void, CStr};
use std::marker::PhantomPinned;
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_uint};
use std::ptr;
use std::sync::atomic::{self, AtomicBool, AtomicI32, AtomicPtr, AtomicU32, Ordering};
use std::sync::Mutex;

use bindings::THD;
use log::trace;
use mariadb_sys as bindings;

use super::variables_parse::{CliMysqlValue, CliValue};

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
    // String needs memory allocation - don't expose this
    //  MemAlloc= bindings::PLUGIN_VAR_MEMALLOC,
}

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

impl SysVarOpt {
    pub const fn as_plugin_var_info(self) -> i32 {
        self as i32
    }
}

/// `bindings::mysql_var_update_func` without the `Option`
type SvUpdateFn =
    unsafe extern "C" fn(*mut THD, *mut bindings::st_mysql_sys_var, *mut c_void, *const c_void);

/// A wrapper for system variables. This won't be exposed externally.
///
/// This provides the interface of update functions
pub trait SysVarInterface: Sized {
    /// The C struct representation, e.g. `sysvar_str_t`
    type CStType;

    /// Intermediate type, pointed to by the CStType's `value` pointer
    type Intermediate;

    /// Associated const with an optional function pointer to an update
    /// function.
    ///
    /// If a sysvar type should use a custom update function, implmeent `update`
    /// and set this value to `update_wrap`.
    const UPDATE_FUNC: Option<SvUpdateFn> = None;

    /// Options to implement by default
    const DEFAULT_OPTS: i32;

    /// Wrapper for the task of storing the result of the `check` function.
    /// Simply converts to our safe rust function "update".
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
        let new_save: *const Self::Intermediate = save.cast();
        Self::update(&*target.cast(), &*var.cast(), new_save.as_ref());
    }

    /// Update function: override this if it is pointed to by `UPDATE_FUNC`
    unsafe fn update(&self, var: &Self::CStType, save: Option<&Self::Intermediate>) {
        unimplemented!()
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
pub struct SysVarConstString(AtomicPtr<c_char>);

impl SysVarConstString {
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

impl SysVarInterface for SysVarConstString {
    type CStType = bindings::sysvar_str_t;
    type Intermediate = *mut c_char;
    const DEFAULT_OPTS: i32 = bindings::PLUGIN_VAR_STR as i32;
    // const UPDATE_FUNC: Option<bindings::mysql_var_update_func> =
    //     Some(Self::update_wrap as bindings::mysql_var_update_func);

    // unsafe fn update(var: &Self::CStType, target: &Self, save: &Self::Intermediate) {
    //     target.0.store(*save, Ordering::SeqCst);
    //     trace!("updated system variable to '{}'", target.get());
    // }
}

/// Macro to easily create implementations for all the atomics
macro_rules! atomic_svinterface {
    ($atomic_ty:ty, $cs_ty:ty, $inter_ty:ty, $opts:expr) => {
        impl SysVarInterface for $atomic_ty {
            type CStType = $cs_ty;
            type Intermediate = $inter_ty;
            const DEFAULT_OPTS: i32 = ($opts) as i32;
            const UPDATE_FUNC: Option<SvUpdateFn> = Some(Self::update_wrap as SvUpdateFn);

            unsafe fn update(&self, var: &Self::CStType, save: Option<&Self::Intermediate>) {
                // based on sql_plugin.cc, seems like there are no null integers
                // (can't represent that anyway)
                let new = save.expect("somehow got a null pointer");
                self.store(*new, Ordering::SeqCst);
                trace!("updated system variable to '{}'", new);
            }
        }
    };
}

atomic_svinterface!(
    atomic::AtomicBool,
    bindings::sysvar_bool_t,
    bool,
    bindings::PLUGIN_VAR_BOOL
);
atomic_svinterface!(
    atomic::AtomicI32,
    bindings::sysvar_int_t,
    c_int,
    bindings::PLUGIN_VAR_INT
);
atomic_svinterface!(
    atomic::AtomicU32,
    bindings::sysvar_uint_t,
    c_uint,
    bindings::PLUGIN_VAR_INT | bindings::PLUGIN_VAR_UNSIGNED
);
atomic_svinterface!(
    atomic::AtomicI64,
    bindings::sysvar_longlong_t,
    c_longlong,
    bindings::PLUGIN_VAR_LONGLONG
);
atomic_svinterface!(
    atomic::AtomicU64,
    bindings::sysvar_ulonglong_t,
    c_ulonglong,
    bindings::PLUGIN_VAR_LONGLONG | bindings::PLUGIN_VAR_UNSIGNED
);
