//! Exhaustive power-cut sweep for persistent mutations.
//!
//! The device below models a volatile write cache: a write only becomes durable once
//! a later `flush` succeeds. A simulated power cut is injected at **every** write or
//! flush event of an operation. The surviving image is then built under several cache
//! loss scenarios (nothing survives, everything survives, only even-numbered pending
//! blocks survive, only the second half of the pending blocks survives), which also
//! exercises write reordering. After each cut the volume must:
//!
//! * reopen through normal recovery (never fail closed on a legitimate crash),
//! * expose exactly the complete old state or the complete new state,
//! * show zero orphaned blocks and zero dangling references,
//! * keep acknowledged operations durable (an `Ok` result implies the new state
//!   survives even if the whole cache is lost),
//! * remain writable: the interrupted operation can be retried or a fresh object
//!   created, and the result is still consistent.

use crate::{format_volume, open_volume, ObjectId, OpenedVolume};
use openfs_block::BlockDevice;
use openfs_core::{Error, Result};

#[derive(Clone)]
struct CrashDevice {
    block_size: u32,
    block_count: u64,
    durable: Vec<u8>,
    pending: Vec<(u64, Vec<u8>)>,
    events: usize,
    crash_at: Option<usize>,
    crashed: bool,
}

#[derive(Clone, Copy, Debug)]
enum Keep {
    Nothing,
    Everything,
    EvenPending,
    SecondHalfPending,
}

const KEEP_MODES: [Keep; 4] = [
    Keep::Nothing,
    Keep::Everything,
    Keep::EvenPending,
    Keep::SecondHalfPending,
];

impl CrashDevice {
    fn blank(block_size: u32, block_count: u64) -> Self {
        let length = usize::try_from(u64::from(block_size) * block_count).unwrap();
        Self::from_image(block_size, block_count, vec![0; length])
    }

    fn from_image(block_size: u32, block_count: u64, durable: Vec<u8>) -> Self {
        Self {
            block_size,
            block_count,
            durable,
            pending: Vec::new(),
            events: 0,
            crash_at: None,
            crashed: false,
        }
    }

    fn arm(&mut self, events_from_now: usize) {
        self.crash_at = Some(self.events + events_from_now);
    }

    fn image(&self, keep: Keep) -> Vec<u8> {
        let mut image = self.durable.clone();
        let bs = self.block_size as usize;
        let total = self.pending.len();
        for (index, (block, bytes)) in self.pending.iter().enumerate() {
            let survives = match keep {
                Keep::Nothing => false,
                Keep::Everything => true,
                Keep::EvenPending => index % 2 == 0,
                Keep::SecondHalfPending => index >= total / 2,
            };
            if survives {
                let start = usize::try_from(*block).unwrap() * bs;
                image[start..start + bs].copy_from_slice(bytes);
            }
        }
        image
    }

    fn tick(&mut self) -> Result<()> {
        if self.crashed {
            return Err(Error::Io(std::io::Error::other("power already lost")));
        }
        let index = self.events;
        self.events += 1;
        if self.crash_at == Some(index) {
            self.crashed = true;
            return Err(Error::Io(std::io::Error::other("injected power cut")));
        }
        Ok(())
    }
}

impl BlockDevice for CrashDevice {
    fn block_size(&self) -> u32 {
        self.block_size
    }

    fn block_count(&self) -> u64 {
        self.block_count
    }

    fn read_blocks(&mut self, first_block: u64, output: &mut [u8]) -> Result<()> {
        let bs = self.block_size as usize;
        let start = usize::try_from(first_block).unwrap() * bs;
        output.copy_from_slice(&self.durable[start..start + output.len()]);
        let blocks = (output.len() / bs) as u64;
        for (block, bytes) in &self.pending {
            if *block >= first_block && *block < first_block + blocks {
                let offset = usize::try_from(*block - first_block).unwrap() * bs;
                output[offset..offset + bs].copy_from_slice(bytes);
            }
        }
        Ok(())
    }

    fn write_blocks(&mut self, first_block: u64, input: &[u8]) -> Result<()> {
        self.tick()?;
        let bs = self.block_size as usize;
        for (index, chunk) in input.chunks(bs).enumerate() {
            self.pending
                .push((first_block + index as u64, chunk.to_vec()));
        }
        Ok(())
    }

    fn flush(&mut self) -> Result<()> {
        self.tick()?;
        let bs = self.block_size as usize;
        for (block, bytes) in self.pending.drain(..) {
            let start = usize::try_from(block).unwrap() * bs;
            self.durable[start..start + bs].copy_from_slice(&bytes);
        }
        Ok(())
    }
}

