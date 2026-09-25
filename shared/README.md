# Shared folder

This folder is shared live with a Linux guest. Put a file here from Windows
and the guest sees it straight away; write one from the guest and it is here
straight away. Nothing needs copying and neither side has to shut down.

It is served to the guest over virtio-9p, the same mechanism QEMU's shared
folders use. Inside the guest:

```
mkdir -p /mnt/shared
mount -t 9p -o trans=virtio,version=9p2000.L shared /mnt/shared
```

`shared` is the mount tag. To mount it at every Ubuntu boot, add this line to
the guest's `/etc/fstab`:

```
shared  /mnt/shared  9p  trans=virtio,version=9p2000.L,nofail  0  0
```

`nofail` keeps the boot going on a run with no shared folder.

## How it differs from drives/

`drives/` holds disk images. The guest owns what is inside one, so Windows can
only look at a drive after the guest has let go of it -- right for bulk storage,
wrong for handing a file across. This folder is the other way round: both sides
see the same files at the same time. Use it to move files in and out; use a
drive for anything that needs to be a real Linux filesystem.

## What it is and is not

Windows folders have no owners, permission bits, symlinks or case-sensitive
names, so the guest sees an approximation:

- **Every file belongs to root,** readable and writable by everyone. The
  Windows read-only attribute shows as a file without write permission, and
  `chmod -w` sets it.
- **Names are case-insensitive,** as on Windows: `Notes.txt` and `notes.txt`
  are the same file.
- **Names Windows cannot hold are refused** -- `a:b`, `what?`, `CON`, a name
  ending in a dot or space -- with an error, rather than being quietly
  altered into a different name.
- **No symlinks, device nodes or extended attributes.** Creating one fails
  with "Operation not supported".
- **It is not a place for a root filesystem, a database or a git
  repository's internals.** Those depend on exactly the POSIX behaviour
  listed above. A drive image is the right home for them.
- **The guest cannot reach outside this folder.** A Windows link or junction
  inside it that points elsewhere is refused.
- **Times come from the guest, not from Windows.** A file or directory the
  guest creates or changes is stamped with the guest's time: 2024-01-01 plus
  the instruction count as nanoseconds. The stamp is written to the Windows
  file as well, so Explorer shows the same time the guest does -- which is why
  a file saved from Linux can look like it was modified in 2024. Access and
  change times read the same as the modification time.
- **Inode numbers are the guest's own.** They count up from 1 in the order the
  guest first sees each file, instead of being the NTFS file index, which
  would be different on every run.
- **`df` reports a fixed 1 TiB free.** The real free space of the Windows disk
  changes with everything else the host does. A write the disk cannot fit
  still fails, at the write.

All of that is so that a run using the folder is deterministic: given the
same starting contents, the guest sees exactly the same folder on every run.

`-shared=<dir>` serves a different folder, and `-shared=` turns it off.

## Handing a program to the guest

The share is the easy way to get a binary in, but Windows has no execute bit,
so everything here arrives in the guest as `-rw-rw-rw-` and running it in
place gives `Permission denied`. Copy it into the guest first:

```sh
cp /mnt/shared/myprogram /root/ && chmod +x /root/myprogram
/root/myprogram
```

Data files need none of that -- reading them from `/mnt/shared` is fine, which
is what you want for anything large. The main README's
[Running your own programs in the guest](../README.md#own-programs) covers
cross-compiling for the guest, with the static linking that saves you from its
glibc.
