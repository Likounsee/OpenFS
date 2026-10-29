//! Bounded block-device abstractions and a file-backed development image.

use fs2::FileExt;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

use openfs_core::{check_block_range, Error, Result};

/// Minimum and maximum supported block sizes for image devices.
pub const MIN_BLOCK_SIZE: u32 = 512;
pub const MAX_BLOCK_SIZE: u32 = 65_536;

/// Minimal mutable block device contract used by early OpenFS components.
pub trait BlockDevice {
    /// Device block size in bytes.
    fn block_size(&self) -> u32;
    /// Number of addressable blocks.
    fn block_count(&self) -> u64;
    /// Read complete blocks into a buffer whose length is a block-size multiple.
    fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()>;
    /// Write complete blocks from a buffer whose length is a block-size multiple.
    fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()>;
    /// Request host-side synchronization of pending file contents.
    fn flush(&mut self) -> Result<()>;
}

/// Operation type used by the deterministic test-only I/O fault injector.
#[cfg(any(test, feature = "test-support"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoOperation {
    Read,
    Write,
    Flush,
}

/// A one-shot fault injector for filesystem tests.
///
/// It can return operation-boundary I/O errors or model a torn write by persisting
/// a prefix of new bytes while retaining the old suffix. The torn-write mode writes
/// complete blocks containing that mixed state, then reports an I/O error. It does
/// not emulate physical-device firmware or ordering barriers.
#[cfg(any(test, feature = "test-support"))]
pub struct FaultInjectingDevice<D> {
    inner: D,
    next_failure: Option<FaultAction>,
}

#[cfg(any(test, feature = "test-support"))]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum FaultAction {
    Fail(IoOperation),
    FailAfter {
        operation: IoOperation,
        remaining: usize,
    },
    TornWrite {
        prefix_bytes: usize,
    },
}

#[cfg(any(test, feature = "test-support"))]
impl<D> FaultInjectingDevice<D> {
    pub fn new(inner: D) -> Self {
        Self {
            inner,
            next_failure: None,
        }
    }

    pub fn fail_next(&mut self, operation: IoOperation) {
        self.next_failure = Some(FaultAction::Fail(operation));
    }

    /// Fail after this many successful occurrences of the selected operation.
    ///
    /// For example, `fail_after(IoOperation::Write, 3)` lets three writes pass
    /// and fails the fourth. This allows recovery tests to target failures after
    /// a journal COMMIT has been written, rather than only failing the first write.
    pub fn fail_after(&mut self, operation: IoOperation, successful_matches: usize) {
        self.next_failure = Some(FaultAction::FailAfter {
            operation,
            remaining: successful_matches,
        });
    }

    /// Make the next write persist only this prefix of the new bytes, then fail.
    ///
    /// The prefix must be shorter than the supplied write buffer. The unmodified
    /// suffix is read from the device first, making the mixed block state repeatable.
    pub fn tear_next_write_after(&mut self, prefix_bytes: usize) {
        self.next_failure = Some(FaultAction::TornWrite { prefix_bytes });
    }

    pub fn into_inner(self) -> D {
        self.inner
    }

    fn check_failure(&mut self, operation: IoOperation) -> Result<()> {
        match self.next_failure {
            Some(FaultAction::Fail(expected)) if expected == operation => {
                self.next_failure = None;
                Err(Error::Io(std::io::Error::other(
                    "injected OpenFS block-device failure",
                )))
            }
            Some(FaultAction::FailAfter {
                operation: expected,
                remaining,
            }) if expected == operation => {
                if remaining == 0 {
                    self.next_failure = None;
                    Err(Error::Io(std::io::Error::other(
                        "injected OpenFS block-device failure",
                    )))
                } else {
                    self.next_failure = Some(FaultAction::FailAfter {
                        operation: expected,
                        remaining: remaining - 1,
                    });
                    Ok(())
                }
            }
            _ => Ok(()),
        }
    }
}

