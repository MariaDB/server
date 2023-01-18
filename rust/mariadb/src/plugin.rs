//! Parent module for all plugin types

// use std::cell::UnsafeCell;

use std::ffi::{c_int, c_uint};

use mariadb_sys as bindings;
pub mod encryption;
#[doc(hidden)]
pub mod encryption_wrapper;
mod variables;
#[doc(hidden)]
pub mod wrapper;
pub use mariadb_macros::register_plugin;
pub use variables::{PluginVarInfo, SysVarAtomic};

/// Commonly used plugin types for reexport
pub mod prelude {
    pub use super::{register_plugin, Init, InitError, License, Maturity, PluginType};
}

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

    #[doc(hidden)]
    pub const fn to_license_registration(self) -> c_int {
        self as c_int
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
    /// Use this plugin type both for key managers and for full encryption plugins
    MariaEncryption = bindings::MariaDB_ENCRYPTION_PLUGIN as isize,
    MariaDataType = bindings::MariaDB_DATA_TYPE_PLUGIN as isize,
    MariaFunction = bindings::MariaDB_FUNCTION_PLUGIN as isize,
}

impl PluginType {
    #[doc(hidden)]
    pub const fn to_ptype_registration(self) -> c_int {
        self as c_int
    }
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

impl Maturity {
    #[doc(hidden)]
    pub const fn to_maturity_registration(self) -> c_uint {
        self as c_uint
    }
}

pub struct InitError;

/// Implement this trait if your plugin requires init or deinit functions
pub trait Init {
    fn init() -> Result<(), InitError> {
        Ok(())
    }

    fn deinit() -> Result<(), InitError> {
        Ok(())
    }
}
