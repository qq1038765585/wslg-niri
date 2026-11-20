fn main() {
    println!("cargo:rustc-check-cfg=cfg(have_libinput_plugin_system)");
    if pkg_config::Config::new()
        .atleast_version("1.30.0")
        .probe("libinput")
        .is_ok()
    {
        println!("cargo:rustc-cfg=have_libinput_plugin_system")
    }

    println!("cargo:rerun-if-changed=native/freerdp_wrapper.h");
    let packages = [
        ("winpr3", Some(">= 3.32.0")),
        ("freerdp3", Some(">= 3.32.0")),
        ("freerdp-server3", Some(">= 3.32.0")),
        ("wayland-server", Some(">= 1.26.0")),
        ("openssl", Some(">= 3.0.0")),
        ("wsland", Some(">= 1.0.0")),
    ];

    let mut include_paths = Vec::new();

    for (name, version) in packages {
        let mut config = pkg_config::Config::new();

        if let Some(version) = version {
            config.atleast_version(version.trim_start_matches(">= ").trim());
        }

        let lib = config
            .probe(name)
            .unwrap_or_else(|e| panic!("找不到依赖 {name}: {e}"));

        include_paths.extend(lib.include_paths);
    }

    // Cairo 在原 Meson 配置中是可选依赖。
    if let Ok(lib) = pkg_config::Config::new().probe("cairo") {
        include_paths.extend(lib.include_paths);
    }

    let mut builder =
        bindgen::Builder::default().header("native/freerdp_wrapper.h");

    for path in include_paths {
        builder = builder.clang_arg(format!("-I{}", path.display()));
    }

    let bindings = builder
        .generate()
        .expect("生成 FreeRDP bindings 失败");

    bindings
        .write_to_file(
            std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap())
                .join("freerdp_bindings.rs"),
        )
        .expect("写入 bindings 失败");
}
