#!/usr/bin/env python3
"""conform-lgx.py — make a mkLogosModule .lgx signable by liblgx.

WHY THIS EXISTS (upstream conflict, 2026-09):
  The current logos-module-builder (mkLogosModule) unconditionally publishes the
  module's LIDL to a ROOT `assets/lidl/` entry (see nix-bundle-lgx; its own tests
  assert the entry exists). But liblgx's package validator — used by BOTH `lgx sign`
  and install — only permits these root entries:
      manifest.json, manifest.sig, variants, docs, licenses
  (logos-package src/core/package.cpp: ALLOWED_ROOT_ENTRIES). A root `assets/`
  therefore fails with "Forbidden root entry: assets", so NO module built with the
  current builder can be signed. Track upstream: reconcile nix-bundle-lgx (emits
  root assets/) with logos-package liblgx (forbids it).

WHAT THIS DOES (deterministic, reversible-by-rebuild):
  Strips the root `assets/` tree and rewrites manifest.json so the package is
  format-valid again:
    - drops the `assets` content-hash entry,
    - recomputes the `root` Merkle hash over the remaining top-level dir hashes,
      using liblgx's exact scheme: root = SHA256( concat over sorted top-level
      dirs of  name + '\\0' + <dir-hash-hex> + '\\n' )   [computeParentDirectoryHash].
      The per-dir hashes (variants, variants/<v>, docs, …) are unchanged by the
      strip, so we reuse them from the manifest — verified to reproduce the
      original root exactly before we change anything.
    - blanks a manifest `icon` that pointed into the removed assets/ (cosmetic).
  The LIDL is a build-time codegen aid for dependents; it is not needed at runtime
  (installed modules carry no lidl), so stripping it does not affect loading.

USAGE: conform-lgx.py <in.lgx> <out.lgx>
"""
import sys, os, json, hashlib, tarfile, tempfile, shutil

def sha256hex(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()

def parent_hash(children: dict) -> str:
    """liblgx computeParentDirectoryHash: sorted name\\0hash\\n concat, sha256 hex."""
    c = b""
    for name in sorted(children):
        c += name.encode() + b"\x00" + children[name].encode() + b"\n"
    return sha256hex(c)

def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, dst = sys.argv[1], sys.argv[2]
    work = tempfile.mkdtemp(prefix="conform-lgx-")
    try:
        with tarfile.open(src, "r:gz") as t:
            t.extractall(work)

        mpath = os.path.join(work, "manifest.json")
        m = json.load(open(mpath))
        hashes = m.get("hashes", {})

        # 0) sanity: reproduce the ORIGINAL root from the manifest sub-hashes before
        #    touching anything — proves our algorithm matches this package's builder.
        top_orig = {k: v for k, v in hashes.items() if "/" not in k and k != "root"}
        if "root" in hashes and parent_hash(top_orig) != hashes["root"]:
            sys.exit("ABORT: cannot reproduce original root hash — algorithm mismatch, "
                     "refusing to rewrite (package format may have changed).")

        assets_dir = os.path.join(work, "assets")
        if not os.path.isdir(assets_dir):
            print("no root assets/ — already conformant, copying through")
            shutil.copyfile(src, dst); return

        # 1) strip the forbidden root assets/ tree
        shutil.rmtree(assets_dir)

        # 2) recompute manifest hashes: drop assets, recompute root over the rest
        hashes.pop("assets", None)
        top = {k: v for k, v in hashes.items() if "/" not in k and k != "root"}
        hashes["root"] = parent_hash(top)
        m["hashes"] = hashes

        # 3) a manifest icon that lived in assets/ is now dangling — blank it (cosmetic;
        #    validation never reads it). Leaves non-assets icons untouched.
        if str(m.get("icon", "")).startswith("assets/"):
            m["icon"] = ""

        json.dump(m, open(mpath, "w"), indent=2)

        # 4) repack with GNU tar (liblgx's TarReader can't parse Python's default PAX
        #    extended headers → "Failed to load package"). manifest.json first, then
        #    the remaining allowed dirs, sorted for determinism, no assets/.
        import subprocess
        roots = ["manifest.json"] + sorted(
            d for d in os.listdir(work) if d != "manifest.json")
        subprocess.run(["tar", "--format=gnu", "-czf", os.path.abspath(dst),
                        "-C", work, *roots], check=True)
        print(f"conformed: {dst}  (root {hashes['root'][:12]}…, assets stripped)")
    finally:
        shutil.rmtree(work, ignore_errors=True)

if __name__ == "__main__":
    main()
