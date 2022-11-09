pub mod encryption;

use mariadb_server_sys::bindings;

/// Defines possible licenses for plugins
#[repr(C)]
#[non_exhaustive]
pub enum License {
    Proprietary = bindings::PLUGIN_LICENSE_PROPRIETARY,
    Gpl = bindings::PLUGIN_LICENSE_GPL,
    Bsd = bindings::PLUGIN_LICENSE_BSD
}

impl License {
    /// Helper method for creating from a string
    fn from_str(s: &str) -> Option<Self> {
        match s.lower() {
            "proprietary" => Some(Self::Proprietary)
            "gpl" => Some(Self::Gpl)
            "bsd" => Some(Self::Bsd)
            _ => None
        }
    }
}

/// Defines a type of plugin. This determines the required implementation.
#[repr(C)]
#[non_exhaustive]
pub enum PluginType {
    MyUdf = bindings::MYSQL_UDF_PLUGIN,
    MyStorageEngine = bindings::MYSQL_STORAGE_ENGINE_PLUGIN,
    MyFtParser = bindings::MYSQL_FTPARSER_PLUGIN,
    MyDaemon = bindings::MYSQL_DAEMON_PLUGIN,
    MyInformationSchema = bindings::MYSQL_INFORMATION_SCHEMA_PLUGIN,
    MyAudit = bindings::MYSQL_AUDIT_PLUGIN,
    MyReplication = bindings::MYSQL_REPLICATION_PLUGIN,
    MyAuthentication = bindings::MYSQL_AUTHENTICATION_PLUGIN,
    MariaPasswordValidation = bindings::MariaDB_PASSWORD_VALIDATION_PLUGIN,
    MariaEncryption = bindings::MariaDB_ENCRYPTION_PLUGIN,
    MariaDataType = bindings::MariaDB_DATA_TYPE_PLUGIN,
    MariaFunction = bindings::MariaDB_FUNCTION_PLUGIN,
}

/// Defines possible licenses for plugins
#[repr(C)]
#[non_exhaustive]
pub enum Maturity {
    Unknown = bindings::MariaDB_PLUGIN_MATURITY_UNKNOWN,
    Experimental = bindings::MariaDB_PLUGIN_MATURITY_EXPERIMENTAL,
    Alpha = bindings::MariaDB_PLUGIN_MATURITY_ALPHA
    Beta = bindings::MariaDB_PLUGIN_MATURITY_BETA
    Gamma = bindings::MariaDB_PLUGIN_MATURITY_GAMMA
    Stable = bindings::MariaDB_PLUGIN_MATURITY_STABLE
}
