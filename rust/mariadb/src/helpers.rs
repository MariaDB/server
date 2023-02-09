use std::cell::UnsafeCell;
use std::ptr;

use super::bindings;

/// Used for plugin registrations, which are in global scope.
#[doc(hidden)]
#[derive(Debug)]
#[repr(transparent)]
pub struct UnsafeSyncCell<T>(UnsafeCell<T>);

impl<T> UnsafeSyncCell<T> {
    /// # Safety
    ///
    /// This inner value be used in a Sync/Send way
    pub const unsafe fn new(value: T) -> Self {
        Self(UnsafeCell::new(value))
    }

    pub const fn as_ptr(&self) -> *const T {
        self.0.get()
    }

    pub const fn get(&self) -> *mut T {
        self.0.get()
    }

    pub fn get_mut(&mut self) -> &mut T {
        self.0.get_mut()
    }
}

#[allow(clippy::non_send_fields_in_send_ty)]
unsafe impl<T> Send for UnsafeSyncCell<T> {}
unsafe impl<T> Sync for UnsafeSyncCell<T> {}

pub fn str2bool(s: &str) -> Option<bool> {
    const TRUE_VALS: [&str; 3] = ["on", "true", "1"];
    const FALSE_VALS: [&str; 3] = ["off", "false", "0"];
    let lower = s.to_lowercase();
    if TRUE_VALS.contains(&lower.as_str()) {
        Some(true)
    } else if FALSE_VALS.contains(&lower.as_str()) {
        Some(false)
    } else {
        None
    }
}
