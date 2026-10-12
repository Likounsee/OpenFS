//! In-memory namespace validation and lookup over decoded OpenFS records.
//!
//! This layer intentionally does not claim disk persistence or crash consistency.
//! It provides a deterministic logical model that a future journaled metadata store
//! can validate before committing changes to the block device.

use std::collections::BTreeMap;

use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};
use openfs_core::{Error, Result};

/// Logical namespace rooted at one directory object.
///
/// The initial implementation supports one parent per object (no hard links) and
/// case-sensitive names. Mutations are validated before either index is changed.
#[derive(Debug, Clone)]
pub struct Namespace {
    root_id: ObjectId,
    objects: BTreeMap<ObjectId, MetadataRecord>,
    names: BTreeMap<(ObjectId, String), DirectoryEntry>,
}

impl Namespace {
    /// Create an empty namespace with a canonical root directory.
    pub fn new(root_id: ObjectId) -> Result<Self> {
        ObjectId::new(root_id.0)?;
        let root = MetadataRecord {
            id: root_id,
            parent_id: 0,
            kind: ObjectKind::Directory,
            flags: 0,
            size: 0,
            extent_root_block: None,
            generation: 1,
        };
        let mut objects = BTreeMap::new();
        objects.insert(root_id, root);
        Ok(Self {
            root_id,
            objects,
            names: BTreeMap::new(),
        })
    }

    pub fn root_id(&self) -> ObjectId {
        self.root_id
    }

    pub fn object(&self, id: ObjectId) -> Option<&MetadataRecord> {
        self.objects.get(&id)
    }

    /// Find a child by exact, case-sensitive name.
    pub fn lookup(&self, parent_id: ObjectId, name: &str) -> Option<&DirectoryEntry> {
        self.names.get(&(parent_id, name.to_owned()))
    }

    /// Resolve a slash-separated path from the namespace root.
    ///
    /// Leading, trailing, and repeated slashes are accepted. Dot components are rejected
    /// rather than interpreted, and every intermediate component must be a directory.
    /// This resolves the in-memory model only; it does not read or persist disk state.
    pub fn resolve_path(&self, path: &str) -> Result<ObjectId> {
        let components: Vec<&str> = path
            .split('/')
            .filter(|component| !component.is_empty())
            .collect();
        let mut current = self.root_id;
        for (index, component) in components.iter().enumerate() {
            if *component == "." || *component == ".." {
                return Err(Error::InvalidInput("dot path components are not supported"));
            }
            let entry = self
                .lookup(current, component)
                .ok_or(Error::InvalidInput("path component does not exist"))?;
            let next = ObjectId::new(entry.object_id)?;
            if next != self.root_id && self.objects.get(&next).is_none() {
                return Err(Error::Corrupt("directory entry points to a missing object"));
            }
            current = next;
            if index + 1 < components.len() {
                let record = self
                    .objects
                    .get(&current)
                    .ok_or(Error::Corrupt("path object metadata is missing"))?;
                if record.kind != ObjectKind::Directory {
                    return Err(Error::InvalidInput(
                        "intermediate path component is not a directory",
                    ));
                }
            }
        }
        Ok(current)
    }

    /// Iterate over a directory's children in deterministic lexical order.
    pub fn children(&self, parent_id: ObjectId) -> impl Iterator<Item = &DirectoryEntry> {
        self.names
            .range((parent_id, String::new())..=(parent_id, char::MAX.to_string().repeat(64)))
            .map(|(_, entry)| entry)
    }

