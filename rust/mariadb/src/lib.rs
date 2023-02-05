//! Crate representing safe abstractions over MariaDB bindings
#![warn(clippy::pedantic)]
#![warn(clippy::nursery)]
#![warn(clippy::missing_inline_in_public_items)]
#![warn(clippy::str_to_string)]
#![allow(clippy::missing_errors_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::useless_conversion)]
#![allow(clippy::missing_const_for_fn)]
#![allow(clippy::module_name_repetitions)]
#![allow(clippy::missing_inline_in_public_items)]
#![allow(unused)]

use time::{format_description, OffsetDateTime};

mod common;
pub mod plugin;
pub mod service_sql;
use std::fmt::Write;

#[doc(inline)]
pub use common::*;
#[doc(hidden)]
pub use cstr;
pub use log;
#[doc(hidden)]
pub use mariadb_sys as bindings;

#[inline]
#[doc(hidden)]
pub fn log_timestamped_message(title: &str, msg: &str) {
    let t = time::OffsetDateTime::now_utc();
    let fmt = time::format_description::parse(
        "[year]-[month]-[day] [hour]:[minute]:[second][offset_hour sign:mandatory]:[offset_minute]",
    )
    .unwrap();
    let mut to_print = t.format(&fmt).unwrap();
    to_print.push(' ');
    to_print.push_str(title);
    to_print.push_str(msg);
    eprintln!("{to_print}");
}

#[doc(hidden)]
pub struct MariaLogger {
    pkg: Option<&'static str>,
}

impl MariaLogger {
    pub const fn new() -> Self {
        Self {
            pkg: option_env!("CARGO_PKG_NAME"),
        }
    }
}

impl log::Log for MariaLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        // metadata.level() <= log::Level::Info
        true
    }

    fn log(&self, record: &log::Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let t = time::OffsetDateTime::now_utc();
        let fmt = time::format_description::parse(
            "[year]-[month]-[day] [hour]:[minute]:[second][offset_hour sign:mandatory]:[offset_minute]",
        )
        .unwrap();
        let mut out_str = t.format(&fmt).unwrap();
        write!(out_str, " [{}]", record.level()).unwrap();
        if let Some(pkg) = self.pkg {
            write!(out_str, " {pkg}");
        }
        eprintln!("{out_str}:");
    }

    fn flush(&self) {}
}

/// Configure t
#[macro_export]
macro_rules! configure_logger {
    () => {
        $crate::configure_logger!(log::LevelFilter::Warn)
    };
    ($level:expr) => {{
        static LOGGER = $crate::MariaLogger::new();
        $crate::log::set_logger(&LOGGER).map(|()| log::set_max_level($level))
    }}
}

/// Provide the name of the calling function (full path)
macro_rules! function {
    () => {{
        fn f() {}
        fn type_name_of<T>(_: T) -> &'static str {
            std::any::type_name::<T>()
        }
        let name = type_name_of(f);
        &name[..name.len() - 3]
    }};
}
