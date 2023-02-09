//! Module for everything relevant to plugins
//!
//! Usage:
//!
//! ```
//! use mariadb::plugin::*;
//! use mariadb::plugin::encryption::*;
//! use mariadb::plugin::SysVarConstString;
//!
//! static SYSVAR_STR: SysVarConstString = SysVarConstString::new();
//!
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
//!     init: ExampleKeyManager,                     // optional: struct implementing Init if needed
//!     encryption: false,                           // false to use default encryption, true if your
//!                                                  // struct implements 'Encryption'
//!     variables: [                                 // variables should be a list of typed identifiers
//!         SysVar {
//!             ident: SYSVAR_STR,
//!             vtype: SysVarString,
//!             name: "sql_name",
//!             description: "this is a description",
//!             options: [SysVarOpt::ReadOnly, SysVarOpt::NoCmdOpt],
//!             default: "something"
//!         },
//!     //     SysVar {
//!     //         ident: OTHER_IDENT,
//!     //         vtype: AtomicI32,
//!     //         name: "other_sql_name",
//!     //         description: "this is a description",
//!     //         options: [SysVarOpt::ReqCmdArg],
//!     //         default: 100,
//!     //         min: 10,
//!     //         max: 500,
//!     //         interval: 10
//!     //     }
//!      ]
//! }
//! ```

use std::ffi::{c_int, c_uint};
use std::str::FromStr;

use mariadb_sys as bindings;
pub mod encryption;
mod encryption_wrapper;
mod variables;
mod variables_parse;
mod wrapper;
pub use mariadb_macros::register_plugin;
pub use variables::{SysVarConstString, SysVarOpt};

/// Commonly used plugin types for reexport
pub mod prelude {
    pub use super::{register_plugin, Init, InitError, License, Maturity, PluginType};
}

/// Reexports for use in proc macros
#[doc(hidden)]
pub mod internals {
    pub use super::encryption_wrapper::{WrapEncryption, WrapKeyMgr};
    pub use super::variables::SysVarInterface;
    pub use super::wrapper::{new_null_st_maria_plugin, WrapInit};
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

impl FromStr for License {
    type Err = String;

    /// Create a license type from a string
    fn from_str(s: &str) -> Result<Self, String> {
        match s.to_lowercase().as_str() {
            "proprietary" => Ok(Self::Proprietary),
            "gpl" => Ok(Self::Gpl),
            "bsd" => Ok(Self::Bsd),
            _ => Err(format!("'{s}' has no matching license type")),
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
