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

    /// Iterate over a directory's children in deterministic lexical order.
    pub fn children(
        &self,
        parent_id: ObjectId,
    ) -> impl Iterator<Item = &DirectoryEntry> {
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