type Op = fn(&mut OpenedVolume, &mut CrashDevice) -> Result<()>;

struct Scenario {
    name: &'static str,
    setup: Op,
    op: Op,
}

fn payload(len: usize, seed: u8) -> Vec<u8> {
    (0..len)
        .map(|i| (i as u8).wrapping_mul(31).wrapping_add(seed))
        .collect()
}

/// Canonical dump of the whole namespace: paths, kinds, sizes and file contents.
fn observe(volume: &OpenedVolume, device: &mut CrashDevice) -> Result<String> {
    let mut out = String::new();
    let mut stack = vec!["/".to_string()];
    let mut lines = Vec::new();
    while let Some(dir) = stack.pop() {
        let entries = volume.list_persistent_directory(device, &dir, None, 10_000)?;
        for entry in entries {
            let path = if dir == "/" {
                format!("/{}", entry.name)
            } else {
                format!("{dir}/{}", entry.name)
            };
            let record = volume.stat_persistent_path(device, &path)?;
            if format!("{:?}", entry.kind).contains("Directory") {
                lines.push(format!("D {path}"));
                stack.push(path);
            } else {
                let data = if record.size == 0 {
                    Vec::new()
                } else {
                    volume.read_persistent_path_range(
                        device,
                        &path,
                        0,
                        usize::try_from(record.size).unwrap(),
                    )?
                };
                let digest = data.iter().fold(0xcbf2_9ce4_8422_2325_u64, |h, b| {
                    (h ^ u64::from(*b)).wrapping_mul(0x0100_0000_01b3)
                });
                lines.push(format!("F {path} size={} fnv={digest:016x}", record.size));
            }
        }
    }
    lines.sort();
    for line in lines {
        out.push_str(&line);
        out.push('\n');
    }
    Ok(out)
}

fn assert_clean(volume: &OpenedVolume, device: &mut CrashDevice, context: &str) {
    let report = volume
        .verify_discovered_persistent_inventory(device)
        .unwrap_or_else(|e| panic!("{context}: verification failed: {e:?}"))
        .unwrap_or_else(|| panic!("{context}: namespace vanished"));
    assert!(
        report.is_consistent(),
        "{context}: orphans={} dangling={}",
        report.orphaned_block_count(),
        report.dangling_reference_count()
    );
}

fn sweep(scenario: &Scenario, block_size: u32) {
    const BLOCKS: u64 = 256;
    // Build the clean base image.
    let mut base = CrashDevice::blank(block_size, BLOCKS);
    {
        let mut volume = format_volume(&mut base, *b"openfs-sweep-001", 0).unwrap();
        volume
            .initialize_persistent_namespace(&mut base, ObjectId(1))
            .unwrap();
        (scenario.setup)(&mut volume, &mut base).unwrap();
        base.flush().unwrap();
    }
    let base_image = base.durable.clone();

    let fresh = |image: &[u8]| CrashDevice::from_image(block_size, BLOCKS, image.to_vec());

    // Observe the old and new states without faults and count the events.
    let mut device = fresh(&base_image);
    let mut volume = open_volume(&mut device).unwrap();
    let old_obs = observe(&volume, &mut device).unwrap();
    assert_clean(&volume, &mut device, "base image");
    let start = device.events;
    (scenario.op)(&mut volume, &mut device)
        .unwrap_or_else(|e| panic!("{}: fault-free run failed: {e:?}", scenario.name));
    let total_events = device.events - start;
    let new_obs = observe(&volume, &mut device).unwrap();
    assert_ne!(
        old_obs, new_obs,
        "{}: operation changes nothing",
        scenario.name
    );
    assert_clean(&volume, &mut device, "fault-free new state");
    assert!(total_events > 0);

    for cut in 0..=total_events {
        let label = format!("{} bs={block_size} cut={cut}/{total_events}", scenario.name);
        let mut device = fresh(&base_image);
        let mut volume = open_volume(&mut device).unwrap();
        device.arm(cut);
        let result = (scenario.op)(&mut volume, &mut device);
        // After any error the handle is dead by contract (reopen and recover). Cleanup
        // paths may report a conservative `Corrupt(.. preserve the image)` instead of the
        // original I/O error, so only the post-reopen on-disk state is asserted here.
        let acknowledged = result.is_ok();
        drop(volume);

        for keep in KEEP_MODES {
            let label = format!("{label} keep={keep:?}");
            let mut after = fresh(&device.image(keep));
            let mut reopened = open_volume(&mut after)
                .unwrap_or_else(|e| panic!("{label}: recovery refused to open: {e:?}"));
            // Mount-time orphan reclamation (VolumeSession::open performs the same step).
            reopened
                .reclaim_orphaned_blocks(&mut after)
                .unwrap_or_else(|e| panic!("{label}: orphan reclamation failed: {e:?}"));
            assert_clean(&reopened, &mut after, &label);
            let obs = observe(&reopened, &mut after)
                .unwrap_or_else(|e| panic!("{label}: namespace unreadable: {e:?}"));
            assert!(
                obs == old_obs || obs == new_obs,
                "{label}: torn state\n--- observed ---\n{obs}\n--- old ---\n{old_obs}\n--- new ---\n{new_obs}"
            );
            if acknowledged {
                assert_eq!(obs, new_obs, "{label}: acknowledged operation lost");
            }
            // The volume must still be usable after recovery.
            if obs == old_obs {
                (scenario.op)(&mut reopened, &mut after)
                    .unwrap_or_else(|e| panic!("{label}: retry failed: {e:?}"));
                assert_eq!(
                    observe(&reopened, &mut after).unwrap(),
                    new_obs,
                    "{label}: retry diverged"
                );
            }
            assert_clean(&reopened, &mut after, &format!("{label} after retry"));
            reopened
                .create_persistent_file_at_path(&mut after, "/", ObjectId(900), "post-crash.bin")
                .unwrap_or_else(|e| panic!("{label}: volume not writable after recovery: {e:?}"));
            assert_clean(&reopened, &mut after, &format!("{label} after new file"));
        }
    }
}

