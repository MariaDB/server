/// Trait for plugins that want to use the init/deinit functions
trait Init {
    fn init() -> Self;
}