#[cfg(any(test, feature = "test-support"))]
impl<D: BlockDevice> BlockDevice for FaultInjectingDevice<D> {
    fn block_size(&self) -> u32 {
        self.inner.block_size()
    }

    fn block_count(&self) -> u64 {
        self.inner.block_count()
    }

    fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
        self.check_failure(IoOperation::Read)?;
        self.inner.read_blocks(first_block, output)
    }

    fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
        if let Some(FaultAction::TornWrite { prefix_bytes }) = self.next_failure {
            self.next_failure = None;
            let block_size =
                usize::try_from(self.inner.block_size()).map_err(|_| Error::ArithmeticOverflow)?;
            if block_size == 0
                || input.is_empty()
                || input.len() % block_size != 0
                || prefix_bytes >= input.len()
            {
                return Err(Error::InvalidInput(
                    "torn-write prefix must be shorter than a non-empty block-aligned buffer",
                ));
            }
            let mut mixed = Vec::new();
            mixed.try_reserve_exact(input.len()).map_err(|_| {
                Error::AllocationFailed("unable to allocate torn-write fault-injection buffer")
            })?;
            mixed.resize(input.len(), 0);
            self.inner.read_blocks(first_block, &mut mixed)?;
            mixed[..prefix_bytes].copy_from_slice(&input[..prefix_bytes]);
            self.inner.write_blocks(first_block, &mixed)?;
            return Err(Error::Io(std::io::Error::other(
                "injected torn OpenFS block-device write",
            )));
        }
        self.check_failure(IoOperation::Write)?;
        self.inner.write_blocks(first_block, input)
    }

    fn flush(&mut self) -> Result<()> {
        self.check_failure(IoOperation::Flush)?;
        self.inner.flush()
    }
}

/// A regular file interpreted as a fixed-size block device.
///
/// flush calls File::sync_all. This requests synchronization through the host
/// operating system; it cannot promise that every controller or physical device has
/// honored a hardware-level persistence guarantee.
pub struct ImageDevice {
    file: File,
    block_size: u32,
    block_count: u64,
}

impl ImageDevice {
    /// Create a new image file without overwriting an existing path.
    pub fn create(path: impl AsRef<Path>, block_size: u32, block_count: u64) -> Result<Self> {
        Self::create_internal(path, block_size, block_count, false)
    }

    /// Explicitly create or truncate an image file.
    ///
    /// This operation destroys the previous contents if the path already exists.
    pub fn create_truncate(
        path: impl AsRef<Path>,
        block_size: u32,
        block_count: u64,
    ) -> Result<Self> {
        Self::create_internal(path, block_size, block_count, true)
    }

