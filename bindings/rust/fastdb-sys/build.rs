use std::env;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus};

const LINK_MODE_ENV: &str = "FASTDB_PAYLOAD_LINK_MODE";
const SYSTEM_LIB_DIR_ENV: &str = "FASTDB_PAYLOAD_SYSTEM_LIB_DIR";
const SOURCE_MARKERS: &[&str] = &[
    "fastcarto/CMakeLists.txt",
    "fastcarto/fastdb/CMakeLists.txt",
    "fastcarto/fastdb/include/fastdb_payload.h",
    "fastcarto/fastdb/src/payload",
    "fastcarto/lib/yyjson/src",
    "fastcarto/lib/double-conversion/double-conversion",
    "fastcarto/lib/picosha2",
];

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum LinkMode {
    Source,
    System,
}

fn repository_root() -> PathBuf {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(3)
        .map(Path::to_path_buf)
        .unwrap_or_else(|| packaged_source_mode_boundary());
    if SOURCE_MARKERS
        .iter()
        .all(|marker| root.join(marker).exists())
    {
        root
    } else {
        packaged_source_mode_boundary()
    }
}

fn packaged_source_mode_boundary() -> ! {
    let version = env!("CARGO_PKG_VERSION");
    panic!(
        "{LINK_MODE_ENV}=source is checkout-only; packaged consumers must use \
         {LINK_MODE_ENV}=system with {SYSTEM_LIB_DIR_ENV} set to the absolute \
         lib directory from fastdb-core-{version}-<target>.tar.gz at \
         https://github.com/world-in-progress/fastdb/releases/tag/v{version}"
    )
}

fn run(command: &mut Command, description: &str) {
    let status: ExitStatus = command
        .status()
        .unwrap_or_else(|error| panic!("failed to {description}: {error}"));
    assert!(status.success(), "{description} failed with {status}");
}

fn emit_rerun_tree(path: &Path) {
    let entries = fs::read_dir(path)
        .unwrap_or_else(|error| panic!("failed to read {}: {error}", path.display()));
    for entry in entries {
        let entry = entry.unwrap_or_else(|error| {
            panic!(
                "failed to inspect an entry under {}: {error}",
                path.display()
            )
        });
        let entry_path = entry.path();
        if entry.file_name() == OsStr::new(".git") {
            continue;
        }
        if entry_path.is_dir() {
            emit_rerun_tree(&entry_path);
        } else {
            println!("cargo:rerun-if-changed={}", entry_path.display());
        }
    }
}

fn link_mode() -> LinkMode {
    match env::var(LINK_MODE_ENV).as_deref() {
        Err(env::VarError::NotPresent) | Ok("source") => LinkMode::Source,
        Ok("system") => LinkMode::System,
        Ok(value) => {
            panic!("{LINK_MODE_ENV} must be either 'source' or 'system', received {value:?}")
        }
        Err(error) => panic!("failed to read {LINK_MODE_ENV}: {error}"),
    }
}

fn system_library_filename() -> &'static str {
    match env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("macos") | Ok("ios") => "libfastdb.dylib",
        Ok("linux") | Ok("android") | Ok("freebsd") => "libfastdb.so",
        Ok("windows") => "fastdb.lib",
        Ok(target) => panic!("system FastDB linking is unsupported for target OS {target:?}"),
        Err(error) => panic!("Cargo did not provide CARGO_CFG_TARGET_OS: {error}"),
    }
}

