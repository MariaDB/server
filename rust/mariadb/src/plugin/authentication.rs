// st_mysql_server_auth_info
// st_mysql_auth

//!
//!
//!
//!
//!
//! # Implementation
//!
//! `st_mysql_auth` requires:
//!
//! - `interface_version`: int, set by macro
//! - `client_auth_plugin`: `char*`, indicates client's required plugin for
//!   authentication. Set by macro
//! - `authenticate_user`: function, wraps `authenticate_user`
//!
//!
//!
//!
//!
//!

use std::slice;

use crate::plugins::vio::Vio;

#[repr(transparent)]
struct AuthInfo(bindings::MYSQL_SERVER_AUTH_INFO);

enum PasswordUsage {
    NotUsed = bindings::PASSWORD_USED_NO,
    NotUsedMention = bindings::PASSWORD_USED_NO_MENTION,
    Used = bindings::PASSWORD_USED_YES,
}

impl AuthInfo {
    fn get_user_name(&self) -> &[u8] {
        // SAFETY: caller guarantees validity of self
        unsafe { slice::from_raw_parts(self.0.user_name, self.0.user_name_length as usize) }
    }

    fn get_auth_string(&self) -> &[u8] {
        // SAFETY: caller guarantees validity of self
        unsafe { slice::from_raw_parts(self.0.auth_string, self.0.auth_string_length as usize) }
    }

    fn get_authenticated_as(&self) -> &[u8] {

    }

    fn set_authenticated_as(&self, s: AsRef<[u8]>) -> Result<(), TruncatedError> {

    }

    fn set_password_usage(u: PasswordUsage) {

    }

    fn set_host(&self, s: AsRef<[u8]>) -> Result<(), TruncatedError> {

    }
}

struct AuthError;

trait Authentication { 
    fn authenticate_user(vio: &Vio, info: &AuthInfo) -> Result<bool, AuthError>;

    /// Hash the provided password and write the output to `hash`. Return the
    /// number of written bytes if successful, or `Err` if not.
    fn hash_password(password: &[u8], hash: &mut [u8]) -> Result<usize, AuthError>;

    /// Prepare the password hash for authentication
    fn preprocess_hash(hash: &[u8], out: &mut [u8]) -> Result<usize, AuthError>;
}
