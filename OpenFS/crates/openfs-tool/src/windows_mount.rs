//! Windows user-mode mount adapter backed by Dokany.
//!
//! Dokany is an optional host integration: OpenFS remains the on-disk filesystem.
//! This module maps Windows file operations to the portable VolumeSession API.

use std::collections::{HashMap, HashSet};
use std::error::Error as StdError;
use std::path::Path;
use std::sync::Mutex;
use std::time::SystemTime;

use dokan::{
    CreateFileInfo, DiskSpaceInfo, FileInfo, FileSystemHandler, FindData, MountFlags,
    OperationError, OperationInfo, VolumeInfo,
};
use openfs_block::{BlockDevice, ImageDevice, RawDevice};
use openfs_core::Error as CoreError;
use openfs_format::{ObjectId, ObjectKind, VolumeSession};
use widestring::{U16CStr, U16CString};
use winapi::shared::winerror::{
    ERROR_ALREADY_EXISTS, ERROR_DIR_NOT_EMPTY, ERROR_FILE_EXISTS, ERROR_FILE_NOT_FOUND,
    ERROR_ACCESS_DENIED, ERROR_GEN_FAILURE, ERROR_INVALID_PARAMETER, ERROR_SHARING_VIOLATION,
};
use winapi::um::fileapi::{
    CREATE_ALWAYS, CREATE_NEW, OPEN_ALWAYS, OPEN_EXISTING, TRUNCATE_EXISTING,
};
use winapi::um::winnt::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL, FILE_CASE_PRESERVED_NAMES,
    FILE_CASE_SENSITIVE_SEARCH, FILE_UNICODE_ON_DISK,
};

type FileHandle = ObjectId;

/// Collects callback failures that Dokan cleanup cannot return to Windows.
#[derive(Default)]
struct DeferredErrorLatch {
    message: Mutex<Option<String>>,
}

impl DeferredErrorLatch {
    fn record(&self, error: String) {
        match self.message.lock() {
            Ok(mut pending) => match pending.as_mut() {
                Some(existing) => {
                    existing.push_str("; ");
                    existing.push_str(&error);
                }
                None => *pending = Some(error),
            },
            Err(_) => eprintln!("OpenFS: unable to record deferred cleanup error: {error}"),
        }
    }

    fn take(&self) -> Result<Option<String>, &'static str> {
        self.message
            .lock()
            .map(|mut pending| pending.take())
            .map_err(|_| "OpenFS deferred-error lock was poisoned")
    }
}

/// Tracks independent Dokany opens by stable object ID. Delete-on-close is
/// deferred until the final open object is cleaned up, so other live handles do
/// not suddenly lose their namespace target.
#[derive(Default)]
struct OpenHandleState {
    // Keep counts and delete-pending state under one mutex. A single critical
    // section makes open/close transitions atomic and removes lock-order risks.
    inner: Mutex<OpenHandleInner>,
}

#[derive(Default)]
struct OpenHandleInner {
    counts: HashMap<ObjectId, usize>,
    delete_pending: HashSet<ObjectId>,
}

