#!/usr/bin/env python3
"""Read-only NUP (NX Package) container tool. Stdlib only (Python >= 3.14 for zstd).

Format per Lichii src/core/file_sys/nup_container.{h,cpp}:
  Superblock 0x80 ("NUP\\x01"), TOC (header 0x30 "TOC\\x01", package records 0x40,
  dir records 0x18, file records 0x50, u32 children array, string pool), blob region.
  children[i] < dir_count -> dir index, else file index = child - dir_count.
  File codec 0 = raw extent, 1 = zstd frame.

Usage (run with python3 -I, container path as an argument):
  nup_tool.py NUP info
  nup_tool.py NUP ls-exefs [PKG]
  nup_tool.py NUP tree [PKG] OUT.txt          # full romfs tree: path size codec extent_off extent_size
  nup_tool.py NUP extract-exefs [PKG] OUTDIR
  nup_tool.py NUP extract [PKG] GLOB OUTDIR   # glob over romfs-relative paths (fnmatch)
  nup_tool.py NUP cat [PKG] PATH              # raw member bytes to stdout
PKG defaults to 0.
"""
import fnmatch
import hashlib
import json
import mmap
import os
import struct
import sys

NODIR = 0xFFFFFFFF
KINDS = {0: "Base", 1: "Update", 2: "Addon"}


class Nup:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.mm = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        mm = self.mm
        if mm[0:4] != b"NUP\x01":
            raise SystemExit("not a NUP file")
        (hsize, hver, self.toc_off, self.toc_size) = struct.unpack_from("<HHQQ", mm, 4)
        self.toc_sha = bytes(mm[0x18:0x38])
        (self.blob_off, self.file_size, self.pkg_count, self.flags) = struct.unpack_from("<QQII", mm, 0x38)
        self.uuid = bytes(mm[0x50:0x60]).hex()
        toc = bytes(mm[self.toc_off:self.toc_off + self.toc_size])
        self.toc_ok = hashlib.sha256(toc).digest() == self.toc_sha
        magic, pc, dc, fc, pool_off, pool_size, _ = struct.unpack_from("<IIQQQQQ", toc, 0)
        assert magic == 0x01434F54, hex(magic)
        self.dir_count, self.file_count = dc, fc
        pool = toc[pool_off:pool_off + pool_size]
        name = lambda o, n: pool[o:o + n].decode("utf-8", "replace")
        pos = 0x30
        self.packages = []
        for _ in range(pc):
            tid, root, exe, rom, ver, kind, flg = struct.unpack_from("<QIIIIBB", toc, pos)
            moff, msz = struct.unpack_from("<QI", toc, pos + 0x20)
            self.packages.append(dict(title_id=tid, root_dir=root, exefs_dir=exe, romfs_dir=rom,
                                      version=ver, kind=KINDS.get(kind, kind),
                                      standalone_update=bool(flg & 1), meta_offset=moff, meta_size=msz))
            pos += 0x40
        self.dirs = []
        for _ in range(dc):
            no, nl, first, cnt = struct.unpack_from("<IIII", toc, pos)
            self.dirs.append((name(no, nl), first, cnt))
            pos += 0x18
        self.files = []
        for _ in range(fc):
            no, nl, size, eo, es = struct.unpack_from("<IIQQQ", toc, pos)
            sha = toc[pos + 0x20:pos + 0x40]
            codec = toc[pos + 0x40]
            self.files.append(dict(name=name(no, nl), size=size, extent_offset=eo,
                                   extent_size=es, sha256=sha.hex(), codec=codec))
            pos += 0x50
        n = (pool_off - pos) // 4
        self.children = struct.unpack_from("<%dI" % n, toc, pos)

    def meta(self, pkg):
        p = self.packages[pkg]
        out = {}
        if not p["meta_offset"]:
            return out
        blk = self.mm[p["meta_offset"]:p["meta_offset"] + p["meta_size"]]
        pos = 0
        names = {1: "npdm", 3: "icon", 4: "display_version", 5: "title_name", 6: "publisher",
                 8: "source_dump", 9: "localized_titles", 10: "localized_icons"}
        while pos + 8 <= len(blk):
            t, _, sz = struct.unpack_from("<HHI", blk, pos)
            pos += 8
            payload = blk[pos:pos + sz]
            pos += sz
            k = names.get(t, "type_%d" % t)
            if k in ("display_version", "title_name", "publisher", "source_dump"):
                out[k] = payload.decode("utf-8", "replace")
            else:
                out[k] = "%d bytes" % sz
        return out

    def walk(self, dir_index, prefix=""):
        """Yield (relative_path, file_index) for every file below dir_index."""
        _, first, cnt = self.dirs[dir_index]
        for slot in range(first, first + cnt):
            c = self.children[slot]
            if c >= self.dir_count:
                fi = c - self.dir_count
                yield prefix + self.files[fi]["name"], fi
            else:
                yield from self.walk(c, prefix + self.dirs[c][0] + "/")

    def read(self, fi):
        f = self.files[fi]
        if f["size"] == 0:
            return b""
        raw = self.mm[f["extent_offset"]:f["extent_offset"] + f["extent_size"]]
        if f["codec"] == 0:
            return bytes(raw)
        from compression import zstd
        data = zstd.decompress(raw)
        assert len(data) == f["size"], "size mismatch"
        return data