    /// Add a new object and its parent/name edge after validating both records.
    ///
    /// This is an in-memory operation only. The future persistent layer must perform
    /// equivalent checks inside a journal transaction before publishing disk changes.
    pub fn insert(&mut self, entry: DirectoryEntry, record: MetadataRecord) -> Result<()> {
        if entry.object_id != record.id.0
            || entry.parent_id != record.parent_id
            || entry.kind != record.kind
        {
            return Err(Error::InvalidInput(
                "directory entry and metadata record disagree",
            ));
        }
        // Reuse the format validators before accepting caller-created structs.
        entry.encode()?;
        record.encode()?;

        let parent_id = ObjectId::new(entry.parent_id)?;
        let object_id = record.id;
        let parent = self
            .objects
            .get(&parent_id)
            .ok_or(Error::InvalidInput("parent directory does not exist"))?;
        if parent.kind != ObjectKind::Directory {
            return Err(Error::InvalidInput("parent object is not a directory"));
        }
        if self.objects.contains_key(&object_id) {
            return Err(Error::InvalidInput("object ID already exists"));
        }
        let key = (parent_id, entry.name.clone());
        if self.names.contains_key(&key) {
            return Err(Error::InvalidInput("name already exists in directory"));
        }

        // No fallible work follows the first mutation: both indexes stay consistent.
        self.objects.insert(object_id, record);
        self.names.insert(key, entry);
        Ok(())
    }

    /// Rename an object or move it to another directory in the in-memory namespace.
    ///
    /// The operation updates the directory edge and the object's parent together.
    /// It rejects moves below the object itself, which would create a directory cycle.
    /// This does not persist changes or provide crash consistency.
    pub fn rename(
        &mut self,
        object_id: ObjectId,
        new_parent_id: ObjectId,
        new_name: &str,
    ) -> Result<()> {
        if object_id == self.root_id {
            return Err(Error::InvalidInput(
                "the root directory cannot be renamed or moved",
            ));
        }
        let record = self
            .objects
            .get(&object_id)
            .cloned()
            .ok_or(Error::InvalidInput("object does not exist"))?;
        let old_parent_id = ObjectId::new(record.parent_id)?;
        let old_entry = self
            .names
            .get(&(
                old_parent_id,
                self.entry_name_for(old_parent_id, object_id)?,
            ))
            .cloned()
            .ok_or(Error::Corrupt("object has no matching directory entry"))?;
        let mut updated_entry = old_entry.clone();
        updated_entry.parent_id = new_parent_id.0;
        updated_entry.name = new_name.to_owned();
        updated_entry.encode()?;

        let parent = self
            .objects
            .get(&new_parent_id)
            .ok_or(Error::InvalidInput("destination directory does not exist"))?;
        if parent.kind != ObjectKind::Directory {
            return Err(Error::InvalidInput("destination parent is not a directory"));
        }

        let mut ancestor = new_parent_id;
        loop {
            if ancestor == object_id {
                return Err(Error::InvalidInput("directory move would create a cycle"));
            }
            let Some(ancestor_record) = self.objects.get(&ancestor) else {
                return Err(Error::Corrupt("destination ancestor is missing"));
            };
            if ancestor == self.root_id {
                break;
            }
            ancestor = ObjectId::new(ancestor_record.parent_id)
                .map_err(|_| Error::Corrupt("non-root ancestor has no parent"))?;
        }

        let new_key = (new_parent_id, new_name.to_owned());
        if self.names.contains_key(&new_key)
            && !(new_parent_id == old_parent_id && new_name == old_entry.name)
        {
            return Err(Error::InvalidInput("destination name already exists"));
        }

        let mut updated_record = record.clone();
        updated_record.parent_id = new_parent_id.0;
        updated_record.generation = updated_record
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        updated_record.encode()?;

        let old_key = (old_parent_id, old_entry.name.clone());
        self.names.remove(&old_key);
        self.names.insert(new_key, updated_entry);
        self.objects.insert(object_id, updated_record);
        Ok(())
    }

