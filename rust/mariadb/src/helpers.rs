/// Pass
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
