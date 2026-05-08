pub mod sample_types;

// Re-export under `types` submodule to satisfy the generated import path
// `use crate::hello::types::types::*;` — protoc-gen-solana maps the last
// package segment as a module name in cross-file imports.
pub mod types {
    pub use super::sample_types::*;
}