def safe_out(outdir, rel):
    # Reject absolute paths and .. components coming from untrusted names.
    parts = [p for p in rel.split("/") if p not in ("", ".")]
    if any(p == ".." or "\x00" in p for p in parts):
        raise ValueError("unsafe path: %r" % rel)
    dst = os.path.join(outdir, *parts)
    if not os.path.realpath(dst).startswith(os.path.realpath(outdir) + os.sep):
        raise ValueError("escapes outdir: %r" % rel)
    return dst


def extract(n, pkg_dir, pattern, outdir, verify=True):
    count = total = 0
    for rel, fi in n.walk(pkg_dir):
        if not fnmatch.fnmatchcase(rel, pattern):
            continue
        data = n.read(fi)
        if verify and hashlib.sha256(data).hexdigest() != n.files[fi]["sha256"]:
            print("SHA MISMATCH", rel, file=sys.stderr)
        dst = safe_out(outdir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as fh:
            fh.write(data)
        count += 1
        total += len(data)
    print("extracted %d files, %d bytes -> %s" % (count, total, outdir))


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    n = Nup(argv[1])
    cmd, rest = argv[2], argv[3:]
    pkg = 0
    if rest and rest[0].isdigit():
        pkg, rest = int(rest[0]), rest[1:]
    p = n.packages[pkg] if n.packages else None
    if cmd == "info":
        print("file_size=%d toc_off=0x%x toc_size=%d toc_sha_ok=%s blob_off=0x%x packages=%d dirs=%d files=%d uuid=%s"
              % (n.file_size, n.toc_off, n.toc_size, n.toc_ok, n.blob_off, n.pkg_count,
                 n.dir_count, n.file_count, n.uuid))
        for i, pk in enumerate(n.packages):
            stats = {}
            for key in ("exefs_dir", "romfs_dir"):
                if pk[key] != NODIR:
                    fs = [n.files[fi] for _, fi in n.walk(pk[key])]
                    stats[key] = dict(files=len(fs), bytes=sum(f["size"] for f in fs),
                                      stored=sum(f["extent_size"] for f in fs),
                                      zstd=sum(1 for f in fs if f["codec"] == 1))
            print(json.dumps(dict(index=i, **{k: (hex(v) if k == "title_id" else v) for k, v in pk.items()},
                                  meta=n.meta(i), stats=stats), indent=1))
    elif cmd == "ls-exefs":
        for rel, fi in n.walk(p["exefs_dir"]):
            f = n.files[fi]
            print("%10d codec=%d %s" % (f["size"], f["codec"], rel))
    elif cmd == "tree":
        with open(rest[0], "w") as out:
            out.write("# path\tsize\tcodec\textent_offset\textent_size\n")
            for rel, fi in n.walk(p["romfs_dir"]):
                f = n.files[fi]
                out.write("%s\t%d\t%d\t%d\t%d\n" % (rel, f["size"], f["codec"], f["extent_offset"], f["extent_size"]))
        print("wrote", rest[0])
    elif cmd == "extract-exefs":
        extract(n, p["exefs_dir"], "*", rest[0])
    elif cmd == "extract":
        extract(n, p["romfs_dir"], rest[0], rest[1])
    elif cmd == "cat":
        for rel, fi in n.walk(p["romfs_dir"]):
            if rel == rest[0]:
                sys.stdout.buffer.write(n.read(fi))
                return 0
        print("not found", file=sys.stderr)
        return 1
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
