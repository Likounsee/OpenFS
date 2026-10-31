//! Windows user-mode mount adapter backed by Dokany.
//!
//! Dokany is an optional host integration: OpenFS remains the on-disk filesystem.
//! This module maps Windows file operations to the portable VolumeSession API.

use std::error::Error as StdError;
use std::path::Path;
use std::sync::{Arc, Mutex};
use std::time::SystemTime;

use dokan::{
    CreateFileInfo, DiskSpaceInfo, FileInfo, FileSystemHandler, FindData, MountFlags,
    OperationError, OperationInfo, VolumeInfo,
};
use openfs_block::ImageDevice;
use openfs_core::Error as CoreError;
use openfs_format::{ObjectKind, VolumeSession};
use widestring::{U16CString, U16CStr};
use winapi::shared::winerror::{
    ERROR_ALREADY_EXISTS, ERROR_DIR_NOT_EMPTY, ERROR_FILE_EXISTS, ERROR_FILE_NOT_FOUND,
    ERROR_GEN_FAILURE, ERROR_INVALID_PARAMETER,
};
use winapi::um::fileapi::{
    CREATE_ALWAYS, CREATE_NEW, OPEN_ALWAYS, OPEN_EXISTING, TRUNCATE_EXISTING,
};
use winapi::um::winnt::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL, FILE_CASE_PRESERVED_NAMES,
    FILE_CASE_SENSITIVE_SEARCH, FILE_UNICODE_ON_DISK,
};

type Session = VolumeSession<ImageDevice>;
type FileHandle = Arc<Mutex<String>>;

/// Shared OpenFS volume state. Dokany may dispatch callbacks concurrently, so
/// the session is serialized while per-open path state is independently guarded.
struct OpenFsHandler {
    session: Mutex<Session>,
    total_bytes: u64,
}

impl OpenFsHandler {
    fn new(session: Session, total_bytes: u64) -> Self {
        Self {
            session: Mutex::new(session),
            total_bytes,
        }
    }

    fn path_for(&self, context: &FileHandle) -> Result<String, OperationError> {
        context
            .lock()
            .map(|path| path.clone())
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))
    }

    fn stat(&self, path: &str) -> Result<openfs_format::MetadataRecord, OperationError> {
        self.session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
            .stat(path)
            .map_err(map_core_error)
    }

    fn list_directory(&self, path: &str) -> Result<Vec<openfs_format::DirectoryEntry>, OperationError> {
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let mut entries = Vec::new();
        let mut marker: Option<String> = None;
        loop {
            let page = session
                .read_directory(path, marker.as_deref(), 256)
                .map_err(map_core_error)?;
            if page.is_empty() {
                break;
            }
            marker = page.last().map(|entry| entry.name.clone());
            let page_len = page.len();
            entries.extend(page);
            if page_len < 256 {
                break;
            }
        }
        Ok(entries)
    }
}

fn map_core_error(error: CoreError) -> OperationError {
    match error {
        CoreError::InvalidInput(_) => OperationError::Win32(ERROR_INVALID_PARAMETER),
        CoreError::Io(_) => OperationError::Win32(ERROR_GEN_FAILURE),
        _ => OperationError::Win32(ERROR_GEN_FAILURE),
    }
}

