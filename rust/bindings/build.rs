//! This file runs `cmake` as needed, then `bindgen` to produce the rust
//! bindings
//!
//! Since we want to avoid configuring if possible, we try a few things in
//! order:
//!
//! - Check if cmake args are present, if so use that built output
//! - Check if the source directory root can be used, use that if so
//! - Configure it outselves, output in a temp directory

use std::collections::HashSet;
use std::env;
use std::path::PathBuf;
use std::process::Command;

use bindgen::callbacks::{DeriveInfo, MacroParsingBehavior, ParseCallbacks};
use bindgen::{BindgenError, Bindings, EnumVariation};

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

const DERIVE_COPY_NAMES: [&str; 1] = ["enum_field_types"];

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

    fn add_derives(&self, _info: &DeriveInfo<'_>) -> Vec<String> {
        if DERIVE_COPY_NAMES.contains(&_info.name) {
            vec!["Copy".to_owned()]
        } else {
            vec![]
        }
    }
}

impl BuildCallbacks {
    fn new() -> Self {
        Self(IGNORE_MACROS.into_iter().map(|s| s.to_owned()).collect())
    }
}

/// Get the root of our mariadb project
fn mariadb_root() -> String {
    format!("{}/../../", env::var("CARGO_MANIFEST_DIR").unwrap())
}

/// Find paths provided by CMake environment variables
fn include_paths_from_cmake() -> Option<[String; 2]> {
    if let Ok(src_dir) = env::var("CMAKE_SOURCE_DIR") {
        let Ok(dst_dir) = env::var("CMAKE_BINARY_DIR") else {
            panic!("CMAKE_SOURCE_DIR set but CMAKE_BINARY_DIR unset");
        };
        Some([src_dir, dst_dir])
    } else {
        None
    }
}

/// Run cmake in our temp directory
fn configure_returning_incl_paths() -> [String; 2] {
    let root = mariadb_root();
    let output_dir = format!("{}/cmake", env::var("OUT_DIR").unwrap());

    // Run cmake to configure only
    Command::new("cmake")
        .arg(format!("-S{root}"))
        .arg(format!("-B{output_dir}"))
        .output()
        .expect("failed to invoke cmake");

    [root, output_dir]
}

/// Given some include directories, see if bindgen works
fn run_bindgen_with_includes(search_paths: &[String]) -> Result<Bindings, BindgenError> {
    let incl_args = search_paths
        .iter()
        .flat_map(|path| [format!("-I{path}/sql"), format!("-I{path}/include")]);

    bindgen::Builder::default()
        // The input header we would like to generate
        // bindings for.
        .header("src/wrapper.h")
        // Fix math.h double defines
        .parse_callbacks(Box::new(BuildCallbacks::new()))
        .clang_args(incl_args)
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
        .blocklist_function("strfroml")
        .blocklist_function("strfromf64x")
        .blocklist_function("strtof64x_l")
        .blocklist_function("strtof64x")
        .blocklist_function("strtold")
        .blocklist_function("strtold_l")
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
}

fn main() {
    // Tell cargo to invalidate the built crate whenever the wrapper changes
    println!("cargo:rerun-if-changed=src/wrapper.h");

    let bindings = include_paths_from_cmake()
        .and_then(|paths| run_bindgen_with_includes(&paths).ok())
        .or_else(|| run_bindgen_with_includes(&[mariadb_root()]).ok())
        .or_else(|| run_bindgen_with_includes(&configure_returning_incl_paths()).ok())
        .expect("Unable to generate bindings");

    // Write the bindings to the $OUT_DIR/bindings.rs file.
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("couldn't write bindings");
}
