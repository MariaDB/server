include!("../include.rs");

register_plugin! {
    TestPlugin,
    ptype: PluginType::MariaEncryption,
    name: "debug_key_management",
    author: "Trevor Gross",
    description: "Debug key management plugin",
    license: License::Gpl,
    maturity: Maturity::Experimental,
    version: "0.1",
    init: TestPlugin,
    encryption: false,
    extra: false
}

fn main() {}
