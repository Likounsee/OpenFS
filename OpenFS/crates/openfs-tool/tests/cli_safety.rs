//! End-to-end CLI contract tests for safety-critical argument handling.

use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

fn unique_path(label: &str) -> PathBuf {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("system clock must be after UNIX epoch")
        .as_nanos();
    std::env::temp_dir().join(format!(
        "openfs-cli-{label}-{}-{nonce}.tmp",
        std::process::id()
    ))
}

fn run(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_openfs"))
        .args(args)
        .output()
        .expect("openfs binary should start")
}

#[test]
fn help_is_successful_and_documents_recovery_safety() {
    for flag in ["help", "--help", "-h"] {
        let output = run(&[flag]);
        assert!(
            output.status.success(),
            "{flag} should exit successfully: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        let help = String::from_utf8_lossy(&output.stderr);
        assert!(help.contains("create-image"), "help should list image creation");
        assert!(help.contains("recover-image"), "help should list recovery");
        assert!(
            help.contains("back it up first"),
            "help should warn that recovery may modify an image"
        );
    }
}

#[test]
fn unsupported_block_size_is_rejected_before_opening_an_image() {
    // The path intentionally does not exist: the argument error must be reported
    // before any attempt to open an image.
    let path = unique_path("missing-image");
    let output = run(&["check-image", path.to_str().unwrap(), "1234"]);
    assert!(!output.status.success());
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(
        stderr.contains("unsupported block size"),
        "expected validation error, got: {stderr}"
    );
    assert!(
        !stderr.contains("No such file") && !stderr.contains("cannot find the path"),
        "invalid geometry should be rejected before filesystem access: {stderr}"
    );
}

#[test]
fn create_image_never_overwrites_an_existing_file() {
    let path = unique_path("existing-image");
    let original = b"preserve existing bytes";
    fs::write(&path, original).expect("create sentinel file");

    let output = run(&["create-image", path.to_str().unwrap(), "1"]);
    assert!(
        !output.status.success(),
        "create-image must refuse an existing path"
    );
    assert_eq!(
        fs::read(&path).expect("read original file"),
        original,
        "refused creation must leave the existing file byte-for-byte intact"
    );

    fs::remove_file(path).expect("remove sentinel file");
}
