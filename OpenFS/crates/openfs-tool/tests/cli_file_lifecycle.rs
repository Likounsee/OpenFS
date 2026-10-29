use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};

const BLOCK_SIZE: &str = "4096";

struct TestDirectory(PathBuf);

impl TestDirectory {
    fn new() -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock must be after Unix epoch")
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "openfs-cli-e2e-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir(&path).expect("create isolated test directory");
        Self(path)
    }

    fn image(&self) -> PathBuf {
        self.0.join("filesystem.openfs")
    }
}

impl Drop for TestDirectory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn invoke<I, S>(arguments: I) -> Output
where
    I: IntoIterator<Item = S>,
    S: AsRef<OsStr>,
{
    Command::new(env!("CARGO_BIN_EXE_openfs"))
        .args(arguments)
        .output()
        .expect("launch OpenFS CLI")
}

fn invoke_image(image: &Path, command: &str, arguments: &[&str]) -> Output {
    let image = image.to_str().expect("temporary image path is UTF-8");
    let mut args = vec![command, image, BLOCK_SIZE];
    args.extend_from_slice(arguments);
    invoke(args)
}

fn assert_ok(output: Output, operation: &str) -> Output {
    assert!(
        output.status.success(),
        "{operation} failed\nstdout:\n{}\nstderr:\n{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    output
}

fn run_image(image: &Path, command: &str, arguments: &[&str]) -> Output {
    assert_ok(
        invoke_image(image, command, arguments),
        &format!("{command} {arguments:?}"),
    )
}

#[test]
fn cli_file_lifecycle_survives_close_and_reopen() {
    let directory = TestDirectory::new();
    let image = directory.image();
    let image_arg = image.to_str().unwrap();

    assert_ok(
        invoke(["create-image", image_arg, "32"]),
        "create-image",
    );
    run_image(&image, "init-namespace", &[]);
    run_image(&image, "mkdir-p", &["/docs/deep"]);
    run_image(&image, "touch", &["/docs/deep/payload.bin"]);

    // More than two filesystem blocks: this exercises extent allocation and
    // streaming reads/writes rather than only tiny metadata-sized files.
    let original = "0123456789abcdef".repeat(1024);
    run_image(&image, "write", &["/docs/deep/payload.bin", &original]);
    let read_back = run_image(&image, "cat", &["/docs/deep/payload.bin"]);
    assert_eq!(read_back.stdout, original.as_bytes());

    // Modify bytes across a block boundary, then append without rewriting the
    // complete file. Each command opens and closes the image independently.
    let replacement = "CROSS-BLOCK";
    run_image(
        &image,
        "write-range",
        &["/docs/deep/payload.bin", "4090", replacement],
    );
    let mut expected = original.into_bytes();
    expected[4090..4090 + replacement.len()].copy_from_slice(replacement.as_bytes());
    run_image(&image, "append", &["/docs/deep/payload.bin", "TAIL"]);
    expected.extend_from_slice(b"TAIL");

    let read_back = run_image(&image, "cat", &["/docs/deep/payload.bin"]);
    assert_eq!(read_back.stdout, expected);

    run_image(&image, "touch", &["/docs/copied.bin"]);
    run_image(
        &image,
        "copy",
        &["/docs/deep/payload.bin", "/docs/copied.bin"],
    );
    run_image(
        &image,
        "mv",
        &["/docs/copied.bin", "/docs/deep/moved.bin"],
    );
    let moved = run_image(&image, "cat", &["/docs/deep/moved.bin"]);
    assert_eq!(moved.stdout, expected);

    let stat = run_image(&image, "stat", &["/docs/deep/moved.bin"]);
    assert!(
        String::from_utf8_lossy(&stat.stdout).contains(&format!("Size: {} bytes", expected.len())),
        "stat did not report the expected file size: {}",
        String::from_utf8_lossy(&stat.stdout)
    );

    run_image(&image, "truncate", &["/docs/deep/moved.bin", "4096"]);
    let truncated = run_image(&image, "cat", &["/docs/deep/moved.bin"]);
    assert_eq!(truncated.stdout, expected[..4096]);

    // A fresh process reopening the image must reconstruct the durable namespace.
    let tree = run_image(&image, "tree", &["/docs"]);
    let tree = String::from_utf8_lossy(&tree.stdout);
    assert!(tree.contains("deep/"), "{tree}");
    assert!(tree.contains("moved.bin"), "{tree}");
    assert!(tree.contains("payload.bin"), "{tree}");

    run_image(&image, "rm", &["/docs/deep/payload.bin"]);
    let listing = run_image(&image, "ls", &["/docs/deep"]);
    let listing = String::from_utf8_lossy(&listing.stdout);
    assert!(!listing.contains("payload.bin"), "{listing}");
    assert!(listing.contains("moved.bin"), "{listing}");

    run_image(&image, "check-image", &[]);
}

#[test]
fn create_image_never_truncates_an_existing_file() {
    let directory = TestDirectory::new();
    let image = directory.image();
    let sentinel = b"do not overwrite this file";
    fs::write(&image, sentinel).expect("write sentinel");

    let output = invoke([
        "create-image",
        image.to_str().expect("temporary path is UTF-8"),
        "32",
    ]);
    assert!(!output.status.success(), "create-image unexpectedly replaced an existing file");
    assert_eq!(fs::read(&image).expect("read sentinel"), sentinel);
}