fn path_from_windows(path: &U16CStr) -> Result<String, OperationError> {
    let raw = path.to_string_lossy();
    let mut components = Vec::new();
    for component in raw.replace('\\', "/").split('/') {
        if component.is_empty() {
            continue;
        }
        if component == "." || component == ".." || component.contains(':') {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        components.push(component.to_owned());
    }
    if components.is_empty() {
        Ok("/".to_owned())
    } else {
        Ok(format!("/{}", components.join("/")))
    }
}

fn split_parent(path: &str) -> Result<(&str, &str), OperationError> {
    let (parent, name) = path
        .rsplit_once('/')
        .ok_or(OperationError::Win32(ERROR_INVALID_PARAMETER))?;
    if name.is_empty() {
        return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
    }
    Ok((if parent.is_empty() { "/" } else { parent }, name))
}

fn make_file_info(record: &openfs_format::MetadataRecord) -> FileInfo {
    let is_dir = record.kind == ObjectKind::Directory;
    let epoch = SystemTime::UNIX_EPOCH;
    FileInfo {
        attributes: if is_dir {
            FILE_ATTRIBUTE_DIRECTORY
        } else {
            FILE_ATTRIBUTE_NORMAL
        },
        creation_time: epoch,
        last_access_time: epoch,
        last_write_time: epoch,
        file_size: record.size,
        number_of_links: 1,
        file_index: record.id.0 as u64,
    }
}

impl<'a, 'b: 'a> FileSystemHandler<'a, 'b> for OpenFsHandler {
    type Context = FileHandle;

    fn create_file(
        &'b self,
        file_name: &U16CStr,
        _security_context: &dokan::IO_SECURITY_CONTEXT,
        _desired_access: winapi::um::winnt::ACCESS_MASK,
        _file_attributes: u32,
        _share_access: u32,
        create_disposition: u32,
        create_options: u32,
        _info: &mut OperationInfo<'a, 'b, Self>,
    ) -> Result<CreateFileInfo<Self::Context>, OperationError> {
        let path = path_from_windows(file_name)?;
        if path == "/" {
            self.stat("/")?;
            return Ok(CreateFileInfo {
                context: Arc::new(Mutex::new(path)),
                is_dir: true,
                new_file_created: false,
            });
        }

        let wants_directory = (create_options & 0x0000_0001) != 0;
        let existing = self.stat(&path);
        let (is_dir, new_file_created) = match existing {
            Ok(record) => {
                if create_disposition == CREATE_NEW {
                    return Err(OperationError::Win32(ERROR_FILE_EXISTS));
                }
                if wants_directory != (record.kind == ObjectKind::Directory) {
                    return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
                }
                if create_disposition == CREATE_ALWAYS || create_disposition == TRUNCATE_EXISTING {
                    if wants_directory {
                        return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
                    }
                    self.session
                        .lock()
                        .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
                        .set_file_len(&path, 0)
                        .map_err(map_core_error)?;
                }
                (wants_directory, false)
            }
            Err(_) => {
                if create_disposition == OPEN_EXISTING || create_disposition == TRUNCATE_EXISTING {
                    return Err(OperationError::Win32(ERROR_FILE_NOT_FOUND));
                }
                if !matches!(create_disposition, CREATE_NEW | CREATE_ALWAYS | OPEN_ALWAYS) {
                    return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
                }
                let (parent, name) = split_parent(&path)?;
                let mut session = self
                    .session
                    .lock()
                    .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
                if wants_directory {
                    session.create_directory_auto(parent, name).map_err(map_core_error)?;
                } else {
                    session.create_file_auto(parent, name).map_err(map_core_error)?;
                }
                (wants_directory, true)
            }
        };

        Ok(CreateFileInfo {
            context: Arc::new(Mutex::new(path)),
            is_dir,
            new_file_created,
        })
    }

    fn cleanup(
        &'b self,
        _file_name: &U16CStr,
        info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) {
        if info.delete_on_close() {
            if let Ok(path) = self.path_for(context) {
                if let Ok(mut session) = self.session.lock() {
                    let _ = session.remove(&path);
                }
            }
        }
    }

    fn read_file(
        &'b self,
        _file_name: &U16CStr,
        offset: i64,
        buffer: &mut [u8],
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<u32, OperationError> {
        if offset < 0 {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        let path = self.path_for(context)?;
        let bytes = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
            .read_file_range(&path, offset as u64, buffer.len())
            .map_err(map_core_error)?;
        buffer[..bytes.len()].copy_from_slice(&bytes);
        Ok(bytes.len() as u32)
    }

    fn write_file(
        &'b self,
        _file_name: &U16CStr,
        offset: i64,
        buffer: &[u8],
        info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<u32, OperationError> {
        let path = self.path_for(context)?;
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let write_offset = if info.write_to_eof() {
            session.stat(&path).map_err(map_core_error)?.size
        } else {
            if offset < 0 {
                return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
            }
            offset as u64
        };
        session
            .write_file_range(&path, write_offset, buffer)
            .map_err(map_core_error)?;
        Ok(buffer.len() as u32)
    }

    fn flush_file_buffers(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        _context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        self.session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
            .flush()
            .map_err(map_core_error)
    }

    fn get_file_information(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<FileInfo, OperationError> {
        let path = self.path_for(context)?;
        self.stat(&path).map(|record| make_file_info(&record))
    }

    fn find_files(
        &'b self,
        _file_name: &U16CStr,
        mut fill_find_data: impl FnMut(&FindData) -> Result<(), dokan::FillDataError>,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        let path = self.path_for(context)?;
        for entry in self.list_directory(&path)? {
            let file_name = U16CString::from_str(&entry.name)
                .map_err(|_| OperationError::Win32(ERROR_INVALID_PARAMETER))?;
            let epoch = SystemTime::UNIX_EPOCH;
            let data = FindData {
                attributes: if entry.kind == ObjectKind::Directory {
                    FILE_ATTRIBUTE_DIRECTORY
                } else {
                    FILE_ATTRIBUTE_NORMAL
                },
                creation_time: epoch,
                last_access_time: epoch,
                last_write_time: epoch,
                file_size: self.stat(&format!("{}/{}", path.trim_end_matches('/'), entry.name))?.size,
                file_name,
            };
            fill_find_data(&data)?;
        }
        Ok(())
    }

    fn delete_file(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        let path = self.path_for(context)?;
        let record = self.stat(&path)?;
        if record.kind != ObjectKind::File {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        Ok(())
    }

    fn delete_directory(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        let path = self.path_for(context)?;
        let record = self.stat(&path)?;
        if record.kind != ObjectKind::Directory {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        if !self.list_directory(&path)?.is_empty() {
            return Err(OperationError::Win32(ERROR_DIR_NOT_EMPTY));
        }
        Ok(())
    }

    fn move_file(
        &'b self,
        _file_name: &U16CStr,
        new_file_name: &U16CStr,
        replace_if_existing: bool,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        let old_path = self.path_for(context)?;
        let new_path = path_from_windows(new_file_name)?;
        if self.stat(&new_path).is_ok() {
            if !replace_if_existing {
                return Err(OperationError::Win32(ERROR_ALREADY_EXISTS));
            }
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        let (new_parent, new_name) = split_parent(&new_path)?;
        self.session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
            .rename(&old_path, new_parent, new_name)
            .map_err(map_core_error)?;
        *context
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))? = new_path;
        Ok(())
    }

    fn set_end_of_file(
        &'b self,
        _file_name: &U16CStr,
        offset: i64,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        if offset < 0 {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        let path = self.path_for(context)?;
        self.session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
            .set_file_len(&path, offset as u64)
            .map_err(map_core_error)
    }

    fn get_disk_free_space(
        &'b self,
        _info: &OperationInfo<'a, 'b, Self>,
    ) -> Result<DiskSpaceInfo, OperationError> {
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let free = session
            .free_data_blocks()
            .map_err(map_core_error)?
            .saturating_mul(u64::from(session.block_size()));
        Ok(DiskSpaceInfo {
            byte_count: self.total_bytes,
            free_byte_count: free,
            available_byte_count: free,
        })
    }

    fn get_volume_information(
        &'b self,
        _info: &OperationInfo<'a, 'b, Self>,
    ) -> Result<VolumeInfo, OperationError> {
        let session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let uuid = session.uuid();
        Ok(VolumeInfo {
            name: U16CString::from_str("OpenFS")
                .map_err(|_| OperationError::Win32(ERROR_INVALID_PARAMETER))?,
            serial_number: u32::from_le_bytes([uuid[0], uuid[1], uuid[2], uuid[3]]),
            max_component_length: 64,
            fs_flags: FILE_CASE_PRESERVED_NAMES | FILE_CASE_SENSITIVE_SEARCH | FILE_UNICODE_ON_DISK,
            fs_name: U16CString::from_str("OpenFS")
                .map_err(|_| OperationError::Win32(ERROR_INVALID_PARAMETER))?,
        })
    }
}

pub fn mount_image(
    image_path: &Path,
    mount_point: &str,
    block_size: u32,
) -> Result<(), Box<dyn StdError>> {
    let image_bytes = std::fs::metadata(image_path)?.len();
    let device = ImageDevice::open(image_path, block_size)?;
    let session = VolumeSession::open(device)?;
    let handler = OpenFsHandler::new(session, image_bytes);

    let mount_text = if mount_point.ends_with(':') {
        format!("{mount_point}\\")
    } else {
        mount_point.to_owned()
    };
    let mount_point = U16CString::from_str(&mount_text)?;
    let mut drive = dokan::Drive::new();
    drive
        .mount_point(&mount_point)
        .sector_size(block_size)
        .allocation_unit_size(block_size)
        .flags(MountFlags::CURRENT_SESSION | MountFlags::ENABLE_FCB_GARBAGE_COLLECTION);

    dokan::init();
    let result = drive.mount(&handler);
    let flush_result = handler
        .session
        .lock()
        .map_err(|_| "OpenFS session lock was poisoned")?
        .flush();
    dokan::shutdown();
    result?;
    flush_result?;
    println!("OpenFS volume unmounted cleanly.");
    Ok(())
}

pub fn unmount_drive(mount_point: &str) -> Result<(), Box<dyn StdError>> {
    let mount_text = if mount_point.ends_with(':') {
        format!("{mount_point}\\")
    } else {
        mount_point.to_owned()
    };
    let mount_point = U16CString::from_str(&mount_text)?;
    dokan::init();
    let result = dokan::unmount(&mount_point);
    dokan::shutdown();
    if !result {
        return Err(format!("Dokan could not unmount {mount_text}").into());
    }
    println!("Unmount request sent to {mount_text}.");
    Ok(())
}