fn nothing(_: &mut OpenedVolume, _: &mut CrashDevice) -> Result<()> {
    Ok(())
}

fn setup_file(v: &mut OpenedVolume, d: &mut CrashDevice) -> Result<()> {
    v.create_persistent_file_at_path(d, "/", ObjectId(2), "f.bin")?;
    v.write_persistent_file_contents(d, "/f.bin", &payload(10_000, 3))?;
    v.create_persistent_file_at_path(d, "/", ObjectId(3), "keep.bin")?;
    v.write_persistent_file_contents(d, "/keep.bin", &payload(5000, 9))
}

fn setup_tree(v: &mut OpenedVolume, d: &mut CrashDevice) -> Result<()> {
    setup_file(v, d)?;
    v.create_persistent_directory_at_path(d, "/", ObjectId(4), "dir")?;
    v.create_persistent_file_at_path(d, "/dir", ObjectId(5), "inner.bin")?;
    v.write_persistent_file_contents(d, "/dir/inner.bin", &payload(9000, 5))
}

fn setup_empty_file(v: &mut OpenedVolume, d: &mut CrashDevice) -> Result<()> {
    v.create_persistent_file_at_path(d, "/", ObjectId(2), "f.bin")
        .map(|_| ())
}

fn setup_replace(v: &mut OpenedVolume, d: &mut CrashDevice) -> Result<()> {
    setup_file(v, d)?;
    v.create_persistent_file_at_path(d, "/", ObjectId(6), "src.bin")?;
    v.write_persistent_file_contents(d, "/src.bin", &payload(7000, 77))
}

