# Benchmarks

No OpenFS performance measurements are available yet. The block-image and superblock primitives are not a complete filesystem and should not be compared to NTFS as if they were.

A future benchmark report must record hardware, OS and filesystem versions, Rust toolchain, build flags, image/device configuration, cache state, workload generator and seeds. Include sequential and random reads/writes, small and large files, directory-scale tests, metadata latency, concurrency, memory use, and fragmentation where applicable. Report throughput and latency distributions, warm/cold runs, repetitions, error bars, and limitations.

NTFS comparisons must use the same machine and equivalent workloads. Do not publish a parity or superiority claim without reproducible raw results and commands.
