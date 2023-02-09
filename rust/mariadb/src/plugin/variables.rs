//! "show variables" and "system variables"

use std::cell::UnsafeCell;
use std::ffi::{c_double, c_int, c_long, c_longlong, c_ulong, c_ulonglong, c_void, CStr, CString};
use std::marker::PhantomPinned;
use std::mem::ManuallyDrop;
use std::os::raw::{c_char, c_uint};
use std::ptr;
use std::sync::atomic::{self, AtomicBool, AtomicI32, AtomicPtr, AtomicU32, Ordering};
use std::sync::Mutex;

use bindings::THD;
use cstr::cstr;
use log::{trace, warn};
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
/// This provides the interface of update functions. Trait is unsafe because
/// using the wrong structures would cause UB.
pub unsafe trait SysVarInterface: Sized {
    /// The C struct representation, e.g. `sysvar_str_t`
    type CStructType;

    /// Intermediate type, pointed to by the CStructType's `value` pointer
    type Intermediate;

    /// Associated const with an optional function pointer to an update
    /// function.
    ///
    /// If a sysvar type should use a custom update function, implmeent `update`
    /// and set this value to `update_wrap`.
    const UPDATE_FUNC: Option<SvUpdateFn> = None;

    /// Options to implement by default
    const DEFAULT_OPTS: i32;

    /// C struct filled with default values.
    const DEFAULT_C_STRUCT: Self::CStructType;

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
    unsafe fn update(&self, var: &Self::CStructType, save: Option<&Self::Intermediate>) {
        unimplemented!()
    }
}

/// A const string system variable
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

unsafe impl SysVarInterface for SysVarConstString {
    type CStructType = bindings::sysvar_str_t;
    type Intermediate = *mut c_char;
    const DEFAULT_OPTS: i32 = bindings::PLUGIN_VAR_STR as i32;
    const DEFAULT_C_STRUCT: Self::CStructType = Self::CStructType {
        flags: 0,
        name: ptr::null(),
        comment: ptr::null(),
        check: None,
        update: None,
        value: ptr::null_mut(),
        def_val: cstr!("").as_ptr().cast_mut(),
    };
}

/// An editable c string
///
/// Note on race conditions:
///
/// There is a race if the C side reads data while being updated on the Rust
/// side. No worse than what would exist if the plugin was written in C, but
/// important to note it does exist.
#[repr(C)]
pub struct SysVarString {
    /// This points to our c string
    ptr: AtomicPtr<c_char>,
    mutex: Mutex<Option<CString>>,
}

impl SysVarString {
    pub const fn new() -> Self {
        Self {
            ptr: AtomicPtr::new(std::ptr::null_mut()),
            mutex: Mutex::new(None),
        }
    }

    /// Get the current value of this variable
    pub fn get(&self) -> Option<String> {
        let guard = &*self.mutex.lock().expect("failed to lock mutex");
        let ptr = self.ptr.load(Ordering::SeqCst);

        if !ptr.is_null() && guard.is_some() {
            let cs = guard.as_ref().unwrap();
            assert!(
                ptr.cast_const() == cs.as_ptr(),
                "pointer and c string unsynchronized"
            );
            Some(cstr_to_string(&cs))
        } else if ptr.is_null() && guard.is_none() {
            None
        } else {
            warn!("pointer {ptr:p} mismatch with guard {guard:?}");
            // prefer the pointer, must have been set on the C side
            let cs = unsafe { CStr::from_ptr(ptr) };
            Some(cstr_to_string(cs))
        }
    }
}

unsafe impl SysVarInterface for SysVarString {
    type CStructType = bindings::sysvar_str_t;
    type Intermediate = *mut c_char;
    const UPDATE_FUNC: Option<SvUpdateFn> = Some(Self::update_wrap);
    const DEFAULT_OPTS: i32 = bindings::PLUGIN_VAR_STR as i32;
    const DEFAULT_C_STRUCT: Self::CStructType = Self::CStructType {
        flags: 0,
        name: ptr::null(),
        comment: ptr::null(),
        check: None,
        update: Some(Self::update_wrap),
        value: ptr::null_mut(),
        def_val: cstr!("").as_ptr().cast_mut(),
    };

    unsafe fn update(&self, var: &Self::CStructType, save: Option<&Self::Intermediate>) {
        let to_save = save.map(|ptr| unsafe { CStr::from_ptr(*ptr).to_owned() });
        let guard = &mut *self.mutex.lock().expect("failed to lock mutex");
        *guard = to_save;
        let new_ptr = guard
            .as_deref()
            .map_or(ptr::null_mut(), |cs| cs.as_ptr().cast_mut());
        self.ptr.store(new_ptr, Ordering::SeqCst);
    }
}

fn cstr_to_string(cs: &CStr) -> String {
    cs.to_str()
        .unwrap_or_else(|_| panic!("got non-UTF8 string like {}", cs.to_string_lossy()))
        .to_owned()
}

/// Macro to easily create implementations for all the atomics
macro_rules! atomic_svinterface {
    // Special case for boolean, which doesn't have as many fields
    (   $atomic_type:ty,
        $c_struct_type:ty,
        bool,
        $default_options:expr $(,)?
    ) => {
        atomic_svinterface!{
            $atomic_type,
            $c_struct_type,
            bool,
            $default_options,
            { def_val: false }
        }
    };

    // All other integer types have the same fields
    (   $atomic_type:ty,
        $c_struct_type:ty,
        $inter_type:ty,
        $default_options:expr $(,)?
    ) => {
        atomic_svinterface!{
            $atomic_type,
            $c_struct_type,
            $inter_type,
            $default_options,
            { def_val: 0, min_val: <$inter_type>::MIN, max_val: <$inter_type>::MAX, blk_sz: 0 }
        }
    };

    // Full generic implementation
    (   $atomic_type:ty,                // e.g., AtomicI32
        $c_struct_type:ty,              // e.g. sysvar_int_t
        $inter_type:ty,                 // e.g. i32
        $default_options:expr,          // e.g. PLUGIN_VAR_INT
        { $( $extra_struct_fields:tt )* } $(,)?  // e.g. default, min, max fields
    ) => {
        unsafe impl SysVarInterface for $atomic_type {
            type CStructType = $c_struct_type;
            type Intermediate = $inter_type;
            const DEFAULT_OPTS: i32 = ($default_options) as i32;
            const UPDATE_FUNC: Option<SvUpdateFn> = Some(Self::update_wrap as SvUpdateFn);
            const DEFAULT_C_STRUCT: Self::CStructType = Self::CStructType {
                flags: 0,
                name: ptr::null(),
                comment: ptr::null(),
                check: None,
                update: None,
                value: ptr::null_mut(),
                $( $extra_struct_fields )*
            };

            unsafe fn update(&self, var: &Self::CStructType, save: Option<&Self::Intermediate>) {
                trace!(
                    "updated {} system variable to '{:?}'",
                    std::any::type_name::<$atomic_type>(), save
                );
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
    bindings::PLUGIN_VAR_BOOL,
);
atomic_svinterface!(
    atomic::AtomicI32,
    bindings::sysvar_int_t,
    c_int,
    bindings::PLUGIN_VAR_INT,
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