    /// Remove an object from the in-memory namespace.
    ///
    /// Directories must be empty. The root cannot be removed. This changes only
    /// namespace records; a future persistent implementation must free data extents
    /// transactionally before exposing equivalent deletion semantics.
    pub fn remove(&mut self, object_id: ObjectId) -> Result<MetadataRecord> {
        if object_id == self.root_id {
            return Err(Error::InvalidInput("the root directory cannot be removed"));
        }
        let record = self
            .objects
            .get(&object_id)
            .cloned()
            .ok_or(Error::InvalidInput("object does not exist"))?;
        if record.kind == ObjectKind::Directory
            && self
                .names
                .keys()
                .any(|(parent_id, _)| *parent_id == object_id)
        {
            return Err(Error::InvalidInput("directory is not empty"));
        }

        let parent_id = ObjectId::new(record.parent_id)?;
        let name = self.entry_name_for(parent_id, object_id)?;
        let key = (parent_id, name);
        let entry = self
            .names
            .get(&key)
            .ok_or(Error::Corrupt("object has no matching directory entry"))?;
        if entry.object_id != object_id.0 || entry.kind != record.kind {
            return Err(Error::Corrupt(
                "directory entry disagrees with object metadata",
            ));
        }

        self.names.remove(&key);
        self.objects
            .remove(&object_id)
            .ok_or(Error::Corrupt("object disappeared during removal"))
    }

    fn entry_name_for(&self, parent_id: ObjectId, object_id: ObjectId) -> Result<String> {
        self.names
            .iter()
            .find_map(|((parent, _), entry)| {
                (*parent == parent_id && entry.object_id == object_id.0).then(|| entry.name.clone())
            })
            .ok_or(Error::Corrupt("object has no matching directory entry"))
    }

    /// Validate both namespace indexes and prove that every object is reachable from root.
    ///
    /// This is an in-memory consistency check, not a disk scanner or a persistence guarantee.
    pub fn validate(&self) -> Result<()> {
        let root = self
            .objects
            .get(&self.root_id)
            .ok_or(Error::Corrupt("namespace root object is missing"))?;
        if root.kind != ObjectKind::Directory || root.parent_id != 0 {
            return Err(Error::Corrupt("namespace root metadata is invalid"));
        }
        if self.names.len().checked_add(1) != Some(self.objects.len()) {
            return Err(Error::Corrupt(
                "namespace object and directory-entry counts disagree",
            ));
        }

        let mut referenced = std::collections::BTreeSet::new();
        for ((parent_id, key_name), entry) in &self.names {
            entry
                .encode()
                .map_err(|_| Error::Corrupt("invalid directory entry in namespace"))?;
            if entry.parent_id != parent_id.0 || entry.name != *key_name {
                return Err(Error::Corrupt("directory-entry index key is inconsistent"));
            }
            let parent = self
                .objects
                .get(parent_id)
                .ok_or(Error::Corrupt("directory entry parent is missing"))?;
            if parent.kind != ObjectKind::Directory {
                return Err(Error::Corrupt("directory entry parent is not a directory"));
            }
            let object_id = ObjectId::new(entry.object_id)
                .map_err(|_| Error::Corrupt("directory entry has an invalid object ID"))?;
            let object = self
                .objects
                .get(&object_id)
                .ok_or(Error::Corrupt("directory entry points to a missing object"))?;
            object
                .encode()
                .map_err(|_| Error::Corrupt("invalid metadata record in namespace"))?;
            if object.parent_id != parent_id.0 || object.kind != entry.kind {
                return Err(Error::Corrupt(
                    "directory entry disagrees with object metadata",
                ));
            }
            if !referenced.insert(object_id) {
                return Err(Error::Corrupt(
                    "an object is referenced by more than one directory entry",
                ));
            }
        }

        if referenced.contains(&self.root_id) {
            return Err(Error::Corrupt(
                "namespace root must not have a directory entry",
            ));
        }
        let mut visited = std::collections::BTreeSet::new();
        let mut pending = vec![self.root_id];
        visited.insert(self.root_id);
        while let Some(parent_id) = pending.pop() {
            for entry in self.children(parent_id) {
                let child_id = ObjectId::new(entry.object_id)
                    .map_err(|_| Error::Corrupt("invalid child object ID"))?;
                if !visited.insert(child_id) {
                    return Err(Error::Corrupt(
                        "namespace contains a cycle or multiply referenced object",
                    ));
                }
                if self.objects.get(&child_id).map(|record| record.kind)
                    == Some(ObjectKind::Directory)
                {
                    pending.push(child_id);
                }
            }
        }
        if visited.len() != self.objects.len() {
            return Err(Error::Corrupt("namespace contains unreachable objects"));
        }
        Ok(())
    }

