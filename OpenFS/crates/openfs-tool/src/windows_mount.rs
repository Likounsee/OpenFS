//! Windows user-mode mount adapter backed by Dokany.
//!
//! Dokany is an optional host integration: OpenFS remains the on-disk filesystem.
//! This module maps Windows file operations to the portable VolumeSession API.

use std::collections::{HashMap, HashSet};
use std::error::Error as StdError;
use std::path::Path;
use std::sync::{Mutex, MutexGuard};
use std::time::SystemTime;

use dokan::{
    CreateFileInfo, DiskSpaceInfo, FileInfo, FileSystemHandler, FindData, MountFlags,
    OperationError, OperationInfo, VolumeInfo,
};
use openfs_block::{BlockDevice, ImageDevice, RawDevice};
use openfs_core::Error as CoreError;
use openfs_format::{ObjectId, ObjectKind, VolumeSession};
use openfs_partition::{read_gpt, GptLayout, PartitionDevice, OPENFS_PARTITION_TYPE_GUID};
use widestring::{U16CStr, U16CString};
use winapi::shared::winerror::{
    ERROR_ACCESS_DENIED, ERROR_ALREADY_EXISTS, ERROR_DIR_NOT_EMPTY, ERROR_FILE_EXISTS,
    ERROR_FILE_NOT_FOUND, ERROR_GEN_FAILURE, ERROR_INVALID_PARAMETER, ERROR_SHARING_VIOLATION,
};
use winapi::um::winnt::{
    FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_NORMAL, FILE_CASE_PRESERVED_NAMES,
    FILE_CASE_SENSITIVE_SEARCH, FILE_UNICODE_ON_DISK,
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct FileHandle {
    object_id: ObjectId,
    handle_id: u64,
}

// Dokan's CreateFile callback supplies native NT disposition values,
 // not Win32 CREATE_* values (which use a different numbering).
const FILE_OPEN: u32 = 1;
const FILE_CREATE: u32 = 2;
const FILE_OPEN_IF: u32 = 3;
const FILE_OVERWRITE: u32 = 4;
const FILE_OVERWRITE_IF: u32 = 5;

const SHARE_READ: u32 = 1;
const SHARE_WRITE: u32 = 2;
const SHARE_DELETE: u32 = 4;
const SHARE_VALID_MASK: u32 = SHARE_READ | SHARE_WRITE | SHARE_DELETE;
const ACCESS_READ: u8 = 1;
const ACCESS_WRITE: u8 = 2;
const ACCESS_DELETE: u8 = 4;
const GENERIC_ALL: u32 = 0x1000_0000;
const GENERIC_EXECUTE: u32 = 0x2000_0000;
const GENERIC_WRITE: u32 = 0x4000_0000;
const GENERIC_READ: u32 = 0x8000_0000;
const MAXIMUM_ALLOWED: u32 = 0x0200_0000;
const FILE_READ_DATA: u32 = 1;
const FILE_WRITE_DATA: u32 = 2;
const FILE_APPEND_DATA: u32 = 4;
const FILE_READ_EA: u32 = 8;
const FILE_WRITE_EA: u32 = 0x10;
const FILE_EXECUTE: u32 = 0x20;
const DELETE: u32 = 0x1_0000;
const FILE_READ_ATTRIBUTES: u32 = 0x80;
const FILE_WRITE_ATTRIBUTES: u32 = 0x100;

fn access_categories(desired_access: u32) -> u8 {
    let mut result = 0;
    if desired_access
        & (GENERIC_READ
            | GENERIC_EXECUTE
            | GENERIC_ALL
            | MAXIMUM_ALLOWED
            | FILE_READ_DATA
            | FILE_READ_EA
            | FILE_READ_ATTRIBUTES
            | FILE_EXECUTE)
        != 0
    {
        result |= ACCESS_READ;
    }
    if desired_access
        & (GENERIC_WRITE
            | GENERIC_ALL
            | MAXIMUM_ALLOWED
            | FILE_WRITE_DATA
            | FILE_APPEND_DATA
            | FILE_WRITE_EA
            | FILE_WRITE_ATTRIBUTES)
        != 0
    {
        result |= ACCESS_WRITE;
    }
    if desired_access & (DELETE | GENERIC_ALL | MAXIMUM_ALLOWED) != 0 {
        result |= ACCESS_DELETE;
    }
    result
}

#[derive(Debug, Clone, Copy)]
struct OpenHandleRecord {
    handle_id: u64,
    desired_access: u32,
    access: u8,
    share_access: u32,
}

#[derive(Default)]
struct OpenHandleState {
    inner: Mutex<OpenHandleInner>,
}

#[derive(Default)]
struct DeferredErrorLatch {
    errors: Mutex<Vec<String>>,
}

impl DeferredErrorLatch {
    fn record(&self, error: String) {
        let mut errors = self
            .errors
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        errors.push(error);
    }

    fn take(&self) -> Result<Option<String>, std::io::Error> {
        self.errors
            .lock()
            .map(|mut errors| {
                if errors.is_empty() {
                    None
                } else {
                    Some(errors.drain(..).collect::<Vec<_>>().join("; "))
                }
            })
            .map_err(|_| std::io::Error::other("OpenFS deferred error latch lock was poisoned"))
    }
}

#[derive(Default)]
struct OpenHandleInner {
    handles: HashMap<ObjectId, Vec<OpenHandleRecord>>,
    delete_pending: HashSet<ObjectId>,
    next_handle_id: u64,
}

impl OpenHandleState {
    fn lock(&self) -> Result<MutexGuard<'_, OpenHandleInner>, &'static str> {
        self.inner
            .lock()
            .map_err(|_| "OpenFS open-handle state lock was poisoned")
    }

    fn register(
        &self,
        object_id: ObjectId,
        desired_access: u32,
        share_access: u32,
    ) -> Result<FileHandle, &'static str> {
        if share_access & !SHARE_VALID_MASK != 0 {
            return Err("unsupported Windows share-access flags");
        }
        let access = access_categories(desired_access);
        let mut inner = self.lock()?;
        if inner.delete_pending.contains(&object_id) {
            return Err("object is pending deletion");
        }
        if let Some(existing) = inner.handles.get(&object_id) {
            for open in existing {
                let new_access_denied = (access & ACCESS_READ != 0
                    && open.share_access & SHARE_READ == 0)
                    || (access & ACCESS_WRITE != 0 && open.share_access & SHARE_WRITE == 0)
                    || (access & ACCESS_DELETE != 0 && open.share_access & SHARE_DELETE == 0);
                let existing_access_denied = (open.access & ACCESS_READ != 0
                    && share_access & SHARE_READ == 0)
                    || (open.access & ACCESS_WRITE != 0 && share_access & SHARE_WRITE == 0)
                    || (open.access & ACCESS_DELETE != 0 && share_access & SHARE_DELETE == 0);
                if new_access_denied || existing_access_denied {
                    return Err("Windows file sharing violation");
                }
            }
        }
        let handle_id = inner
            .next_handle_id
            .checked_add(1)
            .ok_or("OpenFS handle identifier overflow")?;
        inner.next_handle_id = handle_id;
        inner
            .handles
            .entry(object_id)
            .or_default()
            .push(OpenHandleRecord {
                handle_id,
                desired_access,
                access,
                share_access,
            });
        Ok(FileHandle {
            object_id,
            handle_id,
        })
    }

    fn close(&self, handle: FileHandle, delete_on_close: bool) -> Result<bool, &'static str> {
        let mut inner = self
            .inner
            .lock()
            .map_err(|_| "OpenFS open-handle state lock was poisoned")?;
        let Some(opens) = inner.handles.get(&handle.object_id) else {
            return Err("cannot close an object without a registered open handle");
        };
        if !opens.iter().any(|open| open.handle_id == handle.handle_id) {
            return Err("cannot close an unknown or already closed handle");
        }
        if delete_on_close {
            inner.delete_pending.insert(handle.object_id);
        }
        let opens = inner
            .handles
            .get_mut(&handle.object_id)
            .expect("validated handle entry remains present");
        let index = opens
            .iter()
            .position(|open| open.handle_id == handle.handle_id)
            .expect("validated handle remains present");
        opens.remove(index);
        if opens.is_empty() {
            inner.handles.remove(&handle.object_id);
        }
        Ok(!inner.handles.contains_key(&handle.object_id)
            && inner.delete_pending.contains(&handle.object_id))
    }

    fn allows_access(
        &self,
        handle: FileHandle,
        requested_access: u32,
    ) -> Result<bool, &'static str> {
        let inner = self.lock()?;
        let record = inner
            .handles
            .get(&handle.object_id)
            .and_then(|opens| opens.iter().find(|open| open.handle_id == handle.handle_id))
            .ok_or("unknown or closed OpenFS handle")?;
        let generic_access = match requested_access {
            FILE_READ_DATA => GENERIC_READ | GENERIC_ALL | MAXIMUM_ALLOWED,
            FILE_READ_ATTRIBUTES => GENERIC_READ | GENERIC_EXECUTE | GENERIC_ALL | MAXIMUM_ALLOWED,
            FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES => {
                GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED
            }
            DELETE => GENERIC_ALL | MAXIMUM_ALLOWED,
            _ => 0,
        };
        Ok(record.desired_access & (requested_access | generic_access) != 0)
    }

    fn is_open(&self, object_id: ObjectId) -> Result<bool, &'static str> {
        let inner = self.lock()?;
        Ok(inner
            .handles
            .get(&object_id)
            .is_some_and(|opens| !opens.is_empty()))
    }

    fn is_delete_pending(&self, object_id: ObjectId) -> Result<bool, &'static str> {
        Ok(self.lock()?.delete_pending.contains(&object_id))
    }

    fn complete_delete(&self, object_id: ObjectId) -> Result<(), &'static str> {
        self.lock()?.delete_pending.remove(&object_id);
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
    fn lock_session(&self) -> Result<MutexGuard<'_, VolumeSession<D>>, OperationError> {
        self.session
            .lock()
            .map_err(|_| OperationError::Win32(ERROR_GEN_FAILURE))
    }

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
        let mut session = self.lock_session()?;
        let path = session
            .path_for_object(context.object_id)
            .map_err(map_core_error)?;
        operation(&mut session, &path).map_err(map_core_error)
    }

    fn register_open_handle(
        &self,
        object_id: ObjectId,
        desired_access: u32,
        share_access: u32,
    ) -> Result<FileHandle, OperationError> {
        self.open_handles
            .register(object_id, desired_access, share_access)
            .map_err(|error| match error {
                "Windows file sharing violation" => OperationError::Win32(ERROR_SHARING_VIOLATION),
                "object is pending deletion" => OperationError::Win32(ERROR_ACCESS_DENIED),
                "unsupported Windows share-access flags" => {
                    OperationError::Win32(ERROR_INVALID_PARAMETER)
                }
                _ => OperationError::Win32(ERROR_GEN_FAILURE),
            })
    }

    fn require_handle_access(
        &self,
        handle: FileHandle,
        requested_access: u32,
    ) -> Result<(), OperationError> {
        match self.open_handles.allows_access(handle, requested_access) {
            Ok(true) => Ok(()),
            Ok(false) => Err(OperationError::Win32(ERROR_ACCESS_DENIED)),
            Err(_) => Err(OperationError::Win32(ERROR_GEN_FAILURE)),
        }
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
        self.lock_session()?.stat(path).map_err(map_core_error)
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
        desired_access: winapi::um::winnt::ACCESS_MASK,
        _file_attributes: u32,
        share_access: u32,
        create_disposition: u32,
        create_options: u32,
        _info: &mut OperationInfo<'a, 'b, Self>,
    ) -> Result<CreateFileInfo<Self::Context>, OperationError> {
        if share_access & !SHARE_VALID_MASK != 0 {
            return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
        }
        let path = path_from_windows(file_name)?;
        if path == "/" {
            let record = self.stat("/")?;
            let handle = self.register_open_handle(record.id, desired_access, share_access)?;
            return Ok(CreateFileInfo {
                context: handle,
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
        let mut session = self.lock_session()?;
        let existing = session.stat(&path);
        let (is_dir, new_file_created, object_id, registered_handle) = match existing {
            Ok(record) => {
                if self.is_delete_pending(record.id)? {
                    return Err(OperationError::Win32(ERROR_ACCESS_DENIED));
                }
                if create_disposition == FILE_CREATE {
                    return Err(OperationError::Win32(ERROR_FILE_EXISTS));
                }
                // FILE_DIRECTORY_FILE is a constraint, not a requirement that
                // callers must always provide when opening an existing directory.
                if wants_directory && record.kind != ObjectKind::Directory {
                    return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
                }
                let is_dir = record.kind == ObjectKind::Directory;
                let handle = self.register_open_handle(record.id, desired_access, share_access)?;
                if create_disposition == FILE_OVERWRITE || create_disposition == FILE_OVERWRITE_IF {
                    if is_dir {
                        let _ = self.open_handles.close(handle, false);
                        return Err(OperationError::Win32(ERROR_INVALID_PARAMETER));
                    }
                    // Truncation changes file contents and therefore requires an
                    // actual write-data right; FILE_APPEND_DATA alone is insufficient.
                    const MAXIMUM_ALLOWED_ACCESS: u32 = 0x0200_0000;
                    if desired_access
                        & (FILE_WRITE_DATA | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED_ACCESS)
                        == 0
                    {
                        let _ = self.open_handles.close(handle, false);
                        return Err(OperationError::Win32(ERROR_ACCESS_DENIED));
                    }
                    if let Err(error) = session.set_file_len(&path, 0) {
                        let _ = self.open_handles.close(handle, false);
                        return Err(map_core_error(error));
                    }
                }
                (is_dir, false, record.id, Some(handle))
            }
            Err(CoreError::InvalidInput("path component does not exist")) => {
                if create_disposition == FILE_OPEN || create_disposition == FILE_OVERWRITE {
                    return Err(OperationError::Win32(ERROR_FILE_NOT_FOUND));
                }
                if !matches!(create_disposition, FILE_CREATE | FILE_OPEN_IF | FILE_OVERWRITE_IF) {
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
                (wants_directory, true, object_id, None)
            }
            Err(error) => return Err(map_core_error(error)),
        };

        let handle = match registered_handle {
            Some(handle) => handle,
            None => match self.register_open_handle(object_id, desired_access, share_access) {
                Ok(handle) => handle,
                Err(error) => {
                    if new_file_created {
                        if let Err(cleanup_error) = session.remove(&path) {
                            self.deferred_error.record(format!(
                                "rollback of newly created '{path}' after handle registration failure failed: {cleanup_error}"
                            ));
                        }
                    }
                    return Err(error);
                }
            },
        };
        Ok(CreateFileInfo {
            context: handle,
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
                        .path_for_object(context.object_id)
                        .map_err(|error| error.to_string())?;
                    session
                        .remove(&path)
                        .map_err(|error| format!("delete-on-close for '{path}' failed: {error}"))?;
                    self.open_handles
                        .complete_delete(context.object_id)
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
        self.require_handle_access(*context, FILE_READ_DATA)?;
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
        let required_access = if info.write_to_eof() {
            FILE_APPEND_DATA
        } else {
            FILE_WRITE_DATA
        };
        self.require_handle_access(*context, required_access)?;
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
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        self.require_handle_access(*context, FILE_WRITE_DATA)?;
        self.lock_session()?.flush().map_err(map_core_error)
    }

    fn get_file_information(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<FileInfo, OperationError> {
        self.require_handle_access(*context, FILE_READ_ATTRIBUTES)?;
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
        self.require_handle_access(*context, FILE_READ_DATA)?;
        let mut session = self.lock_session()?;
        let path = session
            .path_for_object(context.object_id)
            .map_err(map_core_error)?;
        let mut fill_error = None;
        session
            .visit_directory(&path, |entry, record| {
                let file_name = match U16CString::from_str(&entry.name) {
                    Ok(name) => name,
                    Err(_) => {
                        fill_error = Some(OperationError::Win32(ERROR_INVALID_PARAMETER));
                        return false;
                    }
                };
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
                    file_size: record.size,
                    file_name,
                };
                if let Err(error) = fill_find_data(&data) {
                    fill_error = Some(error.into());
                    return false;
                }
                true
            })
            .map_err(map_core_error)?;
        if let Some(error) = fill_error {
            return Err(error);
        }
        Ok(())
    }

    fn delete_file(
        &'b self,
        _file_name: &U16CStr,
        _info: &OperationInfo<'a, 'b, Self>,
        context: &'a Self::Context,
    ) -> Result<(), OperationError> {
        self.require_handle_access(*context, DELETE)?;
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
        self.require_handle_access(*context, DELETE)?;
        let mut session = self.lock_session()?;
        let path = session
            .path_for_object(context.object_id)
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
        self.require_handle_access(*context, DELETE)?;
        let new_path = path_from_windows(new_file_name)?;
        let (new_parent, new_name) = split_parent(&new_path)?;
        let mut session = self.lock_session()?;
        let old_path = session
            .path_for_object(context.object_id)
            .map_err(map_core_error)?;
        match session.stat(&new_path) {
            Ok(_) if !replace_if_existing => {
                return Err(OperationError::Win32(ERROR_ALREADY_EXISTS));
            }
            Ok(destination) => {
                if destination.id != context.object_id
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
        self.require_handle_access(*context, FILE_WRITE_DATA)?;
        self.with_path(context, |session, path| {
            session.set_file_len(path, offset as u64)
        })
    }

    fn get_disk_free_space(
        &'b self,
        _info: &OperationInfo<'a, 'b, Self>,
    ) -> Result<DiskSpaceInfo, OperationError> {
        let mut session = self.lock_session()?;
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
        let session = self.lock_session()?;
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

    // GPT images use 512-byte logical sectors independently of the OpenFS
    // allocation block size. Probe GPT first, then retain compatibility with
    // legacy whole-image Raw volumes.
    for sector_size in [512_u32, 1024_u32, 2048_u32, 4096_u32] {
        if image_bytes % u64::from(sector_size) != 0 {
            continue;
        }
        let sector_count = image_bytes / u64::from(sector_size);
        if sector_count < 6 {
            continue;
        }
        let mut disk = ImageDevice::open(image_path, sector_size)?;
        if let Ok(layout) = read_gpt(&mut disk) {
            let partition = unique_openfs_partition(&layout)?;
            let partition_bytes = partition
                .sector_count()?
                .checked_mul(u64::from(sector_size))
                .ok_or("OpenFS partition size overflow")?;
            let device = PartitionDevice::new(
                disk,
                partition.first_lba,
                partition.sector_count()?,
                block_size,
            )?;
            return mount_device(
                device,
                partition_bytes,
                mount_point,
                block_size,
                sector_size,
            );
        }
    }

    let device = ImageDevice::open(image_path, block_size)?;
    mount_device(device, image_bytes, mount_point, block_size, block_size)
}

/// Mount an already formatted whole-disk OpenFS volume from a physical disk.
///
/// Mounted host volumes are dismounted during preflight; the physical disk remains
/// online. The exclusive raw-device handle is retained for the lifetime of the mount.
pub fn mount_disk_device(
    disk_index: u32,
    unique_id: &str,
    expected_model: &str,
    expected_serial: &str,
    expected_size: u64,
    mount_point: &str,
    block_size: u32,
) -> Result<(), Box<dyn StdError>> {
    let (initial_size, initial_sector_size) =
        super::windows_device::verify_disk_identity_geometry(disk_index, unique_id)?;
    if block_size < initial_sector_size || block_size % initial_sector_size != 0 {
        return Err(format!(
            "OpenFS block size {block_size} must be a multiple of the disk logical sector size {initial_sector_size}"
        ).into());
    }
    if initial_size % u64::from(initial_sector_size) != 0 {
        return Err("disk capacity is not a whole multiple of the logical sector size".into());
    }

    let (size_bytes, logical_sector_size) = super::windows_device::prepare_disk_for_raw_access(
        disk_index,
        expected_model,
        expected_serial,
        expected_size,
    )?;
    if (size_bytes, logical_sector_size) != (initial_size, initial_sector_size) {
        return Err("disk geometry changed during mount preflight; refusing to mount".into());
    }
    let device_path = format!(r"\\.\PhysicalDrive{disk_index}");
    let sector_count = size_bytes / u64::from(logical_sector_size);
    let mut disk = RawDevice::open(&device_path, logical_sector_size, sector_count)?;

    // Recheck the full physical-media identity after opening the exclusive handle.
    // A disk index or generic Windows UniqueId alone is not sufficient.
    let confirmed = super::windows_device::verify_usb_identity(
        disk_index,
        expected_model,
        expected_serial,
        expected_size,
    )?;
    if confirmed != (size_bytes, logical_sector_size) {
        return Err("disk identity or geometry changed while opening it; refusing to mount".into());
    }
    let confirmed_unique_id =
        super::windows_device::verify_disk_identity_geometry(disk_index, unique_id)?;
    if confirmed_unique_id != (size_bytes, logical_sector_size) {
        return Err("disk UniqueId or geometry changed while opening it; refusing to mount".into());
    }

    match read_gpt(&mut disk) {
        Ok(layout) => {
            let partition = unique_openfs_partition(&layout)?;
            let partition_bytes = partition
                .sector_count()?
                .checked_mul(u64::from(logical_sector_size))
                .ok_or("OpenFS partition size overflow")?;
            let device = PartitionDevice::new(
                disk,
                partition.first_lba,
                partition.sector_count()?,
                block_size,
            )?;
            println!(
                "Mounting OpenFS GPT partition {} at LBA {}..={}",
                partition.entry_index, partition.first_lba, partition.last_lba
            );
            mount_device(
                device,
                partition_bytes,
                mount_point,
                block_size,
                logical_sector_size,
            )
        }
        Err(gpt_error) => {
            // Preserve support for existing whole-disk Raw volumes. Do not
            // silently fall back when a protective MBR says a GPT is present.
            let mut first_sector = vec![0_u8; logical_sector_size as usize];
            disk.read_blocks(0, &mut first_sector)?;
            let has_protective_mbr = first_sector.get(510..512) == Some(&[0x55, 0xaa])
                && (0..4).any(|index| first_sector[446 + index * 16 + 4] == 0xee);
            if has_protective_mbr {
                return Err(format!(
                    "disk has a protective MBR but its GPT is invalid: {gpt_error}"
                )
                .into());
            }
            drop(disk);
            let raw_block_count = size_bytes / u64::from(block_size);
            let device = RawDevice::open(&device_path, block_size, raw_block_count)?;
            println!("No valid GPT found; mounting legacy whole-disk Raw OpenFS volume.");
            mount_device(
                device,
                size_bytes,
                mount_point,
                block_size,
                logical_sector_size,
            )
        }
    }
}

fn unique_openfs_partition(
    layout: &GptLayout,
) -> Result<openfs_partition::GptPartition, Box<dyn StdError>> {
    let mut matches = layout
        .partitions
        .iter()
        .filter(|partition| partition.type_guid == OPENFS_PARTITION_TYPE_GUID);
    let partition = matches
        .next()
        .cloned()
        .ok_or("GPT contains no partition with the OpenFS type GUID")?;
    if matches.next().is_some() {
        return Err("GPT contains multiple OpenFS partitions; refusing ambiguous mount".into());
    }
    Ok(partition)
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
        .flags(
            MountFlags::MOUNT_MANAGER
                | MountFlags::CURRENT_SESSION
                | MountFlags::ENABLE_FCB_GARBAGE_COLLECTION,
        );

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
    #[test]
    fn dokan_create_disposition_constants_use_native_nt_values() {
        assert_eq!(super::FILE_OPEN, 1);
        assert_eq!(super::FILE_CREATE, 2);
        assert_eq!(super::FILE_OPEN_IF, 3);
        assert_eq!(super::FILE_OVERWRITE, 4);
        assert_eq!(super::FILE_OVERWRITE_IF, 5);
    }

    use super::{
        access_categories, DeferredErrorLatch, OpenHandleState, ACCESS_DELETE, ACCESS_READ,
        ACCESS_WRITE, DELETE, FILE_APPEND_DATA, FILE_READ_ATTRIBUTES, FILE_READ_DATA,
        FILE_WRITE_ATTRIBUTES, FILE_WRITE_DATA, GENERIC_EXECUTE, GENERIC_READ, GENERIC_WRITE,
        MAXIMUM_ALLOWED, SHARE_DELETE, SHARE_READ, SHARE_WRITE,
    };
    use openfs_format::ObjectId;

    #[test]
    fn desired_access_is_grouped_into_windows_share_categories() {
        assert_eq!(
            access_categories(FILE_READ_DATA | GENERIC_READ),
            ACCESS_READ
        );
        assert_eq!(
            access_categories(FILE_WRITE_DATA | GENERIC_WRITE),
            ACCESS_WRITE
        );
        assert_eq!(access_categories(DELETE), ACCESS_DELETE);
        assert_eq!(access_categories(FILE_APPEND_DATA), ACCESS_WRITE);
        assert_eq!(
            access_categories(MAXIMUM_ALLOWED),
            ACCESS_READ | ACCESS_WRITE | ACCESS_DELETE
        );
    }

    #[test]
    fn operation_access_is_checked_against_the_exact_open_handle() {
        let state = OpenHandleState::default();
        let object = ObjectId(41);
        let read_only = state
            .register(object, FILE_READ_DATA, SHARE_READ | SHARE_WRITE)
            .unwrap();
        assert!(state.allows_access(read_only, FILE_READ_DATA).unwrap());
        assert!(!state.allows_access(read_only, FILE_WRITE_DATA).unwrap());
        assert!(!state.allows_access(read_only, DELETE).unwrap());
        assert!(!state
            .allows_access(read_only, FILE_WRITE_ATTRIBUTES)
            .unwrap());
        assert!(!state.close(read_only, false).unwrap());

        let write_only = state
            .register(object, GENERIC_WRITE, SHARE_READ | SHARE_WRITE)
            .unwrap();
        assert!(!state.allows_access(write_only, FILE_READ_DATA).unwrap());
        assert!(state.allows_access(write_only, FILE_WRITE_DATA).unwrap());
        assert!(state.allows_access(write_only, FILE_APPEND_DATA).unwrap());
        assert!(!state.close(write_only, false).unwrap());
    }

    #[test]
    fn generic_execute_grants_metadata_access_without_file_data_read() {
        let state = OpenHandleState::default();
        let object = ObjectId(410);
        let execute_only = state
            .register(object, GENERIC_EXECUTE, SHARE_READ | SHARE_WRITE)
            .unwrap();
        assert!(state
            .allows_access(execute_only, FILE_READ_ATTRIBUTES)
            .unwrap());
        assert!(!state.allows_access(execute_only, FILE_READ_DATA).unwrap());
        assert!(!state.close(execute_only, false).unwrap());
    }

    #[test]
    fn compatible_readers_can_share_read_access() {
        let state = OpenHandleState::default();
        let object = ObjectId(42);
        let a = state.register(object, FILE_READ_DATA, SHARE_READ).unwrap();
        let b = state.register(object, GENERIC_READ, SHARE_READ).unwrap();
        assert!(!state.close(a, false).unwrap());
        assert!(!state.close(b, false).unwrap());
    }

    #[test]
    fn new_access_must_be_allowed_by_every_existing_share_mask() {
        let state = OpenHandleState::default();
        let object = ObjectId(43);
        let first = state.register(object, FILE_READ_DATA, SHARE_READ).unwrap();
        assert!(state
            .register(object, FILE_WRITE_DATA, SHARE_READ | SHARE_WRITE)
            .is_err());
        assert!(!state.close(first, false).unwrap());
    }

    #[test]
    fn new_share_mask_must_allow_every_existing_access_category() {
        let state = OpenHandleState::default();
        let object = ObjectId(44);
        let first = state
            .register(object, FILE_READ_DATA, SHARE_READ | SHARE_WRITE)
            .unwrap();
        assert!(state
            .register(object, FILE_WRITE_DATA, SHARE_WRITE)
            .is_err());
        assert!(!state.close(first, false).unwrap());
    }

    #[test]
    fn delete_access_requires_share_delete_in_both_directions() {
        let state = OpenHandleState::default();
        let object = ObjectId(45);
        let first = state
            .register(object, FILE_READ_DATA, SHARE_READ | SHARE_WRITE)
            .unwrap();
        assert!(state
            .register(object, DELETE, SHARE_READ | SHARE_WRITE)
            .is_err());
        assert!(!state.close(first, false).unwrap());
    }

    #[test]
    fn closing_one_handle_preserves_other_handle_share_constraints() {
        let state = OpenHandleState::default();
        let object = ObjectId(46);
        let first = state
            .register(object, FILE_READ_DATA, SHARE_READ | SHARE_WRITE)
            .unwrap();
        let second = state
            .register(object, FILE_WRITE_DATA, SHARE_READ | SHARE_WRITE)
            .unwrap();
        assert!(!state.close(first, false).unwrap());
        assert!(state.register(object, FILE_READ_DATA, SHARE_READ).is_err());
        assert!(!state.close(second, false).unwrap());
    }

    #[test]
    fn delete_on_close_waits_for_last_exact_handle_and_blocks_new_opens() {
        let state = OpenHandleState::default();
        let object = ObjectId(47);
        let first = state
            .register(
                object,
                FILE_READ_DATA,
                SHARE_READ | SHARE_WRITE | SHARE_DELETE,
            )
            .unwrap();
        let second = state
            .register(
                object,
                FILE_READ_DATA,
                SHARE_READ | SHARE_WRITE | SHARE_DELETE,
            )
            .unwrap();
        assert!(!state.close(first, true).unwrap());
        assert!(state.is_open(object).unwrap());
        assert!(state.is_delete_pending(object).unwrap());
        assert!(state.register(object, FILE_READ_DATA, SHARE_READ).is_err());
        assert!(state.close(second, false).unwrap());
        assert!(!state.is_open(object).unwrap());
        state.complete_delete(object).unwrap();
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn ordinary_close_does_not_mark_object_for_deletion() {
        let state = OpenHandleState::default();
        let object = ObjectId(7);
        let handle = state.register(object, 0, 0).unwrap();
        assert!(!state.close(handle, false).unwrap());
        assert!(!state.is_open(object).unwrap());
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn concurrent_open_close_keeps_per_handle_records_consistent() {
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
                        let handle = state.register(object, FILE_READ_DATA, SHARE_READ).unwrap();
                        assert!(!state.close(handle, false).unwrap());
                    }
                });
            }
            barrier.wait();
        });
        assert!(!state.is_open(object).unwrap());
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn unknown_or_already_closed_handle_is_rejected() {
        let state = OpenHandleState::default();
        let object = ObjectId(100);
        let fake = super::FileHandle {
            object_id: object,
            handle_id: 99,
        };
        assert!(state.close(fake, true).is_err());
        let handle = state.register(object, 0, 0).unwrap();
        assert!(!state.close(handle, false).unwrap());
        assert!(state.close(handle, true).is_err());
        assert!(!state.is_delete_pending(object).unwrap());
    }

    #[test]
    fn unsupported_share_bits_are_rejected() {
        assert!(OpenHandleState::default()
            .register(ObjectId(101), 0, 8)
            .is_err());
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