    fn create_internal(
        path: impl AsRef<Path>,
        block_size: u32,
        block_count: u64,
        truncate: bool,
    ) -> Result<Self> {
        validate_block_size(block_size)?;
        if block_count == 0 {
            return Err(Error::InvalidInput("image must contain at least one block"));
        }
        let length = u64::from(block_size)
            .checked_mul(block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        let mut options = OpenOptions::new();
        options.read(true).write(true);
        if truncate {
            // Never truncate before acquiring the exclusive lock: another OpenFS
            // instance may currently be using this image.
            options.create(true);
        } else {
            options.create_new(true);
        }
        let file = options.open(path)?;
        lock_exclusive(&file)?;
        if truncate {
            // Preserve create_truncate semantics, but only after exclusive ownership.
            file.set_len(0)?;
        }
        file.set_len(length)?;
        file.sync_all()?;
        Ok(Self {
            file,
            block_size,
            block_count,
        })
    }

    /// Open an existing image, rejecting lengths that do not contain whole blocks.
    pub fn open(path: impl AsRef<Path>, block_size: u32) -> Result<Self> {
        validate_block_size(block_size)?;
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        lock_exclusive(&file)?;
        let length = file.metadata()?.len();
        if length == 0 || length % u64::from(block_size) != 0 {
            return Err(Error::InvalidInput(
                "image length must be a non-zero multiple of block size",
            ));
        }
        let block_count = length / u64::from(block_size);
        Ok(Self {
            file,
            block_size,
            block_count,
        })
    }

    fn byte_offset(&self, first_block: u64) -> Result<u64> {
        first_block
            .checked_mul(u64::from(self.block_size))
            .ok_or(Error::ArithmeticOverflow)
    }

    fn validate_buffer(&self, first_block: u64, length: usize) -> Result<u64> {
        let block_size = usize::try_from(self.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        if length == 0 || length % block_size != 0 {
            return Err(Error::InvalidInput(
                "buffer length must be a non-zero multiple of block size",
            ));
        }
        let count = u64::try_from(length / block_size).map_err(|_| Error::ArithmeticOverflow)?;
        check_block_range(first_block, count, self.block_count)?;
        Ok(count)
    }
}

impl BlockDevice for ImageDevice {
    fn block_size(&self) -> u32 {
        self.block_size
    }

    fn block_count(&self) -> u64 {
        self.block_count
    }

    fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
        self.validate_buffer(first_block, output.len())?;
        self.file
            .seek(SeekFrom::Start(self.byte_offset(first_block)?))?;
        self.file.read_exact(output)?;
        Ok(())
    }

    fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
        self.validate_buffer(first_block, input.len())?;
        self.file
            .seek(SeekFrom::Start(self.byte_offset(first_block)?))?;
        self.file.write_all(input)?;
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        self.file.sync_all()?;
        Ok(())
    }
}

/// A read-only handle to an existing raw device or regular-file test fixture.
/// Geometry must come from a trusted, independently verified source. This type
/// never requests write access and deliberately exposes no write operation.
pub struct ReadOnlyRawDevice {
    file: File,
    block_size: u32,
    block_count: u64,
}

impl ReadOnlyRawDevice {
    /// Open an existing path with read access only; never creates or truncates it.
    pub fn open(path: impl AsRef<Path>, block_size: u32, block_count: u64) -> Result<Self> {
        validate_block_size(block_size)?;
        if block_count == 0 {
            return Err(Error::InvalidInput(
                "raw device must contain at least one block",
            ));
        }
        let expected_length = u64::from(block_size)
            .checked_mul(block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        let file = OpenOptions::new().read(true).open(path)?;
        let metadata = file.metadata()?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::FileTypeExt;
            if !metadata.is_file() && !metadata.file_type().is_block_device() {
                return Err(Error::InvalidInput(
                    "raw device path must identify a regular file or block device",
                ));
            }
        }
        if metadata.is_file() && metadata.len() != expected_length {
            return Err(Error::InvalidInput(
                "regular-file length does not match declared raw-device geometry",
            ));
        }
        Ok(Self {
            file,
            block_size,
            block_count,
        })
    }

    pub fn block_size(&self) -> u32 {
        self.block_size
    }

    pub fn block_count(&self) -> u64 {
        self.block_count
    }