    pub fn object_count(&self) -> usize {
        self.objects.len()
    }

    pub fn entry_count(&self) -> usize {
        self.names.len()
    }
}

#[cfg(test)]
mod tests {
    use super::Namespace;
    use crate::{DirectoryEntry, MetadataRecord, ObjectId, ObjectKind};
    use openfs_core::Error;

    fn record(id: u128, parent: u128, kind: ObjectKind) -> MetadataRecord {
        MetadataRecord {
            id: ObjectId(id),
            parent_id: parent,
            kind,
            flags: 0,
            size: 0,
            extent_root_block: None,
            generation: 1,
        }
    }

    fn entry(parent: u128, id: u128, name: &str, kind: ObjectKind) -> DirectoryEntry {
        DirectoryEntry {
            parent_id: parent,
            object_id: id,
            kind,
            name: name.to_owned(),
        }
    }

    #[test]
    fn creates_root_and_resolves_nested_names() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "docs", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(2, 3, "readme.txt", ObjectKind::File),
            record(3, 2, ObjectKind::File),
        )
        .unwrap();

        assert_eq!(ns.root_id(), ObjectId(1));
        assert_eq!(ns.object_count(), 3);
        assert_eq!(ns.entry_count(), 2);
        assert_eq!(ns.lookup(ObjectId(1), "docs").unwrap().object_id, 2);
        assert_eq!(ns.lookup(ObjectId(2), "readme.txt").unwrap().object_id, 3);
        assert_eq!(ns.children(ObjectId(2)).count(), 1);
    }

    #[test]
    fn rejects_duplicate_names_and_object_ids_without_partial_mutation() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "same", ObjectKind::File),
            record(2, 1, ObjectKind::File),
        )
        .unwrap();

        assert!(matches!(
            ns.insert(
                entry(1, 3, "same", ObjectKind::File),
                record(3, 1, ObjectKind::File)
            ),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ns.insert(
                entry(1, 2, "other", ObjectKind::File),
                record(2, 1, ObjectKind::File)
            ),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(ns.object_count(), 2);
        assert_eq!(ns.entry_count(), 1);
        assert!(ns.object(ObjectId(3)).is_none());
    }

    #[test]
    fn rejects_missing_or_non_directory_parents_and_mismatched_records() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        assert!(matches!(
            ns.insert(
                entry(9, 2, "orphan", ObjectKind::File),
                record(2, 9, ObjectKind::File)
            ),
            Err(Error::InvalidInput(_))
        ));
        ns.insert(
            entry(1, 2, "file", ObjectKind::File),
            record(2, 1, ObjectKind::File),
        )
        .unwrap();
        assert!(matches!(
            ns.insert(
                entry(2, 3, "child", ObjectKind::File),
                record(3, 2, ObjectKind::File)
            ),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ns.insert(
                entry(1, 4, "mismatch", ObjectKind::Directory),
                record(4, 1, ObjectKind::File)
            ),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(ns.object_count(), 2);
        assert_eq!(ns.entry_count(), 1);
    }

    #[test]
    fn renames_and_moves_objects_without_partial_mutation() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "left", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(1, 3, "right", ObjectKind::Directory),
            record(3, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(2, 4, "before.txt", ObjectKind::File),
            record(4, 2, ObjectKind::File),
        )
        .unwrap();

        ns.rename(ObjectId(4), ObjectId(3), "after.txt").unwrap();
        assert!(ns.lookup(ObjectId(2), "before.txt").is_none());
        assert_eq!(ns.lookup(ObjectId(3), "after.txt").unwrap().object_id, 4);
        assert_eq!(ns.object(ObjectId(4)).unwrap().parent_id, 3);
        assert_eq!(ns.object(ObjectId(4)).unwrap().generation, 2);
        assert_eq!(ns.entry_count(), 3);

        ns.insert(
            entry(3, 5, "nested", ObjectKind::Directory),
            record(5, 3, ObjectKind::Directory),
        )
        .unwrap();
        assert!(matches!(
            ns.rename(ObjectId(4), ObjectId(3), "nested"),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ns.rename(ObjectId(3), ObjectId(5), "cycle"),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(ns.lookup(ObjectId(3), "after.txt").unwrap().object_id, 4);
        assert_eq!(ns.object(ObjectId(4)).unwrap().parent_id, 3);
    }

    #[test]
    fn removes_files_and_only_empty_directories() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "folder", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(2, 3, "item", ObjectKind::File),
            record(3, 2, ObjectKind::File),
        )
        .unwrap();

        assert!(matches!(
            ns.remove(ObjectId(2)),
            Err(Error::InvalidInput(_))
        ));
        let removed = ns.remove(ObjectId(3)).unwrap();
        assert_eq!(removed.id, ObjectId(3));
        assert!(ns.lookup(ObjectId(2), "item").is_none());
        assert_eq!(ns.object_count(), 2);
        assert_eq!(ns.entry_count(), 1);

        assert_eq!(ns.remove(ObjectId(2)).unwrap().kind, ObjectKind::Directory);
        assert_eq!(ns.object_count(), 1);
        assert_eq!(ns.entry_count(), 0);
        assert!(matches!(
            ns.remove(ObjectId(1)),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn rename_rejects_generation_overflow_without_mutating_indexes() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        let mut item = record(2, 1, ObjectKind::File);
        item.generation = u32::MAX;
        ns.insert(entry(1, 2, "stable", ObjectKind::File), item)
            .unwrap();

        assert!(matches!(
            ns.rename(ObjectId(2), ObjectId(1), "changed"),
            Err(Error::ArithmeticOverflow)
        ));
        assert!(ns.lookup(ObjectId(1), "stable").is_some());
        assert!(ns.lookup(ObjectId(1), "changed").is_none());
    }

    #[test]
    fn resolves_nested_paths_and_rejects_traversal_or_file_intermediates() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "docs", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(2, 3, "readme.txt", ObjectKind::File),
            record(3, 2, ObjectKind::File),
        )
        .unwrap();

        assert_eq!(ns.resolve_path("/").unwrap(), ObjectId(1));
        assert_eq!(ns.resolve_path("///docs//").unwrap(), ObjectId(2));
        assert_eq!(ns.resolve_path("/docs/readme.txt").unwrap(), ObjectId(3));
        assert!(matches!(
            ns.resolve_path("/docs/readme.txt/child"),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ns.resolve_path("../docs"),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ns.resolve_path("/missing"),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn validates_nested_namespace_and_detects_orphan_objects() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "folder", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(2, 3, "file.txt", ObjectKind::File),
            record(3, 2, ObjectKind::File),
        )
        .unwrap();
        assert!(ns.validate().is_ok());

        ns.objects
            .insert(ObjectId(99), record(99, 1, ObjectKind::File));
        assert!(matches!(ns.validate(), Err(Error::Corrupt(_))));
    }

    #[test]
    fn validates_namespace_indexes_and_rejects_mismatched_entry() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "file.txt", ObjectKind::File),
            record(2, 1, ObjectKind::File),
        )
        .unwrap();
        assert!(ns.validate().is_ok());

        let mut bad = entry(1, 2, "file.txt", ObjectKind::File);
        bad.kind = ObjectKind::Directory;
        ns.names.insert((ObjectId(1), "file.txt".to_owned()), bad);
        assert!(matches!(ns.validate(), Err(Error::Corrupt(_))));
    }

    #[test]
    fn names_are_case_sensitive_and_children_are_sorted() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        for (id, name) in [(2, "zeta"), (3, "Alpha"), (4, "alpha")] {
            ns.insert(
                entry(1, id, name, ObjectKind::File),
                record(id, 1, ObjectKind::File),
            )
            .unwrap();
        }
        assert!(ns.lookup(ObjectId(1), "ALPHA").is_none());
        let names: Vec<_> = ns.children(ObjectId(1)).map(|e| e.name.as_str()).collect();
        assert_eq!(names, ["Alpha", "alpha", "zeta"]);
    }
}
