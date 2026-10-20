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
    // Reverse index avoids scanning every sibling when resolving an object's parent edge.
    parents: BTreeMap<ObjectId, (ObjectId, String)>,
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
            parents: BTreeMap::new(),
        })
    }

    pub fn root_id(&self) -> ObjectId {
        self.root_id
    }

    /// Rebuild an in-memory namespace from decoded persistent records.
    ///
    /// The root record must be present, but—as on disk—it must not have a directory
    /// entry. Duplicate object IDs and duplicate (parent, name) keys are rejected;
    /// the complete graph is validated before the namespace is returned.
    /// This is a reconstruction primitive, not a disk scanner or persistence layer.
    pub fn from_records(
        root_id: ObjectId,
        records: impl IntoIterator<Item = MetadataRecord>,
        entries: impl IntoIterator<Item = DirectoryEntry>,
    ) -> Result<Self> {
        ObjectId::new(root_id.0)?;
        let mut objects = BTreeMap::new();
        for record in records {
            record.encode().map_err(|_| {
                Error::Corrupt("invalid metadata record during namespace reconstruction")
            })?;
            if objects.insert(record.id, record).is_some() {
                return Err(Error::Corrupt("duplicate object ID in metadata inventory"));
            }
        }

        let mut names = BTreeMap::new();
        let mut parents = BTreeMap::new();
        for entry in entries {
            entry.encode().map_err(|_| {
                Error::Corrupt("invalid directory entry during namespace reconstruction")
            })?;
            let key = (ObjectId::new(entry.parent_id)?, entry.name.clone());
            if names.insert(key.clone(), entry.clone()).is_some() {
                return Err(Error::Corrupt(
                    "duplicate directory name in metadata inventory",
                ));
            }
            if parents.insert(ObjectId::new(entry.object_id)?, (ObjectId(entry.parent_id), entry.name)).is_some() {
                return Err(Error::Corrupt(
                    "an object has multiple parent directory entries",
                ));
            }
        }

        let namespace = Self {
            root_id,
            objects,
            names,
            parents,
        };
        namespace.validate()?;
        Ok(namespace)
    }

    pub fn object(&self, id: ObjectId) -> Option<&MetadataRecord> {
        self.objects.get(&id)
    }

    /// Iterate all metadata records in ascending object-ID order.
    ///
    /// This exposes a deterministic read-only inventory for disk validation and
    /// consistency tools without allowing callers to mutate the namespace indexes.
    pub fn objects(&self) -> impl Iterator<Item = &MetadataRecord> {
        self.objects.values()
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
        let mut components = path
            .split('/')
            .filter(|component| !component.is_empty())
            .peekable();
        let mut current = self.root_id;
        while let Some(component) = components.next() {
            if component == "." || component == ".." {
                return Err(Error::InvalidInput("dot path components are not supported"));
            }
            let entry = self
                .lookup(current, component)
                .ok_or(Error::InvalidInput("path component does not exist"))?;
            let next = ObjectId::new(entry.object_id)?;
            if next != self.root_id && !self.objects.contains_key(&next) {
                return Err(Error::Corrupt("directory entry points to a missing object"));
            }
            current = next;
            if components.peek().is_some() {
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
            .range((parent_id, String::new())..)
            .take_while(move |((candidate_parent, _), _)| *candidate_parent == parent_id)
            .map(|(_, entry)| entry)
    }

    /// Return a canonical absolute path for an object.
    ///
    /// Every parent edge is checked while walking to root; malformed parent chains
    /// and cycles are reported as corruption rather than looping indefinitely.
    pub fn path_of(&self, object_id: ObjectId) -> Result<String> {
        if !self.objects.contains_key(&object_id) {
            return Err(Error::InvalidInput("object does not exist"));
        }
        if object_id == self.root_id {
            return Ok("/".to_owned());
        }

        let mut current = object_id;
        let mut components = Vec::new();
        // A valid parent chain reaches root in at most objects.len() edges. Use that
        // structural bound for cycle detection instead of allocating a tree set on
        // every path lookup.
        let mut traversed = 0_usize;
        while current != self.root_id {
            if traversed >= self.objects.len() {
                return Err(Error::Corrupt("namespace parent chain contains a cycle"));
            }
            traversed += 1;
            let record = self
                .objects
                .get(&current)
                .ok_or(Error::Corrupt("path object metadata is missing"))?;
            let (parent_id, name) = self.parents.get(&current).cloned()
                .ok_or(Error::Corrupt("path directory entry is missing"))?;
            if record.parent_id != parent_id.0 {
                return Err(Error::Corrupt("parent index disagrees with object metadata"));
            }
            let entry = self
                .names
                .get(&(parent_id, name.clone()))
                .ok_or(Error::Corrupt("path directory entry is missing"))?;
            if entry.parent_id != parent_id.0
                || entry.object_id != current.0
                || entry.kind != record.kind
            {
                return Err(Error::Corrupt("path entry disagrees with object metadata"));
            }
            components.push(name);
            current = parent_id;
        }
        components.reverse();
        Ok(format!("/{}", components.join("/")))
    }

    /// Return one deterministic page of directory entries after an optional name cursor.
    ///
    /// The cursor is exclusive, so callers can continue with the last returned name
    /// without duplicates. This is an in-memory listing API, not a disk-backed iterator.
    pub fn list_directory(
        &self,
        parent_id: ObjectId,
        after_name: Option<&str>,
        limit: usize,
    ) -> Result<Vec<DirectoryEntry>> {
        if limit == 0 {
            return Err(Error::InvalidInput("directory page limit must be nonzero"));
        }
        let parent = self
            .objects
            .get(&parent_id)
            .ok_or(Error::InvalidInput("directory does not exist"))?;
        if parent.kind != ObjectKind::Directory {
            return Err(Error::InvalidInput("object is not a directory"));
        }
        // Start at the cursor in the ordered index instead of walking and discarding
        // every earlier child on each page. Keep the cursor exclusive and stop as soon
        // as the parent key changes, so listing remains bounded to this directory.
        use std::ops::Bound::{Excluded, Included, Unbounded};
        let lower_name = match after_name {
            Some(cursor) => Excluded((parent_id, cursor.to_owned())),
            None => Included((parent_id, String::new())),
        };
        Ok(self
            .names
            .range((lower_name, Unbounded))
            .take_while(|((candidate_parent, _), _)| *candidate_parent == parent_id)
            .map(|(_, entry)| entry)
            .take(limit)
            .cloned()
            .collect())
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
        let mut updated_parent = self
            .objects
            .get(&parent_id)
            .cloned()
            .ok_or(Error::InvalidInput("parent directory does not exist"))?;
        if updated_parent.kind != ObjectKind::Directory {
            return Err(Error::InvalidInput("parent object is not a directory"));
        }
        if self.objects.contains_key(&object_id) {
            return Err(Error::InvalidInput("object ID already exists"));
        }
        let key = (parent_id, entry.name.clone());
        if self.names.contains_key(&key) {
            return Err(Error::InvalidInput("name already exists in directory"));
        }

        // A directory generation changes whenever its child set changes.
        // Validate the increment before mutating either namespace index.
        updated_parent.generation = updated_parent.generation.checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        updated_parent.encode()?;

        // No fallible work follows the first mutation: both indexes stay consistent.
        self.objects.insert(parent_id, updated_parent);
        self.objects.insert(object_id, record);
        self.parents.insert(object_id, (parent_id, entry.name.clone()));
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
        let old_name = self.entry_name_for(old_parent_id, object_id)?;
        let old_entry = self
            .names
            .get(&(old_parent_id, old_name))
            .cloned()
            .ok_or(Error::Corrupt("object has no matching directory entry"))?;
        if old_entry.object_id != object_id.0
            || old_entry.parent_id != record.parent_id
            || old_entry.kind != record.kind
        {
            return Err(Error::Corrupt(
                "directory entry disagrees with object metadata",
            ));
        }
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
        // A true no-op must not consume a metadata generation or rewrite both indexes.
        if new_parent_id == old_parent_id && new_name == old_entry.name {
            return Ok(());
        }

        let mut updated_record = record.clone();
        updated_record.parent_id = new_parent_id.0;
        updated_record.generation = updated_record
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        updated_record.encode()?;

        // Directory generations track child-name changes too. Prepare both parent
        // updates before publishing the moved entry, including same-directory renames.
        let mut updated_old_parent = self.objects.get(&old_parent_id).cloned()
            .ok_or(Error::Corrupt("old parent directory is missing"))?;
        updated_old_parent.generation = updated_old_parent.generation
            .checked_add(1).ok_or(Error::ArithmeticOverflow)?;
        updated_old_parent.encode()?;
        let updated_new_parent = if new_parent_id == old_parent_id {
            None
        } else {
            let mut parent = self.objects.get(&new_parent_id).cloned()
                .ok_or(Error::Corrupt("destination directory is missing"))?;
            parent.generation = parent.generation
                .checked_add(1).ok_or(Error::ArithmeticOverflow)?;
            parent.encode()?;
            Some(parent)
        };

        let old_key = (old_parent_id, old_entry.name.clone());
        self.names.remove(&old_key);
        self.names.insert(new_key, updated_entry.clone());
        self.parents.insert(object_id, (new_parent_id, updated_entry.name));
        self.objects.insert(object_id, updated_record);
        self.objects.insert(old_parent_id, updated_old_parent);
        if let Some(parent) = updated_new_parent {
            self.objects.insert(new_parent_id, parent);
        }
        Ok(())
    }

    /// Update file length and extent-root metadata in memory.
    ///
    /// An empty file must have no extent root; directory records cannot be resized.
    /// This does not allocate/free blocks or persist the updated metadata.
    pub fn set_file_layout(
        &mut self,
        object_id: ObjectId,
        size: u64,
        extent_root_block: Option<u64>,
    ) -> Result<()> {
        let current = self
            .objects
            .get(&object_id)
            .cloned()
            .ok_or(Error::InvalidInput("object does not exist"))?;
        if current.kind != ObjectKind::File {
            return Err(Error::InvalidInput("only files have a file layout"));
        }
        if let Some(root_block) = extent_root_block {
            if size == 0 {
                return Err(Error::InvalidInput("empty files cannot own an extent-map root"));
            }
            if self.objects.values().any(|record| {
                record.id != object_id && record.extent_root_block == Some(root_block)
            }) {
                return Err(Error::InvalidInput(
                    "extent-map root is already owned by another object",
                ));
            }
        }
        let mut updated = current;
        updated.size = size;
        updated.extent_root_block = extent_root_block;
        updated.generation = updated
            .generation
            .checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        updated.encode()?;
        self.objects.insert(object_id, updated);
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
                .range((object_id, String::new())..)
                .next()
                .is_some_and(|((parent_id, _), _)| *parent_id == object_id)
        {
            return Err(Error::InvalidInput("directory is not empty"));
        }

        let (parent_id, name) = self.parents.get(&object_id).cloned()
            .ok_or(Error::Corrupt("object has no matching directory entry"))?;
        if record.parent_id != parent_id.0 {
            return Err(Error::Corrupt("parent index disagrees with object metadata"));
        }
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

        let mut parent_record = self.objects.get(&parent_id).cloned()
            .ok_or(Error::Corrupt("parent directory metadata is missing"))?;
        if parent_record.kind != ObjectKind::Directory {
            return Err(Error::Corrupt("object parent is not a directory"));
        }
        parent_record.generation = parent_record.generation.checked_add(1)
            .ok_or(Error::ArithmeticOverflow)?;
        parent_record.encode()?;

        self.names.remove(&key);
        self.parents.remove(&object_id);
        self.objects.insert(parent_id, parent_record);
        self.objects
            .remove(&object_id)
            .ok_or(Error::Corrupt("object disappeared during removal"))
    }

    fn entry_name_for(&self, parent_id: ObjectId, object_id: ObjectId) -> Result<String> {
        let (indexed_parent, name) = self.parents.get(&object_id)
            .ok_or(Error::Corrupt("object has no matching directory entry"))?;
        if *indexed_parent != parent_id {
            return Err(Error::Corrupt("parent index disagrees with object metadata"));
        }
        Ok(name.clone())
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
        if self.names.len() != self.parents.len()
            || self.names.len().checked_add(1) != Some(self.objects.len()) {
            return Err(Error::Corrupt(
                "namespace object and directory-entry counts disagree",
            ));
        }

        let mut extent_roots = std::collections::BTreeSet::new();
        for record in self.objects.values() {
            if let Some(root_block) = record.extent_root_block {
                if !extent_roots.insert(root_block) {
                    return Err(Error::Corrupt(
                        "multiple namespace objects claim the same extent-map root block",
                    ));
                }
            }
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
            if self.parents.get(&object_id) != Some(&(*parent_id, key_name.clone())) {
                return Err(Error::Corrupt("object-to-parent index is inconsistent"));
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

    /// Return sorted extent-map root blocks owned by namespace objects.
    ///
    /// This bridges decoded namespace metadata to the allocator ownership checker. Every
    /// root must lie inside the allocatable data region, and two objects may not claim the
    /// same root block. This inspects the in-memory namespace only; it does not discover
    /// records by scanning a volume.
    pub fn validated_extent_root_inventory(
        &self,
        data_start: u64,
        data_end: u64,
    ) -> Result<Vec<u64>> {
        if data_start >= data_end {
            return Err(Error::InvalidInput("data region bounds are invalid"));
        }

        let mut roots = Vec::new();
        for record in self.objects.values() {
            record
                .encode()
                .map_err(|_| Error::Corrupt("invalid metadata record in namespace"))?;
            if let Some(root) = record.extent_root_block {
                if root < data_start || root >= data_end {
                    return Err(Error::Corrupt(
                        "extent-map root lies outside the allocatable data region",
                    ));
                }
                roots.push(root);
            }
        }

        roots.sort_unstable();
        if roots.windows(2).any(|pair| pair[0] == pair[1]) {
            return Err(Error::Corrupt(
                "multiple namespace objects claim the same extent-map root block",
            ));
        }
        Ok(roots)
    }

    /// Count objects that own an extent-map root after validating metadata records.
    pub fn extent_root_owner_count(&self) -> Result<usize> {
        self.objects.values().try_fold(0_usize, |count, record| {
            record
                .encode()
                .map_err(|_| Error::Corrupt("invalid metadata record in namespace"))?;
            count
                .checked_add(usize::from(record.extent_root_block.is_some()))
                .ok_or(Error::ArithmeticOverflow)
        })
    }

    /// Iterate all directory entries in deterministic (parent ID, name) order.
    ///
    /// This read-only view lets storage APIs compare the complete namespace graph,
    /// not only object metadata, against the inventory reconstructed from disk.
    pub fn entries(&self) -> impl Iterator<Item = &DirectoryEntry> {
        self.names.values()
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
    fn extent_root_inventory_is_sorted_bounded_and_rejects_aliases() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "later", ObjectKind::File),
            MetadataRecord {
                id: ObjectId(2),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 1,
                extent_root_block: Some(30),
                generation: 1,
            },
        )
        .unwrap();
        ns.insert(
            entry(1, 3, "earlier", ObjectKind::File),
            MetadataRecord {
                id: ObjectId(3),
                parent_id: 1,
                kind: ObjectKind::File,
                flags: 0,
                size: 1,
                extent_root_block: Some(12),
                generation: 1,
            },
        )
        .unwrap();

        assert_eq!(
            ns.validated_extent_root_inventory(8, 40).unwrap(),
            vec![12, 30]
        );
        assert_eq!(ns.extent_root_owner_count().unwrap(), 2);
        assert!(matches!(
            ns.validated_extent_root_inventory(13, 40),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            ns.validated_extent_root_inventory(8, 30),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            ns.validated_extent_root_inventory(40, 40),
            Err(Error::InvalidInput(_))
        ));

        ns.set_file_layout(ObjectId(3), 1, Some(30)).unwrap();
        assert!(matches!(
            ns.validated_extent_root_inventory(8, 40),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(ns.validate(), Err(Error::Corrupt(_))));
    }

    #[test]
    fn reconstructs_namespace_from_decoded_record_inventories() {
        let root = record(1, 0, ObjectKind::Directory);
        let folder = record(2, 1, ObjectKind::Directory);
        let file = record(3, 2, ObjectKind::File);
        let ns = Namespace::from_records(
            ObjectId(1),
            [root.clone(), folder.clone(), file.clone()],
            [
                entry(1, 2, "docs", ObjectKind::Directory),
                entry(2, 3, "readme.txt", ObjectKind::File),
            ],
        )
        .unwrap();
        assert_eq!(ns.object_count(), 3);
        assert_eq!(ns.entry_count(), 2);
        assert_eq!(ns.resolve_path("/docs/readme.txt").unwrap(), ObjectId(3));
        assert_eq!(ns.path_of(ObjectId(3)).unwrap(), "/docs/readme.txt");
    }

    #[test]
    fn reconstruction_rejects_duplicate_records_names_and_broken_graphs() {
        let root = record(1, 0, ObjectKind::Directory);
        assert!(matches!(
            Namespace::from_records(ObjectId(1), [root.clone(), root.clone()], []),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            Namespace::from_records(
                ObjectId(1),
                [
                    root.clone(),
                    record(2, 1, ObjectKind::File),
                    record(3, 1, ObjectKind::File),
                ],
                [
                    entry(1, 2, "same", ObjectKind::File),
                    entry(1, 3, "same", ObjectKind::File),
                ],
            ),
            Err(Error::Corrupt(_))
        ));
        assert!(matches!(
            Namespace::from_records(ObjectId(1), [root, record(2, 99, ObjectKind::File)], []),
            Err(Error::Corrupt(_))
        ));
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
    fn updates_file_layout_and_rejects_invalid_transitions_atomically() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "file.bin", ObjectKind::File),
            record(2, 1, ObjectKind::File),
        )
        .unwrap();
        ns.set_file_layout(ObjectId(2), 8192, Some(42)).unwrap();
        assert_eq!(ns.object(ObjectId(2)).unwrap().size, 8192);
        assert_eq!(ns.object(ObjectId(2)).unwrap().extent_root_block, Some(42));
        assert_eq!(ns.object(ObjectId(2)).unwrap().generation, 2);

        ns.set_file_layout(ObjectId(2), 0, None).unwrap();
        assert_eq!(ns.object(ObjectId(2)).unwrap().size, 0);
        assert_eq!(ns.object(ObjectId(2)).unwrap().extent_root_block, None);
        assert_eq!(ns.object(ObjectId(2)).unwrap().generation, 3);

        assert!(matches!(
            ns.set_file_layout(ObjectId(2), 0, Some(42)),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(ns.object(ObjectId(2)).unwrap().size, 0);
        assert_eq!(ns.object(ObjectId(2)).unwrap().generation, 3);
        assert!(matches!(
            ns.set_file_layout(ObjectId(1), 1, Some(9)),
            Err(Error::InvalidInput(_))
        ));
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
    fn paths_round_trip_and_paginated_directory_listing_has_no_duplicates() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "docs", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        ns.insert(
            entry(2, 3, "report.txt", ObjectKind::File),
            record(3, 2, ObjectKind::File),
        )
        .unwrap();
        for (id, name) in [(4, "a.txt"), (5, "b.txt"), (6, "c.txt")] {
            ns.insert(
                entry(1, id, name, ObjectKind::File),
                record(id, 1, ObjectKind::File),
            )
            .unwrap();
        }

        assert_eq!(ns.path_of(ObjectId(1)).unwrap(), "/");
        assert_eq!(ns.path_of(ObjectId(3)).unwrap(), "/docs/report.txt");
        assert_eq!(
            ns.resolve_path(&ns.path_of(ObjectId(3)).unwrap()).unwrap(),
            ObjectId(3)
        );

        let first = ns.list_directory(ObjectId(1), None, 2).unwrap();
        assert_eq!(
            first.iter().map(|e| e.name.as_str()).collect::<Vec<_>>(),
            ["a.txt", "b.txt"]
        );
        let second = ns
            .list_directory(ObjectId(1), Some(&first[1].name), 2)
            .unwrap();
        assert_eq!(
            second.iter().map(|e| e.name.as_str()).collect::<Vec<_>>(),
            ["c.txt", "docs"]
        );
        assert!(ns
            .list_directory(ObjectId(1), Some("docs"), 10)
            .unwrap()
            .is_empty());
        assert!(matches!(
            ns.list_directory(ObjectId(1), None, 0),
            Err(Error::InvalidInput(_))
        ));
        assert!(matches!(
            ns.list_directory(ObjectId(3), None, 1),
            Err(Error::InvalidInput(_))
        ));
    }

    #[test]
    fn namespace_remains_valid_across_insert_move_resize_and_remove() {
        let mut ns = Namespace::new(ObjectId(1)).unwrap();
        ns.insert(
            entry(1, 2, "folder", ObjectKind::Directory),
            record(2, 1, ObjectKind::Directory),
        )
        .unwrap();
        assert!(ns.validate().is_ok());

        ns.insert(
            entry(2, 3, "inside.bin", ObjectKind::File),
            record(3, 2, ObjectKind::File),
        )
        .unwrap();
        assert_eq!(ns.path_of(ObjectId(3)).unwrap(), "/folder/inside.bin");
        assert!(ns.validate().is_ok());

        ns.rename(ObjectId(3), ObjectId(1), "outside.bin").unwrap();
        assert_eq!(ns.path_of(ObjectId(3)).unwrap(), "/outside.bin");
        assert!(ns.validate().is_ok());

        assert!(matches!(
            ns.rename(ObjectId(2), ObjectId(2), "cycle"),
            Err(Error::InvalidInput(_))
        ));
        assert_eq!(ns.path_of(ObjectId(2)).unwrap(), "/folder");
        assert_eq!(ns.path_of(ObjectId(3)).unwrap(), "/outside.bin");
        assert!(ns.validate().is_ok());

        ns.set_file_layout(ObjectId(3), 8192, Some(77)).unwrap();
        assert!(ns.validate().is_ok());
        let removed = ns.remove(ObjectId(3)).unwrap();
        assert_eq!(removed.size, 8192);
        assert!(ns.validate().is_ok());

        ns.remove(ObjectId(2)).unwrap();
        assert_eq!(ns.object_count(), 1);
        assert_eq!(ns.entry_count(), 0);
        assert!(ns.validate().is_ok());
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
