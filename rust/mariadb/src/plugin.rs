//! Module for everything relevant to plugins
//!
//! Usage:
//!
//! ```
//! use mariadb::plugin::prelude::*;
//!
//! // May be empty or not
//! struct ExampleKeyManager;
//!
//! impl KeyManager for ExampleKeyManager {
//!     // ...
//!     # fn get_latest_key_version(key_id: u32) -> Result<u32, KeyError> { todo!() }
//!     # fn get_key(key_id: u32, key_version: u32, dst: &mut [u8]) -> Result<(), KeyError> { todo!() }
//!     # fn key_length(_key_id: u32, _key_version: u32) -> Result<usize, KeyError> { todo!() }
//! }
//!
//! impl Init for ExampleKeyManager {
//!     // ...
//!     # fn init() -> Result<(), InitError> { todo!() }
//!     # fn deinit() -> Result<(), InitError> { todo!() }
//! }
//!
//! register_plugin! {
//!     ExampleKeyManager,                           // Name of the struct implementing KeyManager
//!     ptype: PluginType::MariaEncryption,          // plugin type; only encryption supported for now
//!     name: "name_as_sql_server_sees_it",          // loadable plugin name
//!     author: "Author Name",                       // author's name
//!     description: "Sample key managment plugin",  // give a description
//!     license: License::Gpl,                       // select a license type
//!     maturity: Maturity::Experimental,            // indicate license maturity
//!     version: "0.1",                              // provide an "a.b" version
//!     init: ExampleKeyManager                      // optional: struct implementing Init if needed
//!     encryption: false,                           // false to use default encryption, true if your
//!                                                  // struct implements 'Encryption'
//! }
//! ```

use std::ffi::{c_int, c_uint};
use std::str::FromStr;

use mariadb_sys as bindings;
pub mod encryption;
#[doc(hidden)]
pub mod encryption_wrapper;
mod variables;
mod variables_parse;
#[doc(hidden)]
pub mod wrapper;
pub use mariadb_macros::register_plugin;
pub use variables::PluginVarInfo;

/// Commonly used plugin types for reexport
pub mod prelude {
    pub use super::{register_plugin, Init, InitError, License, Maturity, PluginType};
}

/// Defines possible licenses for plugins
#[non_exhaustive]
#[derive(Clone, Copy, Debug)]
#[allow(clippy::cast_possible_wrap)]
pub enum License {
    Proprietary = bindings::PLUGIN_LICENSE_PROPRIETARY as isize,
    Gpl = bindings::PLUGIN_LICENSE_GPL as isize,
    Bsd = bindings::PLUGIN_LICENSE_BSD as isize,
}

impl License {
    #[must_use]
    #[doc(hidden)]
    pub const fn to_license_registration(self) -> c_int {
        self as c_int
    }
}

#[derive(Clone, Copy, Debug)]
pub struct NoMatchingLicenseError;

impl FromStr for License {
    type Err = NoMatchingLicenseError;

    /// Create a license type from a string
    fn from_str(s: &str) -> Result<Self, NoMatchingLicenseError> {
        match s.to_lowercase().as_str() {
            "proprietary" => Ok(Self::Proprietary),
            "gpl" => Ok(Self::Gpl),
            "bsd" => Ok(Self::Bsd),
            _ => Err(NoMatchingLicenseError),
        }
    }
}

/// Defines a type of plugin. This determines the required implementation.
#[non_exhaustive]
#[allow(clippy::cast_possible_wrap)]
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
    #[must_use]
    #[doc(hidden)]
    pub const fn to_ptype_registration(self) -> c_int {
        self as c_int
    }
}

/// Defines possible licenses for plugins
#[non_exhaustive]
#[allow(clippy::cast_possible_wrap)]
pub enum Maturity {
    Unknown = bindings::MariaDB_PLUGIN_MATURITY_UNKNOWN as isize,
    Experimental = bindings::MariaDB_PLUGIN_MATURITY_EXPERIMENTAL as isize,
    Alpha = bindings::MariaDB_PLUGIN_MATURITY_ALPHA as isize,
    Beta = bindings::MariaDB_PLUGIN_MATURITY_BETA as isize,
    Gamma = bindings::MariaDB_PLUGIN_MATURITY_GAMMA as isize,
    Stable = bindings::MariaDB_PLUGIN_MATURITY_STABLE as isize,
}

impl Maturity {
    #[must_use]
    #[doc(hidden)]
    pub const fn to_maturity_registration(self) -> c_uint {
        self as c_uint
    }
}

/// Indicate that an error occured during initialization or deinitialization
pub struct InitError;

/// Implement this trait if your plugin requires init or deinit functions
pub trait Init {
    /// Initialize the plugin
    fn init() -> Result<(), InitError> {
        Ok(())
    }

    /// Deinitialize the plugin
    fn deinit() -> Result<(), InitError> {
        Ok(())
    }
}