fn scenarios() -> Vec<Scenario> {
    vec![
        Scenario {
            name: "create_file",
            setup: nothing,
            op: |v, d| {
                v.create_persistent_file_at_path(d, "/", ObjectId(2), "new.bin")
                    .map(|_| ())
            },
        },
        Scenario {
            name: "create_directory",
            setup: nothing,
            op: |v, d| {
                v.create_persistent_directory_at_path(d, "/", ObjectId(2), "newdir")
                    .map(|_| ())
            },
        },
        Scenario {
            name: "write_contents_into_empty_file",
            setup: setup_empty_file,
            op: |v, d| v.write_persistent_file_contents(d, "/f.bin", &payload(13_000, 1)),
        },
        Scenario {
            name: "overwrite_contents_shorter",
            setup: setup_file,
            op: |v, d| v.write_persistent_file_contents(d, "/f.bin", &payload(3000, 2)),
        },
        Scenario {
            name: "overwrite_contents_longer",
            setup: setup_file,
            op: |v, d| v.write_persistent_file_contents(d, "/f.bin", &payload(30_000, 4)),
        },
        Scenario {
            name: "range_write_in_place",
            setup: setup_file,
            op: |v, d| v.write_persistent_path_range(d, "/f.bin", 100, &payload(300, 8)),
        },
        Scenario {
            name: "range_write_append_at_eof",
            setup: setup_file,
            op: |v, d| v.write_persistent_path_range(d, "/f.bin", 10_000, &payload(9000, 6)),
        },
        Scenario {
            name: "extend_sparse",
            setup: setup_file,
            op: |v, d| v.extend_persistent_file_sparse(d, "/f.bin", 40_000),
        },
        Scenario {
            name: "stream_import_replaces_contents",
            setup: setup_file,
            op: |v, d| {
                let data = payload(21_000, 12);
                v.write_persistent_file_from_reader(d, "/f.bin", &mut std::io::Cursor::new(data))
                    .map(|_| ())
            },
        },
        Scenario {
            name: "truncate_shrink",
            setup: setup_file,
            op: |v, d| v.truncate_persistent_path(d, "/f.bin", 2500),
        },
        Scenario {
            name: "truncate_to_zero",
            setup: setup_file,
            op: |v, d| v.truncate_persistent_path(d, "/f.bin", 0),
        },
        Scenario {
            name: "truncate_grow_sparse",
            setup: setup_file,
            op: |v, d| v.truncate_persistent_path(d, "/f.bin", 50_000),
        },
        Scenario {
            name: "rename_file",
            setup: setup_tree,
            op: |v, d| v.rename_persistent_path(d, "/f.bin", "/dir", "moved.bin"),
        },
        Scenario {
            name: "replace_file",
            setup: setup_replace,
            op: |v, d| v.replace_persistent_file_at_path(d, "/src.bin", "/", "f.bin"),
        },
        Scenario {
            name: "remove_file",
            setup: setup_file,
            op: |v, d| v.remove_persistent_path(d, "/f.bin"),
        },
        Scenario {
            name: "remove_tree",
            setup: setup_tree,
            op: |v, d| v.remove_persistent_tree_at_path(d, "/dir"),
        },
    ]
}

// The complete matrix is CPU heavy in unoptimized builds; run it with
// `cargo test --release -p openfs-format crash_sweep` (CI does). Debug builds still run
// the quick smoke test below.
#[test]
#[cfg_attr(debug_assertions, ignore = "full sweep: run with --release")]
fn power_cut_sweep_4k() {
    for scenario in scenarios() {
        sweep(&scenario, 4096);
    }
}

#[test]
#[cfg_attr(debug_assertions, ignore = "full sweep: run with --release")]
fn power_cut_sweep_16k() {
    for scenario in scenarios() {
        sweep(&scenario, 16_384);
    }
}

#[test]
fn power_cut_sweep_smoke() {
    const QUICK: [&str; 4] = [
        "create_file",
        "remove_file",
        "truncate_shrink",
        "range_write_in_place",
    ];
    for scenario in scenarios()
        .into_iter()
        .filter(|scenario| QUICK.contains(&scenario.name))
    {
        sweep(&scenario, 4096);
    }
}

/// A power cut between allocation and publication leaves allocated-but-unreferenced
/// blocks. Mount-time reclamation must free exactly those, once, and stay idempotent.
#[test]
fn orphaned_blocks_left_by_a_power_cut_are_reclaimed_exactly_once() {
    const BS: u32 = 4096;
    const BLOCKS: u64 = 256;
    let mut base = CrashDevice::blank(BS, BLOCKS);
    {
        let mut volume = format_volume(&mut base, *b"openfs-orphan-01", 0).unwrap();
        volume
            .initialize_persistent_namespace(&mut base, ObjectId(1))
            .unwrap();
        base.flush().unwrap();
    }
    let base_image = base.durable.clone();

    let mut saw_orphans = false;
    for cut in 0..60 {
        let mut device = CrashDevice::from_image(BS, BLOCKS, base_image.clone());
        let mut volume = open_volume(&mut device).unwrap();
        device.arm(cut);
        let _ = volume.create_persistent_file_at_path(&mut device, "/", ObjectId(2), "a.bin");
        drop(volume);

        let mut after = CrashDevice::from_image(BS, BLOCKS, device.image(Keep::Everything));
        let mut reopened = open_volume(&mut after).unwrap();
        let orphans_before = reopened
            .verify_discovered_persistent_inventory(&mut after)
            .unwrap()
            .unwrap()
            .orphaned_block_count();
        let freed = reopened.reclaim_orphaned_blocks(&mut after).unwrap();
        assert_eq!(freed, orphans_before, "cut={cut}");
        assert_clean(&reopened, &mut after, &format!("cut={cut}"));
        assert_eq!(
            reopened.reclaim_orphaned_blocks(&mut after).unwrap(),
            0,
            "cut={cut}: reclamation must be idempotent"
        );
        saw_orphans |= orphans_before > 0;
    }
    assert!(
        saw_orphans,
        "the sweep never produced an orphan; the scenario no longer covers the gap"
    );
}

