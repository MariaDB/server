pub mod plugin {
    /// All fields, in expected order
    pub const ALL_FIELDS: &[&str] = &[
        "ptype",
        "name",
        "author",
        "description",
        "license",
        "maturity",
        "version",
        "init",
        "encryption",
        "variables",
    ];

    /// Always required
    pub const REQ_FIELDS: &[&str] = &[
        "ptype",
        "name",
        "author",
        "description",
        "license",
        "maturity",
        "version",
    ];

    pub const ENCR_REQ_FIELDS: &[&str] = &["encryption"];

    pub const ENCR_OPT_FIELDS: &[&str] = &["init", "sysvars"];
}

pub mod sysvar {
    /// All fields, in expected order
    pub const ALL_FIELDS: &[&str] = &[
        "ident",
        "vtype",
        "name",
        "description",
        "options",
        "default",
        "min",
        "max",
        "interval",
    ];

    /// Always required
    pub const REQ_FIELDS: &[&str] = &["ident", "vtype", "name", "description"];
    pub const OPT_FIELDS: &[&str] = &["default", "options", "min", "max", "interval"];

    // unused since we switched to full generics
    pub const _STR_REQ_FIELDS: &[&str] = &[];
    pub const _STR_OPT_FIELDS: &[&str] = &["default"];
    pub const _NUM_REQ_FIELDS: &[&str] = &[];
    pub const _NUM_OPT_FIELDS: &[&str] = &["default", "min", "max", "interval"];
}
