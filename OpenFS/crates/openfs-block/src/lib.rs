//! Bounded block-device abstractions and a file-backed development image.

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
    TornWrite { prefix_bytes: usize },
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
        if self.next_failure == Some(FaultAction::Fail(operation)) {
            self.next_failure = None;
            return Err(Error::Io(std::io::Error::other(
                "injected OpenFS block-device failure",
            )));
        }
        Ok(())
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
            let block_size = usize::try_from(self.inner.block_size())
                .map_err(|_| Error::ArithmeticOverflow)?;
            if block_size == 0
                || input.is_empty()
                || input.len() % block_size != 0
                || prefix_bytes >= input.len()
            {
                return Err(Error::InvalidInput(
                    "torn-write prefix must be shorter than a non-empty block-aligned buffer",
                ));
            }
            let mut mixed = vec![0; input.len()];
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
            options.create(true).truncate(true);
        } else {
            options.create_new(true);
        }
        let file = options.open(path)?;
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
    use super::{BlockDevice, ImageDevice};
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
        assert!(matches!(
            device.write_blocks(0, &new),
            Err(Error::Io(_))
        ));
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
