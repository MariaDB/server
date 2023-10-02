//! Utilities common to encryption plugins

#[macro_use]
pub mod file_key_mgmt;

/// Create an array of a specific size from a buffer by either zero-extending or truncating
///
/// Returns: `(array, input_is_ok, action)`
pub fn trunc_or_extend<const N: usize>(buf: &[u8]) -> ([u8; N], bool, &'static str) {
    if buf.len() == N {
        (buf.try_into().unwrap(), true, "")
    } else if buf.len() < N {
        let mut tmp = [0u8; N];
        tmp[..buf.len()].copy_from_slice(buf);
        (tmp.try_into().unwrap(), false, "Zero extending")
    } else {
        (buf[..N].try_into().unwrap(), false, "Truncating")
    }
}
