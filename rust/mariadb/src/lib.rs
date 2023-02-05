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

#[doc(inline)]
pub use common::*;
#[doc(hidden)]
pub use cstr;
#[doc(hidden)]
pub use mariadb_sys as bindings;

#[doc(hidden)]
#[inline]
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

/// Log an error to stderr
#[macro_export]
macro_rules! error {
    (target: $target:expr, $($msg:tt)+) => {{
        let mut tmp = "[Error] ".to_owned();
        tmp.push_str($target);
        tmp.push_str(": ");
        $crate::log_timestamped_message(&tmp, &format!($($msg)+));
    }};
    ($($msg:tt)+) => {
        $crate::log_timestamped_message("[Error]: ", &format!($($msg)+));
    }
}

/// Log a warning to stderr
#[macro_export]
macro_rules! warn {
    (target: $target:expr, $($msg:tt)+) => {{
        let mut tmp = "[Warn] ".to_owned();
        tmp.push_str($target);
        tmp.push_str(": ");
        $crate::log_timestamped_message(&tmp, &format!($($msg)+));
    }};
    ($($msg:tt)+) => {
        $crate::log_timestamped_message("[Warn]", &format!($($msg)+));
    }
}

/// Log info to stderr
#[macro_export]
macro_rules! info {
    (target: $target:expr, $($msg:tt)+) => {{
        let mut tmp = "[Info] ".to_owned();
        tmp.push_str($target);
        tmp.push_str(": ");
        $crate::log_timestamped_message(&tmp, &format!($($msg)+));
    }};
    ($($msg:tt)+) => {
        $crate::log_timestamped_message("[Info]", &format!($($msg)+));
    }
}

/// Log debug messages to stderr
#[macro_export]
macro_rules! debug {
    (target: $target:expr, $($msg:tt)+) => {{
        let mut tmp = "[Debuf] ".to_owned();
        tmp.push_str($target);
        tmp.push_str(": ");
        $crate::log_timestamped_message(&tmp, &format!($($msg)+));
    }};
    ($($msg:tt)+) => {
        $crate::log_timestamped_message("[Debuf]", &format!($($msg)+));
    }
}
