//! This file runs `cmake` as needed, then `bindgen` to produce the rust bindings

use bindgen::callbacks::{MacroParsingBehavior, ParseCallbacks};
use bindgen::EnumVariation;
use std::collections::HashSet;
use std::env;
use std::path::PathBuf;
use std::process::Command;

// `math.h` seems to double define some things, To avoid this, we ignore them.
const IGNORE_MACROS: [&str; 20] = [
    "FE_DIVBYZERO",
    "FE_DOWNWARD",
    "FE_INEXACT",
    "FE_INVALID",
    "FE_OVERFLOW",
    "FE_TONEAREST",
    "FE_TOWARDZERO",
    "FE_UNDERFLOW",
    "FE_UPWARD",
    "FP_INFINITE",
    "FP_INT_DOWNWARD",
    "FP_INT_TONEAREST",
    "FP_INT_TONEARESTFROMZERO",
    "FP_INT_TOWARDZERO",
    "FP_INT_UPWARD",
    "FP_NAN",
    "FP_NORMAL",
    "FP_SUBNORMAL",
    "FP_ZERO",
    "IPPORT_RESERVED",
];

#[derive(Debug)]
struct BuildCallbacks(HashSet<String>);

impl ParseCallbacks for BuildCallbacks {
    /// Ignore macros that are in the ignored list
    fn will_parse_macro(&self, name: &str) -> MacroParsingBehavior {
        if self.0.contains(name) {
            MacroParsingBehavior::Ignore
        } else {
            MacroParsingBehavior::Default
        }
    }

    /// Use a converter to turn doxygen comments into rustdoc
    fn process_comment(&self, comment: &str) -> Option<String> {
        Some(doxygen_rs::transform(comment))
    }
}

impl BuildCallbacks {
    fn new() -> Self {
        Self(IGNORE_MACROS.into_iter().map(|s| s.to_owned()).collect())
    }
}

fn main() {
    // Tell cargo to invalidate the built crate whenever the wrapper changes
    println!("cargo:rerun-if-changed=wrapper.h");

    // Run cmake to configure only
    Command::new("cmake")
        .args(["../../", "-B../../"])
        .output()
        .expect("failed to invoke cmake");

    // The bindgen::Builder is the main entry point
    // to bindgen, and lets you build up options for
    // the resulting bindings.
    let bindings = bindgen::Builder::default()
        // The input header we would like to generate
        // bindings for.
        .header("src/wrapper.h")
        // Fix math.h double defines
        .parse_callbacks(Box::new(BuildCallbacks::new()))
        .clang_arg("-I../../include")
        .clang_arg("-I../../sql")
        .clang_arg("-xc++")
        .clang_arg("-std=c++17")
        // Don't derive copy for structs
        .derive_copy(false)
        // Use rust-style enums labeled with non_exhaustive to represent C enums
        .default_enum_style(EnumVariation::Rust {
            non_exhaustive: true,
        })
        // LLVM has some issues with long dobule and ABI compatibility
        // disabling the only relevant function here to suppress errors
        .blocklist_function("strtold")
        // qvct, evct, qfcvt_r, ...
        .blocklist_function("[a-z]{1,2}cvt(?:_r)?")
        // c++ things that aren't supported
        .blocklist_item("List_iterator")
        .blocklist_type("std::char_traits")
        .opaque_type("std_.*")
        .blocklist_item("std_basic_string")
        .blocklist_item("std_collate.*")
        .blocklist_item("__gnu_cxx.*")
        // Finish the builder and generate the bindings.
        .generate()
        // Unwrap the Result and panic on failure.
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("couldn't write bindings");
}
