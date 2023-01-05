#![warn(clippy::pedantic)]
#![warn(clippy::nursery)]
#![warn(clippy::str_to_string)]
#![warn(clippy::missing_inline_in_public_items)]
#![allow(clippy::missing_const_for_fn)]
#![allow(clippy::missing_panics_doc)]
#![allow(clippy::must_use_candidate)]
#![allow(clippy::cast_possible_truncation)]

mod plugin;
use proc_macro::TokenStream;

#[proc_macro]
pub fn plugin(item: TokenStream) -> TokenStream {
    plugin::entry(item)
}
