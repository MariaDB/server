// pub mod encryption;

use std::cell::UnsafeCell;

use mariadb_server_sys::include;
pub mod encryption;

/// Defines possible licenses for plugins
#[non_exhaustive]
#[derive(Clone, Copy, Debug)]
pub enum License {
    Proprietary = include::PLUGIN_LICENSE_PROPRIETARY as isize,
    Gpl = include::PLUGIN_LICENSE_GPL as isize,
    Bsd = include::PLUGIN_LICENSE_BSD as isize,
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
    MyUdf = include::MYSQL_UDF_PLUGIN as isize,
    MyStorageEngine = include::MYSQL_STORAGE_ENGINE_PLUGIN as isize,
    MyFtParser = include::MYSQL_FTPARSER_PLUGIN as isize,
    MyDaemon = include::MYSQL_DAEMON_PLUGIN as isize,
    MyInformationSchema = include::MYSQL_INFORMATION_SCHEMA_PLUGIN as isize,
    MyAudit = include::MYSQL_AUDIT_PLUGIN as isize,
    MyReplication = include::MYSQL_REPLICATION_PLUGIN as isize,
    MyAuthentication = include::MYSQL_AUTHENTICATION_PLUGIN as isize,
    MariaPasswordValidation = include::MariaDB_PASSWORD_VALIDATION_PLUGIN as isize,
    MariaEncryption = include::MariaDB_ENCRYPTION_PLUGIN as isize,
    MariaDataType = include::MariaDB_DATA_TYPE_PLUGIN as isize,
    MariaFunction = include::MariaDB_FUNCTION_PLUGIN as isize,
}

/// Defines possible licenses for plugins
// repr: we let this one default
#[non_exhaustive]
pub enum Maturity {
    Unknown = include::MariaDB_PLUGIN_MATURITY_UNKNOWN as isize,
    Experimental = include::MariaDB_PLUGIN_MATURITY_EXPERIMENTAL as isize,
    Alpha = include::MariaDB_PLUGIN_MATURITY_ALPHA as isize,
    Beta = include::MariaDB_PLUGIN_MATURITY_BETA as isize,
    Gamma = include::MariaDB_PLUGIN_MATURITY_GAMMA as isize,
    Stable = include::MariaDB_PLUGIN_MATURITY_STABLE as isize,
}

#[repr(transparent)]
pub struct PluginContext(pub(crate) UnsafeCell<include::st_plugin_init>);

impl PluginContext {
    pub(crate) unsafe fn from_ptr<'a>(ptr: *mut include::st_plugin_init) -> &'a Self {
        &*ptr.cast()
    }
}