/// Reclamation must never free storage that the published namespace still owns.
#[test]
fn reclamation_never_frees_referenced_storage() {
    const BS: u32 = 4096;
    let mut device = CrashDevice::blank(BS, 256);
    let mut volume = format_volume(&mut device, *b"openfs-orphan-02", 0).unwrap();
    volume
        .initialize_persistent_namespace(&mut device, ObjectId(1))
        .unwrap();
    setup_tree(&mut volume, &mut device).unwrap();
    let before = observe(&volume, &mut device).unwrap();
    assert_eq!(volume.reclaim_orphaned_blocks(&mut device).unwrap(), 0);
    assert_eq!(observe(&volume, &mut device).unwrap(), before);
    assert_clean(&volume, &mut device, "after no-op reclamation");
}

/// A second power cut during orphan reclamation must be recoverable as well.
/// The cleanup transaction itself is journaled, so reopening and retrying it must
/// eventually produce a clean inventory without freeing any published object.
#[test]
fn power_cut_during_orphan_reclamation_is_recoverable() {
    const BS: u32 = 4096;
    const BLOCKS: u64 = 256;
    let mut base = CrashDevice::blank(BS, BLOCKS);
    {
        let mut volume = format_volume(&mut base, *b"openfs-reclaim01", 0).unwrap();
        volume
            .initialize_persistent_namespace(&mut base, ObjectId(1))
            .unwrap();
        base.flush().unwrap();
    }
    let base_image = base.durable.clone();

    // Find a real interrupted publication that leaves allocated, unreferenced blocks.
    let mut orphan_image = None;
    for cut in 0..80 {
        let mut device = CrashDevice::from_image(BS, BLOCKS, base_image.clone());
        let mut volume = open_volume(&mut device).unwrap();
        device.arm(cut);
        let _ = volume.create_persistent_file_at_path(&mut device, "/", ObjectId(2), "orphan.bin");
        drop(volume);

        let image = device.image(Keep::Everything);
        let mut probe = CrashDevice::from_image(BS, BLOCKS, image.clone());
        let reopened = open_volume(&mut probe).unwrap();
        let report = reopened
            .verify_discovered_persistent_inventory(&mut probe)
            .unwrap()
            .unwrap();
        if report.orphaned_block_count() > 0 {
            orphan_image = Some(image);
            break;
        }
    }
    let orphan_image = orphan_image.expect("could not produce an orphaned allocation");

    // Measure the cleanup write/flush event count without injecting a fault.
    let mut probe = CrashDevice::from_image(BS, BLOCKS, orphan_image.clone());
    let mut volume = open_volume(&mut probe).unwrap();
    let start = probe.events;
    assert!(volume.reclaim_orphaned_blocks(&mut probe).unwrap() > 0);
    let total_events = probe.events - start;
    assert!(total_events > 0);

    // Interrupt every persistence event in reclamation, then reopen and retry.
    for cut in 0..=total_events {
        let mut device = CrashDevice::from_image(BS, BLOCKS, orphan_image.clone());
        let mut volume = open_volume(&mut device).unwrap();
        device.arm(cut);
        let _ = volume.reclaim_orphaned_blocks(&mut device);
        drop(volume);

        for keep in KEEP_MODES {
            let label = format!("reclaim cut={cut}/{total_events} keep={keep:?}");
            let mut after = CrashDevice::from_image(BS, BLOCKS, device.image(keep));
            let mut reopened = open_volume(&mut after)
                .unwrap_or_else(|e| panic!("{label}: journal recovery failed: {e:?}"));
            reopened
                .reclaim_orphaned_blocks(&mut after)
                .unwrap_or_else(|e| panic!("{label}: reclamation retry failed: {e:?}"));
            assert_clean(&reopened, &mut after, &label);
            assert_eq!(
                reopened.reclaim_orphaned_blocks(&mut after).unwrap(),
                0,
                "{label}: reclamation is not idempotent"
            );
        }
    }
}
