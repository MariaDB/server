use crate::bindings;

#[non_exhaustive]
pub enum ClientError {
    // CommandsOutOfSync = bindings::CR_COMMANDS_OUT_OF_SYNC
    Unspecified,
}

impl From<i32> for ClientError {
    fn from(value: i32) -> Self {
        match value {
            _ => Self::Unspecified,
        }
    }
}
