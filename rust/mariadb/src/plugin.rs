//! Parent module for all plugin types

// use std::cell::UnsafeCell;

use std::ptr;
use std::{cell::UnsafeCell, ffi::c_void};

use mariadb_server_sys as bindings;
pub mod encryption;
#[doc(hidden)]
pub mod encryption_wrapper;
mod variables;
#[doc(hidden)]
pub mod wrapper;
pub use variables::{PluginVarInfo, SysVarAtomic};

/// Defines possible licenses for plugins
#[non_exhaustive]
#[derive(Clone, Copy, Debug)]
pub enum License {
    Proprietary = bindings::PLUGIN_LICENSE_PROPRIETARY as isize,
    Gpl = bindings::PLUGIN_LICENSE_GPL as isize,
    Bsd = bindings::PLUGIN_LICENSE_BSD as isize,
}

impl License {
    /// Create a license type from a string
    pub fn from_str(s: &str) -> Option<Self> {
        match s.to_lowercase().as_str() {
            "proprietary" => Some(Self::Proprietary),
            "gpl" => Some(Self::Gpl),
            "bsd" => Some(Self::Bsd),
            _ => None,
        }
    }
}

/// Defines a type of plugin. This determines the required implementation.
#[non_exhaustive]
pub enum PluginType {
    MyUdf = bindings::MYSQL_UDF_PLUGIN as isize,
    MyStorageEngine = bindings::MYSQL_STORAGE_ENGINE_PLUGIN as isize,
    MyFtParser = bindings::MYSQL_FTPARSER_PLUGIN as isize,
    MyDaemon = bindings::MYSQL_DAEMON_PLUGIN as isize,
    MyInformationSchema = bindings::MYSQL_INFORMATION_SCHEMA_PLUGIN as isize,
    MyAudit = bindings::MYSQL_AUDIT_PLUGIN as isize,
    MyReplication = bindings::MYSQL_REPLICATION_PLUGIN as isize,
    MyAuthentication = bindings::MYSQL_AUTHENTICATION_PLUGIN as isize,
    MariaPasswordValidation = bindings::MariaDB_PASSWORD_VALIDATION_PLUGIN as isize,
    MariaEncryption = bindings::MariaDB_ENCRYPTION_PLUGIN as isize,
    MariaDataType = bindings::MariaDB_DATA_TYPE_PLUGIN as isize,
    MariaFunction = bindings::MariaDB_FUNCTION_PLUGIN as isize,
}

/// Defines possible licenses for plugins
#[non_exhaustive]
pub enum Maturity {
    Unknown = bindings::MariaDB_PLUGIN_MATURITY_UNKNOWN as isize,
    Experimental = bindings::MariaDB_PLUGIN_MATURITY_EXPERIMENTAL as isize,
    Alpha = bindings::MariaDB_PLUGIN_MATURITY_ALPHA as isize,
    Beta = bindings::MariaDB_PLUGIN_MATURITY_BETA as isize,
    Gamma = bindings::MariaDB_PLUGIN_MATURITY_GAMMA as isize,
    Stable = bindings::MariaDB_PLUGIN_MATURITY_STABLE as isize,
}

pub struct InitError;

/// Initialize state
pub trait Init {
    fn init() -> Result<(), InitError> {
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        Ok(())
    }
}

/// New struct with all null values
#[doc(hidden)]
pub const fn new_null_st_maria_plugin() -> bindings::st_maria_plugin {
    bindings::st_maria_plugin {
        type_: 0,
        info: ptr::null_mut(),
        name: ptr::null(),
        author: ptr::null(),
        descr: ptr::null(),
        license: 0,
        init: None,
        deinit: None,
        version: 0,
        status_vars: ptr::null_mut(),
        system_vars: ptr::null_mut(),
        version_info: ptr::null(),
        maturity: 0,
    }
}

/// Used for plugin registrations, which are in global scope.
#[doc(hidden)]
#[derive(Debug)]
#[repr(transparent)]
pub struct UnsafeSyncCell<T>(UnsafeCell<T>);

impl<T> UnsafeSyncCell<T> {
    /// # Safety
    ///
    /// This inner value be used in a Sync/Send way
    pub const unsafe fn new(value: T) -> Self {
        Self(UnsafeCell::new(value))
    }

    pub const fn as_ptr(&self) -> *const T {
        self.0.get()
    }
}

unsafe impl<T> Send for UnsafeSyncCell<T> {}
unsafe impl<T> Sync for UnsafeSyncCell<T> {}

// #[macro_export]
// macro_rules! plugin {
//     (@def $default:expr, ) => {$default};
//     (@def $default:expr, $replace:expr) => {$replace};

//     (
//         ty: $type:expr,
//         name: $name:expr,
//         author: $author:expr,
//         description: $description:expr,
//         license: $license:expr,
//         version: $version:expr,
//         $(, version_info: $version_info:expr)?
//         maturity: $maturity:expr,
//         // Type to use init/deinit functions on, if applicable
//         $(, init: $init:ty)?
//         $(,)? // trailing comma

//     ) => {
//         use std::ffi::{c_int, c_uint};
//         use $crate::cstr;

//         // Use these intermediates to validate types
//         const ptype: PluginType = $type;
//         const ltype: License = $license
//         const vers: u32 = $version;
//         const maturity: Maturity = $maturity;

//         bindings::st_maria_plugin {
//             type_: ptype as c_int,
//             info: *mut c_void,
//             name: cstr::cstr!($name).as_ptr(),
//             author: cstr::cstr!($author).as_ptr(),
//             descr: cstr::cstr!($description).as_ptr(),
//             license: c_int,
//             init: Option<unsafe extern "C" fn(arg1: *mut c_void) -> c_int>,
//             deinit: Option<unsafe extern "C" fn(arg1: *mut c_void) -> c_int>,
//             version: vers as c_uint,
//             status_vars: *mut st_mysql_show_var,
//             system_vars: *mut *mut st_mysql_sys_var,
//             version_info: plugin!(
//                 @def
//                 cstr::cstr!($vers).as_ptr(),
//                 $(cstr::cstr!($comment).as_ptr())?
//             ),,
//             maturity: maturity as c_uint,
//         }

//     };
// }

// plugin_encryption!{
//     type: RustEncryption,
//     init: RustEncryptionInit, // optional
//     name: "example_key_management",
//     author: "MariaDB Team",
//     description: "Example key management plugin using AES",
//     license: GPL,
//     stability: EXPERIMENTAL
// }
