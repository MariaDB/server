//! Parent module for all plugin types

// use std::cell::UnsafeCell;

use mariadb_server_sys as bindings;
pub mod encryption;
mod encryption_wrapper;
mod variables;
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

/// Initialize state
pub trait Init {
    fn init();
}
