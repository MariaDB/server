#![warn(clippy::pedantic)]
#![warn(clippy::nursery)]
#![warn(clippy::str_to_string)]
#![warn(clippy::missing_inline_in_public_items)]
#![allow(clippy::missing_panics_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::option_if_let_else)]

mod fields;
mod helpers;
mod parse_vars;
mod register_plugin;
use proc_macro::TokenStream;

/// Macro to use to register a plugin
///
/// See the `plugin` module in the main `mariadb` crate for examples.
#[proc_macro]
pub fn register_plugin(item: TokenStream) -> TokenStream {
    register_plugin::entry(item)
}