    pub fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
        let size = usize::try_from(self.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        if output.is_empty() || output.len() % size != 0 {
            return Err(Error::InvalidInput(
                "buffer length must be a non-zero multiple of block size",
            ));
        }
        let count = u64::try_from(output.len() / size).map_err(|_| Error::ArithmeticOverflow)?;
        check_block_range(first_block, count, self.block_count)?;
        let offset = first_block
            .checked_mul(u64::from(self.block_size))
            .ok_or(Error::ArithmeticOverflow)?;
        // Windows raw physical-disk handles do not reliably support the
        // shared file-pointer seek path. Use positional OVERLAPPED reads via
        // FileExt::seek_read instead; this also avoids shared seek state.
        #[cfg(windows)]
        {
            use std::os::windows::fs::FileExt;
            let mut completed = 0_usize;
            while completed < output.len() {
                let current_offset = offset
                    .checked_add(completed as u64)
                    .ok_or(Error::ArithmeticOverflow)?;
                let bytes = self.file.seek_read(&mut output[completed..], current_offset)
                    .map_err(|error| Error::Io(std::io::Error::new(
                        error.kind(),
                        format!(
                            "Windows positional read failed at byte offset {current_offset} (block {first_block}, {count} blocks): {error}"
                        ),
                    )))?;
                if bytes == 0 {
                    return Err(Error::Io(std::io::Error::new(
                        std::io::ErrorKind::UnexpectedEof,
                        format!(
                            "Windows raw-device read returned EOF at byte offset {current_offset}"
                        ),
                    )));
                }
                completed += bytes;
            }
        }
        #[cfg(not(windows))]
        {
            let mut file = &self.file;
            file.seek(SeekFrom::Start(offset)).map_err(|error| {
                Error::Io(std::io::Error::new(
                    error.kind(),
                    format!("seeking raw device to byte offset {offset} failed: {error}"),
                ))
            })?;
            file.read_exact(output).map_err(|error| {
                Error::Io(std::io::Error::new(
                    error.kind(),
                    format!(
                        "reading {count} raw-device block(s) at block {first_block} failed: {error}"
                    ),
                ))
            })?;
        }
        Ok(())
    }
}

/// An explicitly opened existing device or volume, without create/truncate semantics.
///
/// The caller must supply the logical block size and count from a trusted source.
/// This does not discover hardware geometry or coordinate with mounted filesystems, and
/// does not make formatting safe by itself. OpenFS handles acquire an exclusive OS file lock,
/// but external software may ignore advisory locks. Writes are raw and can destroy partition
/// tables or existing filesystems; never point it at a device containing data you need.
/// Regular files are accepted only when their length exactly matches the declared geometry.
pub struct RawDevice {
    file: File,
    block_size: u32,
    block_count: u64,
}

impl RawDevice {
    /// Open an existing path read/write without creating or truncating it.
    ///
    /// For regular files, the declared geometry must exactly match the current file
    /// length. Device nodes may report no meaningful length, so their block count is
    /// supplied explicitly by the caller.
    pub fn open(path: impl AsRef<Path>, block_size: u32, block_count: u64) -> Result<Self> {
        validate_block_size(block_size)?;
        if block_count == 0 {
            return Err(Error::InvalidInput(
                "raw device must contain at least one block",
            ));
        }
        let expected_length = u64::from(block_size)
            .checked_mul(block_count)
            .ok_or(Error::ArithmeticOverflow)?;
        let file = OpenOptions::new().read(true).write(true).open(path)?;
        lock_exclusive(&file)?;
        let metadata = file.metadata()?;
        // On Unix, a writable path is not necessarily a block device: character
        // devices such as /dev/null and FIFOs can otherwise pass the caller-supplied
        // geometry check and silently discard writes or block indefinitely.
        #[cfg(unix)]
        {
            use std::os::unix::fs::FileTypeExt;
            if !metadata.is_file() && !metadata.file_type().is_block_device() {
                return Err(Error::InvalidInput(
                    "raw device path must identify a regular file or block device",
                ));
            }
        }
        if metadata.is_file() && metadata.len() != expected_length {
            return Err(Error::InvalidInput(
                "regular-file length does not match declared raw-device geometry",
            ));
        }
        Ok(Self {
            file,
            block_size,
            block_count,
        })
    }

    fn validate_buffer(&self, first_block: u64, length: usize) -> Result<()> {
        let block_size = usize::try_from(self.block_size).map_err(|_| Error::ArithmeticOverflow)?;
        if length == 0 || length % block_size != 0 {
            return Err(Error::InvalidInput(
                "buffer length must be a non-zero multiple of block size",
            ));
        }
        let count = u64::try_from(length / block_size).map_err(|_| Error::ArithmeticOverflow)?;
        check_block_range(first_block, count, self.block_count)?;
        Ok(())
    }

    fn byte_offset(&self, first_block: u64) -> Result<u64> {
        first_block
            .checked_mul(u64::from(self.block_size))
            .ok_or(Error::ArithmeticOverflow)
    }
}

