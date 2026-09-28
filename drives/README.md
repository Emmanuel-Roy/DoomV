# Storage drives

Every `*.img` file in this folder is attached to a Linux guest as a virtio
disk when DoomV boots. They are raw disk images -- the bytes of the image are
the bytes of the disk -- so anything a real disk can hold, one of these can.

```
python scripts/mkdrive.py data 1G     # drives/data.img, 1 GiB, formatted ext4
python scripts/boot.py ubuntu         # or linux; the drives come along
```

Inside the guest:

```
cat /proc/partitions        # the drives are listed as vdb, vdc, ...
mkdir -p /mnt/data
mount /dev/vdb /mnt/data
```

## Which drive gets which name

Linux numbers virtio disks in the order it finds them, and DoomV attaches the
images in the order their names sort. With a root disk -- Ubuntu's
`ubuntu.img` -- that is `vda`, and the drives here are `vdb`, `vdc` and so on.
Without one, as in the BusyBox boot, they start at `vda`.

Adding or removing an image moves the names of the ones that sort after it.
If a guest mounts drives from `/etc/fstab`, mount them by filesystem label or
UUID rather than by `/dev/vdX` name: `mkdrive.py` labels each drive with its
name, so `LABEL=data` finds `data.img` wherever it lands.

## Limits

- **Eight drives.** The device tree declares eight slots. Images past the
  eighth, in name order, are ignored with a message.
- **Attached at boot.** The folder is read once, when DoomV starts. There is
  no hot-plug; restart the guest to pick up a new image.
- **Read-only images stay read-only.** An image file DoomV cannot open for
  writing is attached read-only, and the guest is told, so a mount comes up
  read-only instead of failing on its first write.
- **One emulator at a time.** Two DoomV instances writing the same image will
  corrupt it, exactly as two computers sharing one disk would.
- **Shut the guest down first.** A drive is only consistent on the host once
  the guest has unmounted it or powered off. Copy or inspect an image after
  that, not while it is mounted.

The folder is optional. `-drives=<dir>` points DoomV at a different one, and
`-drives=` turns the scan off.