impl OpenHandleState {
    fn register(&self, object_id: ObjectId) -> Result<(), &'static str> {
        let mut inner = self
            .inner
            .lock()
            .map_err(|_| "OpenFS open-handle state lock was poisoned")?;
        if inner.delete_pending.contains(&object_id) {
            return Err("object is pending deletion");
        }
        let count = inner.counts.entry(object_id).or_insert(0);
        *count = count.checked_add(1).ok_or("OpenFS open-handle count overflow")?;
        Ok(())
    }

    /// Release one open and return true when a pending deletion can now run.
    fn close(&self, object_id: ObjectId, delete_on_close: bool) -> Result<bool, &'static str> {
        let mut inner = self
            .inner
            .lock()
            .map_err(|_| "OpenFS open-handle state lock was poisoned")?;
        if !inner.counts.contains_key(&object_id) {
            return Err("cannot close an object without a registered open handle");
        }
        if delete_on_close {
            inner.delete_pending.insert(object_id);
        }
        let remove = match inner.counts.get_mut(&object_id) {
            Some(count) if *count > 1 => {
                *count -= 1;
                false
            }
            Some(_) => true,
            None => unreachable!("the open handle was checked above"),
        };
        if remove {
            inner.counts.remove(&object_id);
        }
        Ok(!inner.counts.contains_key(&object_id)
            && inner.delete_pending.contains(&object_id))
    }

    fn is_open(&self, object_id: ObjectId) -> Result<bool, &'static str> {
        self.inner
            .lock()
            .map(|inner| inner.counts.get(&object_id).copied().unwrap_or(0) != 0)
            .map_err(|_| "OpenFS open-handle state lock was poisoned")
    }

    fn is_delete_pending(&self, object_id: ObjectId) -> Result<bool, &'static str> {
        self.inner
            .lock()
            .map(|inner| inner.delete_pending.contains(&object_id))
            .map_err(|_| "OpenFS open-handle state lock was poisoned")
    }

    fn complete_delete(&self, object_id: ObjectId) -> Result<(), &'static str> {
        self.inner
            .lock()
            .map_err(|_| "OpenFS open-handle state lock was poisoned")?
            .delete_pending
            .remove(&object_id);
        Ok(())
    }
}

/// Shared OpenFS volume state. Dokany may dispatch callbacks concurrently, so
/// the session is serialized and open handles retain stable on-disk object IDs.
struct OpenFsHandler<D: BlockDevice> {
    session: Mutex<VolumeSession<D>>,
    total_bytes: u64,
    deferred_error: DeferredErrorLatch,
    open_handles: OpenHandleState,
}

impl<D: BlockDevice> OpenFsHandler<D> {
    fn new(session: VolumeSession<D>, total_bytes: u64) -> Self {
        Self {
            session: Mutex::new(session),
            total_bytes,
            deferred_error: DeferredErrorLatch::default(),
            open_handles: OpenHandleState::default(),
        }
    }

    fn with_path<T>(
        &self,
        context: &FileHandle,
        operation: impl FnOnce(&mut VolumeSession<D>, &str) -> openfs_core::Result<T>,
    ) -> Result<T, OperationError> {
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let path = session
            .path_for_object(*context)
            .map_err(map_core_error)?;
        operation(&mut session, &path).map_err(map_core_error)
    }

    fn register_open_handle(&self, object_id: ObjectId) -> Result<(), OperationError> {
        self.open_handles
            .register(object_id)
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))
    }

    fn has_open_handle(&self, object_id: ObjectId) -> Result<bool, OperationError> {
        self.open_handles
            .is_open(object_id)
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))
    }

    fn is_delete_pending(&self, object_id: ObjectId) -> Result<bool, OperationError> {
        self.open_handles
            .is_delete_pending(object_id)
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))
    }

    fn stat(&self, path: &str) -> Result<openfs_format::MetadataRecord, OperationError> {
        self.session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?
            .stat(path)
            .map_err(map_core_error)
    }

}