fn link_system() {
    let raw_directory = env::var_os(SYSTEM_LIB_DIR_ENV)
        .unwrap_or_else(|| panic!("{SYSTEM_LIB_DIR_ENV} is required in system link mode"));
    let requested = PathBuf::from(raw_directory);
    assert!(
        requested.is_absolute(),
        "{SYSTEM_LIB_DIR_ENV} must be an absolute path"
    );
    let directory = requested.canonicalize().unwrap_or_else(|error| {
        panic!(
            "failed to resolve {SYSTEM_LIB_DIR_ENV}={}: {error}",
            requested.display()
        )
    });
    assert!(
        directory.is_dir(),
        "{SYSTEM_LIB_DIR_ENV} must name a directory: {}",
        directory.display()
    );
    let library = directory.join(system_library_filename());
    assert!(
        library.is_file(),
        "system FastDB library is missing: {}",
        library.display()
    );

    println!("cargo:rerun-if-changed={}", library.display());
    println!("cargo:rustc-link-search=native={}", directory.display());
    println!("cargo:rustc-link-lib=dylib=fastdb");
}

fn link_source() {
    let root = repository_root();
    let out_dir = PathBuf::from(
        env::var_os("OUT_DIR").unwrap_or_else(|| panic!("Cargo did not provide OUT_DIR")),
    );
    let build_dir = out_dir.join("cmake");

    emit_rerun_tree(&root.join("fastcarto/fastdb/src/payload"));
    emit_rerun_tree(&root.join("fastcarto/fastdb/include"));
    emit_rerun_tree(&root.join("fastcarto/lib/yyjson/src"));
    emit_rerun_tree(&root.join("fastcarto/lib/double-conversion/double-conversion"));
    emit_rerun_tree(&root.join("fastcarto/lib/picosha2"));
    println!(
        "cargo:rerun-if-changed={}",
        root.join("fastcarto/CMakeLists.txt").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        root.join("fastcarto/fastdb/CMakeLists.txt").display()
    );
    println!(
        "cargo:rerun-if-changed={}",
        root.join("fastcarto/lib/CMakeLists.txt").display()
    );

    run(
        Command::new("cmake")
            .arg("-S")
            .arg(root.join("fastcarto"))
            .arg("-B")
            .arg(&build_dir)
            .arg("-DBUILD_TESTING=OFF")
            .arg("-DBUILD_TOOLS=OFF")
            .arg("-DUSE_SWIG_PYTHON=OFF")
            .arg("-DUSE_SWIG_NODE=OFF")
            .arg("-DUSE_SWIG_GO=OFF")
            .arg("-DCMAKE_BUILD_TYPE=Release"),
        "configure the FastDB payload Core",
    );
    let mut build = Command::new("cmake");
    build
        .arg("--build")
        .arg(&build_dir)
        .arg("--target")
        .arg("fastdb_payload_native")
        .arg("--config")
        .arg("Release")
        .arg("--parallel");
    if let Some(jobs) = env::var_os("NUM_JOBS") {
        build.arg(jobs);
    }
    run(&mut build, "build the FastDB payload Core");

    for directory in [
        build_dir.join("fastdb"),
        build_dir.join("fastdb/Release"),
        build_dir.join("lib"),
        build_dir.join("lib/Release"),
    ] {
        println!("cargo:rustc-link-search=native={}", directory.display());
    }
    println!("cargo:rustc-link-lib=static=fastdb_payload");
    println!("cargo:rustc-link-lib=static=fastdb_yyjson");
    println!("cargo:rustc-link-lib=static=fastdb_double_conversion");

    match env::var("CARGO_CFG_TARGET_ENV").as_deref() {
        Ok("msvc") => {}
        _ => match env::var("CARGO_CFG_TARGET_OS").as_deref() {
            Ok("macos") | Ok("ios") => println!("cargo:rustc-link-lib=dylib=c++"),
            _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
        },
    }
    if env::var("CARGO_CFG_TARGET_FAMILY").as_deref() == Ok("unix") {
        println!("cargo:rustc-link-lib=dylib=pthread");
    }
}

fn main() {
    println!("cargo:rerun-if-env-changed={LINK_MODE_ENV}");
    println!("cargo:rerun-if-env-changed={SYSTEM_LIB_DIR_ENV}");
    match link_mode() {
        LinkMode::Source => link_source(),
        LinkMode::System => link_system(),
    }
}
