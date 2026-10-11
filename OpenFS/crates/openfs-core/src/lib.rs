//! Shared, platform-neutral types and validation helpers for OpenFS.

use std::fmt;

/// Error categories shared by the early OpenFS crates.
#[derive(Debug)]
pub enum Error {
    /// A caller supplied an invalid parameter or buffer.
    InvalidInput(&'static str),
    /// A requested block range lies outside the device.
    OutOfBounds {
        first_block: u64,
        block_count: u64,
        device_blocks: u64,
    },
    /// An offset or size cannot be represented safely.
    ArithmeticOverflow,
    /// Disk data failed structural or integrity validation.
    Corrupt(&'static str),
    /// The on-disk format version is not supported.
    UnsupportedVersion(u16),
    /// The host reported an input/output failure.
    Io(std::io::Error),
}

/// Result alias used throughout OpenFS crates.
pub type Result<T> = std::result::Result<T, Error>;

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidInput(reason) => write!(f, "invalid input: {reason}"),
            Self::OutOfBounds { first_block, block_count, device_blocks } => write!(
                f,
                "block range {first_block}..+{block_count} exceeds device capacity {device_blocks}"
            ),
            Self::ArithmeticOverflow => f.write_str("arithmetic overflow"),
            Self::Corrupt(reason) => write!(f, "corrupt on-disk data: {reason}"),
            Self::UnsupportedVersion(version) => {
                write!(f, "unsupported on-disk format version {version}")
            }
            Self::Io(error) => write!(f, "I/O error: {error}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            _ => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(value: std::io::Error) -> Self {
        Self::Io(value)
    }
}

/// Validate a non-empty range without adding first_block + block_count.
pub fn check_block_range(first_block: u64, block_count: u64, device_blocks: u64) -> Result<()> {
    if block_count == 0 {
        return Err(Error::InvalidInput("block count must be non-zero"));
    }
    if first_block >= device_blocks || block_count > device_blocks - first_block {
        return Err(Error::OutOfBounds {
            first_block,
            block_count,
            device_blocks,
        });
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{check_block_range, Error};

    #[test]
    fn accepts_last_block_and_rejects_ranges_past_end() {
        assert!(check_block_range(9, 1, 10).is_ok());
        assert!(matches!(
            check_block_range(9, 2, 10),
            Err(Error::OutOfBounds { .. })
        ));
        assert!(matches!(
            check_block_range(0, 0, 10),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn rejects_overflowing_untrusted_range() {
        assert!(matches!(
            check_block_range(u64::MAX, 2, u64::MAX),
            Err(Error::OutOfBounds { .. })
        ));
    }
}
