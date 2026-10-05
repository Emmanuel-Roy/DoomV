# Venus for DoomV

`-gpu` gives a Linux guest Vulkan on the host GPU, through Mesa's Venus
driver in the guest and virglrenderer's Venus renderer on the host. This
folder is what DoomV changes about the host side.

```
python scripts/get_venus.py      # build it: about two minutes, into build/venus/
python scripts/boot.py ubuntu --gpu
```

## Where it comes from

Upstream virglrenderer runs Venus only on Linux. winq-emu's fork,
[winq-emu-virglrenderer](https://github.com/cmspam/winq-emu-virglrenderer)
(MIT, on virglrenderer 1.3.0), ports it to Windows: Win32 handles in place of
file descriptors, threads in place of the render server's processes.
`get_venus.py` builds that fork at a pinned commit, with
[doomv-sync.patch](doomv-sync.patch) applied, using this repository's Clang,
the Khronos Vulkan headers (the protocol needs newer ones than MSYS2 has) and
a few MSYS2 packages.

## What the patch does

Venus normally runs on threads of its own: one per command ring, reading
what the guest writes there as it arrives; one per context, marking the
rings alive; one per queue, retiring fences as the GPU passes them. Each
writes into memory the guest shares, whenever it gets to it. That is fast,
and it means the guest sees GPU results at instructions that depend on host
timing -- which DoomV does not allow. The patch adds a synchronous mode
(`VKR_RENDERER_SYNC`) with none of those threads:

- **`virgl_doomv_venus_step`** runs every ring until none can make progress,
  waits for every queue that was given work, and marks the rings idle and
  alive. DoomV calls it after each command submitted to a Venus context and
  at every input point -- the same instructions on every run.
- **A ring that waits** for a virtqueue seqno not yet submitted stops before
  that command and resumes on a later step; a virtqueue command that waits
  for a ring runs the ring until it gets there.
- **A fence** submitted on a ring is waited for and retired in the call.
- **Memory is mapped, not exported.** A blob of host-visible
  `VkDeviceMemory` is mapped with `vkMapMemory` and the pointer handed to
  DoomV (`virgl_doomv_venus_map_resource`), which places it in the guest's
  host-memory window. Nothing is external, so the fork's Windows shim that
  makes every buffer and image external (to export their memory) is left
  out -- on AMD it narrows a buffer's memory types to device-local only,
  which leaves the guest nothing it can map.
- **Entry points** for driving Venus in-process, on the caller's thread,
  without the render server: `virgl_doomv_venus_*` in `virglrenderer.h`.

## Testing it

[vkfill.c](vkfill.c) needs no window: the GPU fills a buffer and copies it
to another, both in host-visible memory, and the CPU checks every word.
Cross-compile it (`riscv64-linux-gnu-gcc -O2 -I<Vulkan-Headers>/include
vkfill.c -o vkfill -ldl`), put it in the guest, and run it; two runs of the
same session stopped at the same instruction must leave identical
`crash.log` files.

## Record and replay

At every sync point -- a tick, or a command that runs Venus -- DoomV
compares Venus's memory with a mirror of what the guest last had: shared
memory always, device memory when GPU work ran. The runs that differ are
logged by `-record` as `<instruction> vsync <kind> <resource>:<offset>:<bytes>`,
and a `-replay` writes them into plain buffers standing in for the host's,
at the same instructions, with no GPU.

## Presenting

A Vulkan program in a window presents by copying its frames through the
CPU (`MESA_VK_WSI_DEBUG=sw`, set by the desktop sessions): the frames are
host-visible memory the guest can map now that nothing is external. Handing
the image to X directly, as a dma-buf, would need the host to import Vulkan
memory into OpenGL, which the fork rules out on Windows.
