#!/usr/bin/env python3
"""Download the packages build_vulkan.sh needs, on the Windows side.

  python -I tools/llama/fetch_vulkan_debs.py <out dir> [WSL's Ubuntu codename]

Three sets, each into its own folder under <out dir>:

  riscv64/  Ubuntu 24.04 (noble) riscv64: glibc, the kernel headers, the Vulkan
            loader and its headers, libgcc and libstdc++ -- the sysroot. The
            guest is 24.04, so this is the glibc the binary must link against.
  amd64/    glslc and its libraries, for the WSL distro that builds (its
            codename, default resolute): it compiles the shaders there.
  headers/  SPIR-V headers, header-only and unpacked into the sysroot; the
            build machine's current ones rather than 24.04's, since
            ggml-vulkan follows new SPIR-V extensions.

Here rather than in WSL because WSL often has no network of its own (a VPN on
the host); WSL only unpacks what this leaves. A set already downloaded is not
fetched again.
"""
import gzip
import pathlib
import sys
import urllib.request

PORTS = "http://ports.ubuntu.com/ubuntu-ports"
ARCHIVE = "http://archive.ubuntu.com/ubuntu"
SYSROOT = ["libc6", "libc6-dev", "linux-libc-dev", "libcrypt-dev", "libcrypt1", "libvulkan1", "libvulkan-dev",
           "libgcc-s1", "libgcc-14-dev", "libstdc++6", "libstdc++-14-dev", "libatomic1"]


def index(base, dist, arch):
    """Package name -> its Packages stanza, -updates over the release."""
    packages = {}
    for component in ("main", "universe"):
        for suite in (dist, dist + "-updates"):
            url = f"{base}/dists/{suite}/{component}/binary-{arch}/Packages.gz"
            try:
                data = gzip.decompress(urllib.request.urlopen(url, timeout=60).read()).decode()
            except Exception as e:
                print("skip", url, e)
                continue
            for stanza in data.split("\n\n"):
                fields = dict(l.split(": ", 1) for l in stanza.splitlines() if ": " in l and not l.startswith(" "))
                if "Package" in fields:
                    packages[fields["Package"]] = fields
    return packages


def get(base, packages, names, out):
    out.mkdir(parents=True, exist_ok=True)
    for name in names:
        filename = packages[name]["Filename"]
        dst = out / filename.split("/")[-1]
        if not dst.exists():
            print("get", dst.name)
            urllib.request.urlretrieve(f"{base}/{filename}", dst)


def closure(packages, root):
    """root and what it depends on, leaving out what every system has."""
    need, seen = [root], set()
    while need:
        name = need.pop()
        if name in seen or name not in packages:
            continue
        seen.add(name)
        for dep in packages[name].get("Depends", "").split(","):
            dep = dep.split("|")[0].strip().split(" ")[0].split(":")[0]
            if dep and not dep.startswith(("libc6", "libgcc", "libstdc++")):
                need.append(dep)
    return sorted(seen)


def main():
    out = pathlib.Path(sys.argv[1])
    host = sys.argv[2] if len(sys.argv) > 2 else "resolute"
    if not any((out / "riscv64").glob("libvulkan-dev_*.deb")):
        get(PORTS, index(PORTS, "noble", "riscv64"), SYSROOT, out / "riscv64")
    if not any((out / "amd64").glob("glslc_*.deb")) or not any((out / "headers").glob("*.deb")):
        packages = index(ARCHIVE, host, "amd64")
        get(ARCHIVE, packages, closure(packages, "glslc"), out / "amd64")
        get(ARCHIVE, packages, ["spirv-headers"], out / "headers")


if __name__ == "__main__":
    main()