impl BlockDevice for RawDevice {
    fn block_size(&self) -> u32 {
        self.block_size
    }

    fn block_count(&self) -> u64 {
        self.block_count
    }

    fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
        self.validate_buffer(first_block, output.len())?;
        self.file
            .seek(SeekFrom::Start(self.byte_offset(first_block)?))?;
        self.file.read_exact(output)?;
        Ok(())
    }

    fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
        self.validate_buffer(first_block, input.len())?;
        self.file
            .seek(SeekFrom::Start(self.byte_offset(first_block)?))?;
        self.file.write_all(input)?;
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        self.file.sync_all()?;
        Ok(())
    }
}

/// Acquire an advisory exclusive lock before inspecting or mutating a volume.
///
/// Every OpenFS image/raw-device opener uses this gate so cooperating processes cannot
/// hold stale allocation or journal state at the same time. The lock is released by the
/// operating system when the file handle is dropped or the process exits.
fn lock_exclusive(file: &File) -> Result<()> {
    match file.try_lock_exclusive() {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == fs2::lock_contended_error().kind() => Err(
            Error::InvalidInput("volume is already open by another writer"),
        ),
        Err(error) => Err(Error::Io(error)),
    }
}

fn validate_block_size(block_size: u32) -> Result<()> {
    if !(MIN_BLOCK_SIZE..=MAX_BLOCK_SIZE).contains(&block_size) || !block_size.is_power_of_two() {
        return Err(Error::InvalidInput(
            "block size must be a power of two from 512 through 65536",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{BlockDevice, ImageDevice, RawDevice};
    use openfs_core::Error;
    use std::fs;
    use std::path::PathBuf;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_ID: AtomicU64 = AtomicU64::new(0);

    struct TempImage(PathBuf);

    impl TempImage {
        fn new() -> Self {
            let id = NEXT_ID.fetch_add(1, Ordering::Relaxed);
            let path =
                std::env::temp_dir().join(format!("openfs-image-{}-{id}.img", std::process::id()));
            let _ = fs::remove_file(&path);
            Self(path)
        }
    }

    impl Drop for TempImage {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.0);
        }
    }

    #[test]
    fn read_only_raw_device_reads_blocks_and_rejects_out_of_range_access() {
        let image = TempImage::new();
        {
            let mut writable = ImageDevice::create(&image.0, 512, 4).unwrap();
            writable.write_blocks(0, &[0x31; 512]).unwrap();
            writable.write_blocks(3, &[0x73; 512]).unwrap();
            writable.flush().unwrap();
        }
        let mut read_only = super::ReadOnlyRawDevice::open(&image.0, 512, 4).unwrap();
        assert_eq!(read_only.block_size(), 512);
        assert_eq!(read_only.block_count(), 4);
        let mut first = [0; 512];
        let mut last = [0; 512];
        read_only.read_blocks(0, &mut first).unwrap();
        read_only.read_blocks(3, &mut last).unwrap();
        assert_eq!(first, [0x31; 512]);
        assert_eq!(last, [0x73; 512]);
        assert!(read_only.read_blocks(4, &mut last).is_err());
        assert!(super::ReadOnlyRawDevice::open(&image.0, 512, 3).is_err());
    }

    #[test]
    fn image_round_trip_survives_reopen_and_flush() {
        let image = TempImage::new();
        {
            let mut device = ImageDevice::create(&image.0, 4096, 4).unwrap();
            let data = vec![0xA5; 8192];
            device.write_blocks(2, &data).unwrap();
            device.flush().unwrap();
        }
        let mut reopened = ImageDevice::open(&image.0, 4096).unwrap();
        let mut actual = vec![0; 8192];
        reopened.read_blocks(2, &mut actual).unwrap();
        assert_eq!(actual, vec![0xA5; 8192]);
        assert_eq!(reopened.block_count(), 4);
    }

    #[test]
    fn create_does_not_overwrite_existing_images_without_explicit_opt_in() {
        let image = TempImage::new();
        {
            let mut device = ImageDevice::create(&image.0, 4096, 2).unwrap();
            device.write_blocks(0, &vec![0xD4; 4096]).unwrap();
            device.flush().unwrap();
        }

        assert!(matches!(
            ImageDevice::create(&image.0, 4096, 2),
            Err(Error::Io(_))
        ));
        {
            let mut device = ImageDevice::open(&image.0, 4096).unwrap();
            let mut data = vec![0; 4096];
            device.read_blocks(0, &mut data).unwrap();
            assert_eq!(data, vec![0xD4; 4096]);
        }

        let mut truncated = ImageDevice::create_truncate(&image.0, 4096, 2).unwrap();
        let mut data = vec![0xFF; 4096];
        truncated.read_blocks(0, &mut data).unwrap();
        assert_eq!(data, vec![0; 4096]);
    }

    #[test]
    fn image_device_excludes_a_second_writer_until_the_first_handle_drops() {
        let image = TempImage::new();
        let first = ImageDevice::create(&image.0, 4096, 2).unwrap();

        assert!(matches!(
            ImageDevice::open(&image.0, 4096),
            Err(Error::InvalidInput(
                "volume is already open by another writer"
            ))
        ));
        assert!(matches!(
            RawDevice::open(&image.0, 4096, 2),
            Err(Error::InvalidInput(
                "volume is already open by another writer"
            ))
        ));

        drop(first);
        let reopened = ImageDevice::open(&image.0, 4096).unwrap();
        drop(reopened);
    }

    #[test]
    fn create_truncate_never_truncates_an_image_locked_by_another_writer() {
        let image = TempImage::new();
        let mut first = ImageDevice::create(&image.0, 4096, 2).unwrap();
        first.write_blocks(0, &vec![0xA7; 4096]).unwrap();
        first.flush().unwrap();
        let original_len = fs::metadata(&image.0).unwrap().len();

        assert!(matches!(
            ImageDevice::create_truncate(&image.0, 4096, 1),
            Err(Error::InvalidInput(
                "volume is already open by another writer"
            ))
        ));
        assert_eq!(fs::metadata(&image.0).unwrap().len(), original_len);
        let mut actual = vec![0; 4096];
        first.read_blocks(0, &mut actual).unwrap();
        assert_eq!(actual, vec![0xA7; 4096]);
    }

    #[test]
    fn torn_write_injector_persists_new_prefix_and_old_suffix() {
        use super::{FaultInjectingDevice, IoOperation};

        let image = TempImage::new();
        let device = ImageDevice::create(&image.0, 4096, 2).unwrap();
        let mut device = FaultInjectingDevice::new(device);
        let old = vec![0x11; 4096];
        let new = vec![0x22; 4096];
        device.write_blocks(0, &old).unwrap();
        device.flush().unwrap();

        device.tear_next_write_after(257);
        assert!(matches!(device.write_blocks(0, &new), Err(Error::Io(_))));
        let mut actual = vec![0; 4096];
        device.read_blocks(0, &mut actual).unwrap();
        assert_eq!(&actual[..257], &new[..257]);
        assert_eq!(&actual[257..], &old[257..]);

        device.fail_next(IoOperation::Read);
        assert!(matches!(
            device.read_blocks(0, &mut actual),
            Err(Error::Io(_))
        ));
    }

    #[test]
    fn fail_after_counts_only_matching_operations_and_fails_once() {
        use super::{FaultInjectingDevice, IoOperation};

        let image = TempImage::new();
        let device = ImageDevice::create(&image.0, 4096, 2).unwrap();
        let mut device = FaultInjectingDevice::new(device);
        let first = vec![0x31; 4096];
        let second = vec![0x42; 4096];
        let mut actual = vec![0; 4096];

        device.fail_after(IoOperation::Write, 2);
        device.write_blocks(0, &first).unwrap();
        // Reads do not consume a write-specific failure budget.
        device.read_blocks(0, &mut actual).unwrap();
        assert_eq!(actual, first);
        device.write_blocks(1, &second).unwrap();
        assert!(matches!(device.write_blocks(0, &second), Err(Error::Io(_))));
        // The injected failure is one-shot; the following write succeeds.
        device.write_blocks(0, &second).unwrap();
        device.read_blocks(0, &mut actual).unwrap();
        assert_eq!(actual, second);
    }

    #[test]
    fn torn_write_rejects_invalid_prefix_before_touching_the_device() {
        use super::{FaultInjectingDevice, IoOperation};

        let image = TempImage::new();
        let device = ImageDevice::create(&image.0, 4096, 2).unwrap();
        let mut device = FaultInjectingDevice::new(device);
        let original = vec![0x71; 4096];
        device.write_blocks(0, &original).unwrap();

        device.tear_next_write_after(4096);
        assert!(matches!(
            device.write_blocks(0, &[0xA2; 4096]),
            Err(Error::InvalidInput(_))
        ));
        let mut actual = vec![0; 4096];
        device.read_blocks(0, &mut actual).unwrap();
        assert_eq!(actual, original);

        device.fail_next(IoOperation::Read);
        assert!(matches!(
            device.read_blocks(0, &mut actual),
            Err(Error::Io(_))
        ));
    }

    #[test]
    fn raw_device_round_trip_preserves_existing_file_and_reopens() {
        let image = TempImage::new();
        {
            let mut initial = ImageDevice::create(&image.0, 4096, 3).unwrap();
            initial.write_blocks(0, &vec![0x37; 4096]).unwrap();
            initial.flush().unwrap();
        }

        {
            let mut raw = RawDevice::open(&image.0, 4096, 3).unwrap();
            assert_eq!(raw.block_size(), 4096);
            assert_eq!(raw.block_count(), 3);
            raw.write_blocks(1, &vec![0xA9; 4096]).unwrap();
            raw.flush().unwrap();
        }

        let mut reopened = ImageDevice::open(&image.0, 4096).unwrap();
        let mut first = vec![0; 4096];
        let mut second = vec![0; 4096];
        reopened.read_blocks(0, &mut first).unwrap();
        reopened.read_blocks(1, &mut second).unwrap();
        assert_eq!(first, vec![0x37; 4096]);
        assert_eq!(second, vec![0xA9; 4096]);
    }

    #[cfg(unix)]
    #[test]
    fn raw_device_rejects_character_devices_instead_of_treating_them_as_disks() {
        // /dev/null accepts writes but does not store bytes. It must never be
        // accepted as a block device just because the caller supplies geometry.
        assert!(matches!(
            RawDevice::open("/dev/null", 4096, 1),
            Err(Error::InvalidInput(
                "raw device path must identify a regular file or block device"
            ))
        ));
    }

    #[test]
    fn raw_device_rejects_wrong_geometry_without_modifying_file() {
        let image = TempImage::new();
        fs::write(&image.0, vec![0x5C; 8192]).unwrap();

        assert!(matches!(
            RawDevice::open(&image.0, 4096, 1),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            RawDevice::open(&image.0, 4096, 0),
            Err(Error::InvalidInput(_))
        ));

        let data = fs::read(&image.0).unwrap();
        assert_eq!(data, vec![0x5C; 8192]);
    }

    #[test]
    fn raw_device_open_never_creates_a_missing_path() {
        let image = TempImage::new();
        assert!(!image.0.exists());
        assert!(matches!(
            RawDevice::open(&image.0, 4096, 1),
            Err(Error::Io(_))
        ));
        assert!(!image.0.exists());
    }

    #[test]
    fn raw_device_rejects_invalid_or_overflowing_geometry_before_open() {
        let image = TempImage::new();
        assert!(matches!(
            RawDevice::open(&image.0, 1000, 1),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            RawDevice::open(&image.0, 4096, u64::MAX),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(!image.0.exists());
    }

    #[test]
    fn raw_device_rejects_misaligned_and_out_of_range_access() {
        let image = TempImage::new();
        let device = ImageDevice::create(&image.0, 4096, 2).unwrap();
        drop(device);
        let mut raw = RawDevice::open(&image.0, 4096, 2).unwrap();

        assert!(matches!(
            raw.read_blocks(0, &mut [0; 7]),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            raw.write_blocks(2, &[0xAA; 4096]),
            Err(Error::OutOfBounds { .. })
        ));
        assert!(matches!(
            raw.write_blocks(u64::MAX, &[0xAA; 4096]),
            Err(Error::OutOfBounds { .. })
        ));
    }

    #[test]
    fn accepts_supported_block_size_limits() {
        for block_size in [512, 65_536] {
            let image = TempImage::new();
            let device = ImageDevice::create(&image.0, block_size, 1).unwrap();
            assert_eq!(device.block_size(), block_size);
            assert_eq!(device.block_count(), 1);
        }
    }

    #[test]
    fn rejects_invalid_block_sizes_and_empty_images() {
        let image = TempImage::new();
        assert!(matches!(
            ImageDevice::create(&image.0, 1000, 4),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ImageDevice::create(&image.0, 4096, 0),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ImageDevice::create(&image.0, 65_536, u64::MAX),
            Err(Error::ArithmeticOverflow)
        ));
    }

    #[test]
    fn rejects_out_of_range_and_misaligned_buffers_before_io() {
        let image = TempImage::new();
        let mut device = ImageDevice::create(&image.0, 4096, 2).unwrap();
        assert!(matches!(
            device.read_blocks(2, &mut vec![0; 4096]),
            Err(Error::OutOfBounds { .. })
        ));
        assert!(matches!(
            device.write_blocks(1, &vec![1; 8192]),
            Err(Error::OutOfBounds { .. })
        ));
        assert!(matches!(
            device.write_blocks(u64::MAX, &vec![1; 4096]),
            Err(Error::OutOfBounds { .. })
        ));
        assert!(matches!(
            device.write_blocks(0, &[1; 7]),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            device.read_blocks(0, &mut []),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn open_rejects_image_with_partial_trailing_block() {
        let image = TempImage::new();
        fs::write(&image.0, vec![0; 4097]).unwrap();
        assert!(matches!(
            ImageDevice::open(&image.0, 4096),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn open_propagates_missing_file_errors_and_rejects_empty_images() {
        let image = TempImage::new();
        assert!(matches!(
            ImageDevice::open(&image.0, 4096),
            Err(Error::Io(_))
        ));
        fs::write(&image.0, b"").unwrap();
        assert!(matches!(
            ImageDevice::open(&image.0, 4096),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn fault_injector_fails_one_selected_operation_then_recovers() {
        use super::{FaultInjectingDevice, IoOperation};

        let image = TempImage::new();
        let device = ImageDevice::create(&image.0, 4096, 2).unwrap();
        let mut device = FaultInjectingDevice::new(device);
        let expected = vec![0x6B; 4096];

        device.fail_next(IoOperation::Write);
        let mut untouched = vec![0; 4096];
        device.read_blocks(0, &mut untouched).unwrap();
        assert_eq!(untouched, vec![0; 4096]);
        assert!(matches!(
            device.write_blocks(0, &expected),
            Err(Error::Io(_))
        ));
        device.write_blocks(0, &expected).unwrap();

        device.fail_next(IoOperation::Flush);
        assert!(matches!(device.flush(), Err(Error::Io(_))));
        device.flush().unwrap();

        device.fail_next(IoOperation::Read);
        assert!(matches!(
            device.read_blocks(0, &mut vec![0; 4096]),
            Err(Error::Io(_))
        ));
        let mut actual = vec![0; 4096];
        device.read_blocks(0, &mut actual).unwrap();
        assert_eq!(actual, expected);
    }
}
