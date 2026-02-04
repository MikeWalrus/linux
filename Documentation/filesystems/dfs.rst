.. SPDX-License-Identifier: GPL-2.0

============================================
DFS: Direct File System for Flash Devices
============================================

Overview
========

DFS is a Linux file system based on the "DFS: A File System for
Virtualized Flash Storage" paper. The design targets flash devices with a
virtualized block address space that provides allocation, wear leveling, and
atomic updates below the file system. DFS uses that abstraction to simplify
file layout, metadata management, and crash recovery.

This document is intentionally small and will evolve alongside the in-tree
implementation. It focuses on developer-facing information needed to build,
run, and test DFS while the implementation is under active development.

Goals
=====

- Keep DFS in-tree and follow Linux kernel development conventions.
- Provide a minimal, correct VFS integration with a clean, testable design.
- Leverage the virtualized flash layer for allocation and crash recovery.
- Support xfstests-driven validation under QEMU automation.

Non-Goals (initially)
====================

- Advanced on-disk compatibility guarantees across format revisions.
- Feature parity with ext4 or other mature file systems.
- Optimizations that require a specialized flash translation layer beyond the
  virtualized block abstraction described in the paper.

Design Sketch
=============

DFS treats the virtualized flash layer as a single-level store:

- File data and metadata live in the virtual address space provided by the
  device layer.
- The lower layer owns physical block allocation and reclamation.
- Atomic update support in the lower layer enables simple, fast crash recovery.

The in-tree implementation will refine these ideas into Linux VFS semantics
and define the concrete on-disk format.

Current Implementation Deltas
=============================

The in-tree DFS prototype intentionally diverges from the paper in several
areas while the kernel integration is stabilized. These deltas are tracked
here to avoid confusion during development:

- **VFSL semantics:** The paper relies on a virtualized flash layer with
  atomic page updates, allocate/remap, and deallocate. The current prototype
  runs on a plain block device and uses iomap without VFSL semantics.
- **Block size:** The paper assumes 512-byte blocks; the prototype uses a
  4KB block size and writes a 4KB superblock.
- **Inode numbering/size:** The paper uses 32-bit inode numbers. The kernel
  inode numbers are 64-bit, but the on-disk inode stores a 32-bit inode
  field.
- **Inode table layout:** The paper describes a system file containing the
  inode array. The prototype writes inode records directly into a fixed inode
  table starting at block 1.
- **Chunking policy:** The paper shares chunks for small files and promotes
  to dedicated chunks for large files. The prototype assigns every inode a
  fixed 2TB chunk at `base = ino * chunk_bytes` with no small-file sharing
  or promotion.
- **Directory durability:** The paper uses a directory log for multi-block
  operations (e.g., rename). The prototype performs in-place directory
  updates without a log.
- **Deallocate/trim:** The paper uses VFSL deallocate for truncate/unlink and
  sparse reads. The prototype does not issue deallocate/trim operations.
- **Crash recovery:** The paper relies on VFSL atomicity and allocation logs.
  The prototype uses delayed `sync_filesystem()` without VFSL recovery.
- **Direct I/O emphasis:** The paper emphasizes direct I/O. The prototype
  currently uses buffered iomap paths and generic read helpers.

Virtualized Flash Storage Layer
===============================

The DFS paper assumes a virtualized flash storage layer (VFSL) that exposes
a very large sparse, block-addressed space and hides flash translation and
wear leveling. In the prototype described in the paper:

- The logical address space is 64-bit and sparse, far larger than physical
  flash capacity.
- Block size is 512 bytes; updates are page-atomic.
- Operations are read, write, and deallocate (trim). Writes implicitly
  allocate and remap logical blocks to physical pages.
- Deallocate removes mappings for a logical range and returns pages to a
  garbage collector; reads of deallocated ranges return zeros.

DFS depends on these VFSL properties rather than implementing its own block
allocator or journal.

On-disk Format
==============

The paper specifies a minimal on-flash layout built around a single logical
extent per file. The Linux implementation will start from these choices and
evolve as needed.

System region
-------------

The first allocation chunk is reserved for system metadata:

- Boot block (initial sectors).
- Superblock immediately following the boot block.
- The system file containing the inode array.

Given an inode number, its on-flash location is computed directly within the
inode array. Each inode occupies a single 512-byte block to allow atomic
updates at the VFSL layer.

Inodes
------

The paper uses 32-bit inode numbers and 32-bit block offsets. This implies:

- Up to $2^{32} - 1$ inodes (inode 0 is reserved).
- Maximum file size of 2TB with 512-byte blocks.

Each inode stores:

- inode number
- base virtual address of the file extent
- mode, link count, file size, uid/gid
- flags, generation count
- atime/ctime/btime/mtime with nanosecond resolution

The remaining inode space is left for future attributes.

Data placement and allocation chunks
------------------------------------

DFS partitions the virtual address space into allocation chunks. In the paper
the default chunk size is $2^{32}$ blocks (2TB). Files are classified as:

- **Small files:** many files share a single allocation chunk.
- **Large files:** each file owns an entire allocation chunk.

The small-file threshold is a power of two, defaulting to 32KB in the paper.
When a small file grows beyond the threshold, it is moved to a fresh chunk.
The prototype copies data; a future VFSL move operation could remap without
copying.

Because each file is a single logical extent, the logical block address for a
file offset is computed as:

``logical_address = inode.base + file_block_offset``

This keeps the data path short and allows aggressive I/O coalescing.

Directories
-----------

Directories are stored like files but contain an array of
``(name, inode, type)`` tuples. The prototype uses a UFS/FFS-style layout and
relies on a write-ahead log for directory operations that span multiple blocks
(e.g., rename). A future design goal is to replace this with hashing plus
VFSL-supported atomic multi-block updates.

Mount Options
=============

To be defined. When available, options and defaults will be documented here.

Crash Recovery and Truncation
=============================

DFS relies on VFSL atomic page updates and the VFSL allocation log for crash
recovery. DFS does not journal data or metadata in the file system layer.
File system consistency after a crash is provided by the VFSL mapping log;
application-level consistency is still the responsibility of user space.

DFS uses the VFSL deallocate operation to implement truncate and unlink. This
allows sparse files and returns unmapped regions as zeros on read.

Direct Access and Buffering
===========================

The paper emphasizes direct I/O for workloads such as databases. The Linux
prototype still supports the page cache for mmap/exec, but direct I/O is the
preferred path for performance and to avoid double buffering.

Testing
=======

DFS is expected to be exercised via QEMU automation and xfstests. The helper
tools in `dfs-progs` are the canonical workflow for building kernels, running
VMs, and executing xfstests in a non-interactive environment.

Limitations (Paper Prototype)
=============================

- No snapshots (planned for VFSL).
- No atomic multi-block updates in VFSL; directory updates use a simple log.
- Fixed small-file threshold and chunk size at mkfs time.
- Large files are not demoted when shrinking.

References
==========

- DFS paper: "DFS: A File System for Virtualized Flash Storage" (Josephson
  et al.). A plain-text copy of the paper is stored at the repo root as
  `dfs_paper.txt` for quick reference.