fn map_core_error(error: CoreError) -> OperationError {
    match error {
        CoreError::InvalidInput("path component does not exist") => {
            OperationError::Win32(ERROR_FILE_NOT_FOUND)
        }
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

impl<'a, 'b: 'a, D: BlockDevice + Send + 'static> FileSystemHandler<'a, 'b> for OpenFsHandler<D> {
    type Context = FileHandle;

    fn create_file(
        &'b self,
        file_name: &U16CStr,
        _security_context: &dokan::DOKAN_IO_SECURITY_CONTEXT,
        _desired_access: winapi::um::winnt::ACCESS_MASK,
        _file_attributes: u32,
        _share_access: u32,
        create_disposition: u32,
        create_options: u32,
        _info: &mut OperationInfo<'a, 'b, Self>,
    ) -> Result<CreateFileInfo<Self::Context>, OperationError> {
        let path = path_from_windows(file_name)?;
        if path == "/" {
            let record = self.stat("/")?;
            self.register_open_handle(record.id)?;
            return Ok(CreateFileInfo {
                context: record.id,
                is_dir: true,
                new_file_created: false,
            });
        }

        let wants_directory = (create_options & 0x0000_0001) != 0;
        // Only a genuine missing path permits CREATE_* to publish a new entry.
        // Corruption and I/O failures must never be mistaken for absence.
        // Keep lookup and the resulting create/truncate under one session lock.
        // Otherwise two concurrent CREATE_NEW/OPEN_ALWAYS callbacks can both observe
        // a missing path before either publishes it (a filesystem-level TOCTOU race).
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let existing = session.stat(&path);
        let (is_dir, new_file_created, object_id) = match existing {
            Ok(record) => {
                if self.is_delete_pending(record.id)? {
                    return Err(OperationError::Win32(ERROR_ACCESS_DENIED));
                }
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
                    session.set_file_len(&path, 0).map_err(map_core_error)?;
                }
                (wants_directory, false, record.id)
            }
            Err(CoreError::InvalidInput("path component does not exist")) => {
                if create_disposition == OPEN_EXISTING || create_disposition == TRUNCATE_EXISTING {
                    return Err(OperationError::Win32(ERROR_FILE_NOT_FOUND));
                }
                if !matches!(create_disposition, CREATE_NEW | CREATE_ALWAYS | OPEN_ALWAYS) {
                    return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
                }
                let (parent, name) = split_parent(&path)?;
                let object_id = if wants_directory {
                    session
                        .create_directory_auto(parent, name)
                        .map_err(map_core_error)?
                } else {
                    session
                        .create_file_auto(parent, name)
                        .map_err(map_core_error)?
                };
                (wants_directory, true, object_id)
            }
            Err(error) => return Err(map_core_error(error)),
        };

        self.register_open_handle(object_id)?;
        Ok(CreateFileInfo {
            context: object_id,
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
        // Dokan calls Cleanup when the last handle for this file object closes.
        // Flush ordinary closes too, not only delete-on-close, so completed file
        // copies are pushed to the device before Explorer releases the handle.
        // Keep delete + flush under one lock to serialize them with writes/renames.
        let result = self
            .session
            .lock()
            .map_err(|_| "OpenFS session lock was poisoned".to_owned())
            .and_then(|mut session| {
                let should_delete = self
                    .open_handles
                    .close(*context, info.delete_on_close())
                    .map_err(str::to_owned)?;
                if should_delete {
                    let path = session
                        .path_for_object(*context)
                        .map_err(|error| error.to_string())?;
                    session
                        .remove(&path)
                        .map_err(|error| format!("delete-on-close for '{path}' failed: {error}"))?;
                    self.open_handles
                        .complete_delete(*context)
                        .map_err(str::to_owned)?;
                }
                session
                    .flush()
                    .map_err(|error| format!("flush during handle cleanup failed: {error}"))
            });
        if let Err(error) = result {
            eprintln!("OpenFS: {error}");
            self.deferred_error.record(error);
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
        let bytes = self.with_path(context, |session, path| {
            session.read_file_range(path, offset as u64, buffer.len())
        })?;
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
        let written = self.with_path(context, |session, path| {
            let write_offset = if info.write_to_eof() {
                session.stat(path)?.size
            } else {
                if offset < 0 {
                    return Err(CoreError::InvalidInput("negative write offset"));
                }
                offset as u64
            };
            session.write_file_range(path, write_offset, buffer)?;
            Ok(buffer.len())
        })?;
        Ok(written as u32)
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
        self.with_path(context, |session, path| session.stat(path))
            .map(|record| make_file_info(&record))
    }

    fn find_files(
        &'b self,
        _file_name: &U16CStr,
        mut fill_find_data: impl FnMut(&FindData) -> Result<(), dokan::FillDataError>,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        let entries = self.with_path(context, |session, path| {
            let mut entries = Vec::new();
            let mut marker: Option<String> = None;
            loop {
                let page = session.read_directory(path, marker.as_deref(), 256)?;
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
            let mut rows = Vec::with_capacity(entries.len());
            for entry in entries {
                let child_path = if path == "/" {
                    format!("/{}", entry.name)
                } else {
                    format!("{}/{}", path, entry.name)
                };
                let record = session.stat(&child_path)?;
                rows.push((entry, record.size));
            }
            Ok(rows)
        })?;

        for (entry, file_size) in entries {
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
                file_size,
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
        self.with_path(context, |session, path| {
            let record = session.stat(path)?;
            if record.kind != ObjectKind::File {
                return Err(CoreError::InvalidInput("path is not a regular file"));
            }
            Ok(())
        })
    }

    fn delete_directory(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let path = session
            .path_for_object(*context)
            .map_err(map_core_error)?;
        let record = session.stat(&path).map_err(map_core_error)?;
        if record.kind != ObjectKind::Directory {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        if !session
            .read_directory(&path, None, 1)
            .map_err(map_core_error)?
            .is_empty()
        {
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
        let new_path = path_from_windows(new_file_name)?;
        let (new_parent, new_name) = split_parent(&new_path)?;
        let mut session = self
            .session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))?;
        let old_path = session
            .path_for_object(*context)
            .map_err(map_core_error)?;
        match session.stat(&new_path) {
            Ok(_) if !replace_if_existing => {
                return Err(OperationError::Win32(ERROR_ALREADY_EXISTS));
            }
            Ok(destination) => {
                if destination.id != *context
                    && (self.has_open_handle(destination.id)?
                        || self.is_delete_pending(destination.id)?)
                {
                    return Err(OperationError::Win32(ERROR_SHARING_VIOLATION));
                }
                session
                    .replace_file(&old_path, new_parent, new_name)
                    .map_err(map_core_error)?;
                return Ok(());
            }
            Err(CoreError::InvalidInput("path component does not exist")) => {}
            Err(error) => return Err(map_core_error(error)),
        }
        session
            .rename(&old_path, new_parent, new_name)
            .map_err(map_core_error)?;
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
        self.with_path(context, |session, path| {
            session.set_file_len(path, offset as u64)
        })
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
    mount_device(device, image_bytes, mount_point, block_size, block_size)
}

/// Mount an already formatted whole-disk OpenFS volume from an offline USB disk.
///
/// The caller must have verified the disk identity and that Windows has taken it
/// offline. The device is opened read/write because journal recovery and file
/// operations may write to it.
pub fn mount_usb_device(
    disk_index: u32,
    unique_id: &str,
    mount_point: &str,
    block_size: u32,
) -> Result<(), Box<dyn StdError>> {
    let (size_bytes, logical_sector_size) =
        super::windows_device::verify_offline_usb_disk(disk_index, unique_id)?;
    if block_size < logical_sector_size || block_size % logical_sector_size != 0 {
        return Err(format!(
            "OpenFS block size {block_size} must be a multiple of the USB disk logical sector size {logical_sector_size}"
        ).into());
    }
    if size_bytes % u64::from(block_size) != 0 {
        return Err("USB disk capacity is not a whole multiple of the selected OpenFS block size".into());
    }
    let device_path = format!(r"\\.\PhysicalDrive{disk_index}");
    let device = RawDevice::open(
        &device_path,
        block_size,
        size_bytes / u64::from(block_size),
    )?;
    // Recheck after opening the handle so a disk-index change during the
    // preflight/open window cannot silently redirect the mount to another disk.
    let confirmed = super::windows_device::verify_offline_usb_disk(disk_index, unique_id)?;
    if confirmed != (size_bytes, logical_sector_size) {
        return Err("USB disk identity or geometry changed while opening it; refusing to mount".into());
    }
    mount_device(device, size_bytes, mount_point, block_size, logical_sector_size)
}

fn mount_device<D: BlockDevice + Send + 'static>(
    device: D,
    total_bytes: u64,
    mount_point: &str,
    block_size: u32,
    sector_size: u32,
) -> Result<(), Box<dyn StdError>> {
    let session = VolumeSession::open(device)?;
    let handler = OpenFsHandler::new(session, total_bytes);

    let mount_text = if mount_point.ends_with(':') {
        format!("{mount_point}\\")
    } else {
        mount_point.to_owned()
    };
    let mount_point = U16CString::from_str(&mount_text)?;
    let mut drive = dokan::Drive::new();
    drive
        .mount_point(&mount_point)
        .sector_size(sector_size)
        .allocation_unit_size(block_size)
        .flags(MountFlags::CURRENT_SESSION | MountFlags::ENABLE_FCB_GARBAGE_COLLECTION);

    // Drive::mount blocks until the Dokan volume is unmounted. Always attempt
    // the final flush, even if the mount loop reports an error.
    let mount_result = drive.mount(&handler);
    let flush_result = handler
        .session
        .lock()
        .map_err(|_| "OpenFS session lock was poisoned")?
        .flush();
    let deferred_error = handler.deferred_error.take()?;

    mount_result?;
    flush_result?;
    if let Some(error) = deferred_error {
        return Err(format!("one or more file-handle cleanup operations failed: {error}").into());
    }
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
    let result = dokan::unmount(&mount_point);
    if !result {
        return Err(format!("Dokan could not unmount {mount_text}").into());
    }
    println!("Unmount request sent to {mount_text}.");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{DeferredErrorLatch, OpenHandleState};
    use openfs_format::ObjectId;

    #[test]
    fn delete_on_close_waits_for_every_open_handle() {
        let state = OpenHandleState::default();
        let object = ObjectId(42);
        state.register(object).unwrap();
        state.register(object).unwrap();

        assert!(!state.close(object, true).unwrap());
        assert!(state.is_open(object).unwrap());
        assert!(state.is_delete_pending(object).unwrap());
        assert!(state.register(object).is_err());

        assert!(state.close(object, false).unwrap());
        assert!(!state.is_open(object).unwrap());
        assert!(state.is_delete_pending(object).unwrap());
        state.complete_delete(object).unwrap();
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn ordinary_close_does_not_mark_object_for_deletion() {
        let state = OpenHandleState::default();
        let object = ObjectId(7);
        state.register(object).unwrap();
        assert!(!state.close(object, false).unwrap());
        assert!(!state.is_open(object).unwrap());
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn concurrent_open_and_close_keep_handle_accounting_consistent() {
        use std::sync::{Arc, Barrier};
        use std::thread;

        let state = Arc::new(OpenHandleState::default());
        let object = ObjectId(99);
        let barrier = Arc::new(Barrier::new(9));

        thread::scope(|scope| {
            for _ in 0..8 {
                let state = Arc::clone(&state);
                let barrier = Arc::clone(&barrier);
                scope.spawn(move || {
                    barrier.wait();
                    for _ in 0..256 {
                        state.register(object).unwrap();
                        assert!(!state.close(object, false).unwrap());
                    }
                });
            }
            barrier.wait();
        });

        assert!(!state.is_open(object).unwrap());
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn closing_an_unregistered_handle_is_rejected_without_marking_delete() {
        let state = OpenHandleState::default();
        let object = ObjectId(100);

        assert!(state.close(object, true).is_err());
        assert!(!state.is_open(object).unwrap());
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn deferred_error_latch_retains_all_cleanup_failures_until_taken() {
        let latch = DeferredErrorLatch::default();
        latch.record("first flush failed".to_owned());
        latch.record("delete-on-close failed".to_owned());

        assert_eq!(
            latch.take().unwrap().as_deref(),
            Some("first flush failed; delete-on-close failed")
        );
        assert_eq!(latch.take().unwrap(), None);
    }
}

