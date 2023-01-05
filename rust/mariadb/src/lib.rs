//! Crate representing safe abstractions over MariaDB bindings
#![allow(unused)]

pub mod plugin;

#[doc(hidden)]
pub use mariadb_server_sys as bindings;

#[doc(hidden)]
pub use cstr;
