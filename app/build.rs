fn main() {
    // The window paints its own surfaces, but the text fields, sliders and
    // combo boxes come from std-widgets, and those follow the style. The dark
    // one is the only one that does not fight the palette in ui/theme.slint.
    let mut config = slint_build::CompilerConfiguration::new().with_style("fluent-dark".into());

    // The tests in `window_tests` drive the real widgets, and finding an
    // element by id needs the compiler to have kept the ids. Debug builds
    // only: it is of no use in a release binary and not free there.
    if std::env::var("PROFILE").as_deref() == Ok("debug") {
        config = config.with_debug_info(true);
    }

    slint_build::compile_with_config("ui/app.slint", config).expect("slint build failed");
}
