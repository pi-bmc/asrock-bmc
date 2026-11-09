#!/usr/bin/env python3
"""Yocto sstate + downloads cache for .github/workflows/build.yaml, kept in GHCR.

    yocto-cache.py restore KIND...
    yocto-cache.py save [--prune] KIND...      KIND = sstate | downloads

Each KIND is one OCI artifact per scope (branch), tagged `<kind>-<scope>` and
also `<kind>-latest`. Restore tries the run's own scope first, then the
fallbacks (default: latest).

Why it is shaped like this:

* GHCR caps a layer at 10 GB and an upload at 10 minutes, and an OpenBMC
  sstate-cache is bigger than that. So the cache is split into many shard
  layers instead of being pushed as one tarball.
* Shards are byte-reproducible tars (sorted file list, fixed mtime, owner and
  mode). Each one is keyed by recipe (sstate) or download entry (downloads),
  so a commit that touches three recipes changes about three shards. A shard
  whose file list is unchanged is reused from the previous manifest without
  being re-tarred. A shard that re-tars to an existing digest is not uploaded.
* Without pruning the cache would only ever grow. Restored files carry mtime 0,
  and bitbake touches every sstate object it restores (sstate_unpack_package)
  and every download's .done stamp it verifies (fetch2.update_stamp). So at
  save time, "mtime > 0" means "used by this run". An LRU index of the
  generation that last used each unit travels with the artifact. `save --prune`
  evicts the least recently used units once the cache exceeds its budget.
* hash equivalence is on (OEEquivHash). sstate objects are named by unihash, so
  hashserv.db must travel with them or equivalent objects can't be found. It is
  snapshotted with SQLite's backup API, because the server keeps writing to it
  while the build runs.
* Checkpoint saves run while bitbake is writing. Only complete files are
  archived: finished sstate objects, and download units that have a .done
  stamp. Restore extracts each shard into a staging directory and moves the
  files in only after the blob verified, so a broken transfer can't leave a
  truncated sstate object behind.

Environment:
  CACHE_IMAGE              ghcr.io/<owner>/<repo>/yocto-cache
  SSTATE_DIR, DL_DIR       directories to cache (absolute)
  CACHE_SCOPE              this run's scope, usually the branch name
  CACHE_FALLBACK_SCOPES    scopes to restore from if CACHE_SCOPE has no cache
                           (space separated, default "latest")
  CACHE_SSTATE_BUDGET_GB   LRU budget for `save --prune` (default 20)
  CACHE_DOWNLOADS_BUDGET_GB                              (default 16)
  CACHE_JOBS               parallel shard transfers (default 4)
  ORAS_FLAGS               extra oras flags, e.g. "--plain-http" for testing
  RUNNER_TEMP              scratch + per-run state (set by GitHub Actions)
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import contextlib
import dataclasses
import datetime
import fcntl
import gzip
import hashlib
import json
import os
import re
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable

IMAGE = os.environ.get("CACHE_IMAGE", "").lower()  # registry paths must be lowercase
ORAS_FLAGS = os.environ.get("ORAS_FLAGS", "").split()
JOBS = int(os.environ.get("CACHE_JOBS", "4"))
TMP = Path(os.environ.get("RUNNER_TEMP") or tempfile.gettempdir())
STATE = TMP / "yocto-cache-state"

ARTIFACT_TYPE = "application/vnd.yocto.cache.v2"
MT_SHARD = "application/vnd.yocto.cache.shard.v1.tar"
MT_INDEX = "application/vnd.yocto.cache.index.v1.json+gzip"
MT_HASHSERV = "application/vnd.yocto.cache.hashserv.v1.sqlite3"
EMPTY_CONFIG = {
    "mediaType": "application/vnd.oci.empty.v1+json",
    "digest": "sha256:44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a",
    "size": 2,
    "data": "e30=",
}
TITLE = "org.opencontainers.image.title"
FINGERPRINT = "yocto-cache.fingerprint"
GiB = 1 << 30
# Units at least this big get a layer of their own, named after the unit, so a
# multi-GB git clone (the kernel) is uploaded once and then reused as-is.
BIG_UNIT = 1 * GiB
# GHCR's per-layer limit is 10 GB; stay clear of it.
MAX_LAYER = 9 * GiB

TAR_CREATE = [
    "tar", "--create", "--file=-", "--format=gnu",
    "--owner=0", "--group=0", "--numeric-owner", "--mtime=@0",
    "--mode=u=rwX,go=rX", "--hard-dereference", "--no-recursion",
    # A checkpoint can race bitbake; a file that vanished is simply skipped.
    "--ignore-failed-read", "--warning=no-file-changed", "--warning=no-file-removed",
    "--null", "--no-unquote", "--verbatim-files-from",
]


def log(msg: str) -> None:
    print(msg, flush=True)


def warn(msg: str) -> None:
    print(f"::warning::{msg}", flush=True)


def summary(line: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a") as f:
            f.write(line + "\n")


def human(n: float) -> str:
    if n < 1024:
        return f"{int(n)} B"
    for unit in ("KiB", "MiB"):
        n /= 1024
        if n < 1024:
            return f"{n:.1f} {unit}"
    return f"{n / 1024:.1f} GiB"


# --------------------------------------------------------------------------- oras


def oras(sub: list[str], args: list[str], *, retries: int = 1, **kw) -> subprocess.CompletedProcess:
    cmd = ["oras", *sub, *ORAS_FLAGS, *args]
    for attempt in range(1, retries + 1):
        p = subprocess.run(cmd, capture_output=True, **kw)
        if p.returncode == 0 or attempt == retries:
            return p
        time.sleep(5 * attempt)
    raise AssertionError("unreachable")


def oras_check(sub: list[str], args: list[str], **kw) -> subprocess.CompletedProcess:
    p = oras(sub, args, retries=3, **kw)
    if p.returncode != 0:
        raise RuntimeError(f"oras {' '.join(sub)} failed: {p.stderr.decode(errors='replace').strip()}")
    return p


def fetch_manifest(tag: str) -> dict | None:
    p = oras(["manifest", "fetch"], [f"{IMAGE}:{tag}"])
    if p.returncode != 0:
        return None
    m = json.loads(p.stdout)
    return m if m.get("artifactType") == ARTIFACT_TYPE else None


def fetch_blob(digest: str) -> bytes:
    return oras_check(["blob", "fetch"], ["--output", "-", f"{IMAGE}@{digest}"]).stdout


def blob_exists(digest: str) -> bool:
    return oras(["blob", "fetch"], ["--descriptor", f"{IMAGE}@{digest}"]).returncode == 0


def push_blob(path: Path, digest: str) -> bool:
    """Push a blob unless the registry already has it. Returns True if uploaded."""
    if blob_exists(digest):
        return False
    oras_check(["blob", "push"], [f"{IMAGE}@{digest}", str(path)])
    return True


def file_digest(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return "sha256:" + h.hexdigest(), path.stat().st_size


def push_file(path: Path, media_type: str, title: str) -> tuple[dict, bool]:
    digest, size = file_digest(path)
    uploaded = push_blob(path, digest)
    return {"mediaType": media_type, "digest": digest, "size": size,
            "annotations": {TITLE: title}}, uploaded


# --------------------------------------------------------------------------- kinds

Member = tuple[str, os.stat_result]  # (path relative to the cache root, lstat)


@dataclasses.dataclass
class Kind:
    name: str
    root: Path
    shards: int
    budget: int
    unit_of: Callable[[str], str | None]   # rel path -> eviction unit (None = skip)
    shard_key: Callable[[str], str]        # unit -> key that picks its shard
    track_mtime: bool                      # include mtime in the shard fingerprint
    complete: Callable[[list[Member]], bool]
    include_empty_dirs: bool


def sstate_unit(rel: str) -> str | None:
    # A package and its .siginfo/.sig/.done move together. Anything else in the
    # tree (hashserv.db, locks, bitbake's NamedTemporaryFile names that end in
    # random characters) is skipped.
    name = rel.rsplit("/", 1)[-1]
    if not name.startswith("sstate:"):
        return None
    i = rel.rfind(".tar.zst")
    if i < 0 or rel[i:] not in (".tar.zst", ".tar.zst.siginfo", ".tar.zst.sig", ".tar.zst.done"):
        return None
    return rel[: i + len(".tar.zst")]


def sstate_shard_key(unit: str) -> str:
    # sstate:<pn>:<pkgarch>:... -> shard by recipe, so one changed recipe
    # changes one shard.
    return unit.rsplit("/", 1)[-1].split(":")[1]


def downloads_unit(rel: str) -> str | None:
    # foo.tar.gz + foo.tar.gz.done            -> "foo.tar.gz"
    # git2/<repo>/... + git2/<repo>.done      -> "git2/<repo>"
    # uninative/<sha>/<tarball>(.done)        -> "uninative/<sha>"
    if rel.endswith(".lock"):
        return None
    parts = rel.split("/", 2)
    head = parts[0] if len(parts) == 1 else f"{parts[0]}/{parts[1]}"
    return head[: -len(".done")] if head.endswith(".done") else head


def kinds() -> dict[str, Kind]:
    def env_path(var: str) -> Path:
        v = os.environ.get(var)
        if not v:
            sys.exit(f"{var} must be set")
        return Path(v)

    return {
        "sstate": Kind(
            name="sstate",
            root=env_path("SSTATE_DIR"),
            shards=64,
            budget=int(float(os.environ.get("CACHE_SSTATE_BUDGET_GB", "20")) * GiB),
            unit_of=sstate_unit,
            shard_key=sstate_shard_key,
            # sstate objects are named by hash and never rewritten, so the file
            # list alone identifies a shard's content.
            track_mtime=False,
            complete=lambda members: True,
            include_empty_dirs=False,
        ),
        "downloads": Kind(
            name="downloads",
            root=env_path("DL_DIR"),
            shards=32,
            budget=int(float(os.environ.get("CACHE_DOWNLOADS_BUDGET_GB", "16")) * GiB),
            unit_of=downloads_unit,
            shard_key=lambda unit: unit,
            # git clones get updated in place.
            track_mtime=True,
            # No .done stamp = fetch in progress or failed; don't cache it.
            complete=lambda members: any(rel.endswith(".done") for rel, _ in members),
            # A bare git repo is not a repo without its (possibly empty) refs/.
            include_empty_dirs=True,
        ),
    }


def scan(kind: Kind) -> dict[str, list[Member]]:
    units: dict[str, list[Member]] = {}
    root = str(kind.root)
    for dirpath, dirnames, filenames in os.walk(root):
        reldir = os.path.relpath(dirpath, root)
        prefix = "" if reldir == "." else reldir + "/"
        names = list(filenames) + [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]
        entries = [(prefix + n, os.path.join(dirpath, n)) for n in names]
        # Non-empty directories are recreated by their contents on extract;
        # empty ones have to be archived explicitly.
        if kind.include_empty_dirs and prefix and not dirnames and not filenames:
            entries.append((reldir, dirpath))
        for rel, path in entries:
            unit = kind.unit_of(rel)
            if unit is None:
                continue
            try:
                st = os.lstat(path)
            except FileNotFoundError:
                continue
            if not (stat.S_ISREG(st.st_mode) or stat.S_ISLNK(st.st_mode) or stat.S_ISDIR(st.st_mode)):
                continue
            units.setdefault(unit, []).append((rel, st))
    return {u: m for u, m in units.items() if kind.complete(m)}


def unit_size(members: list[Member]) -> int:
    return sum(st.st_size for _, st in members if not stat.S_ISDIR(st.st_mode))


def unit_used(members: list[Member]) -> bool:
    # Restored files have mtime 0; bitbake touched or created everything newer.
    return any(st.st_mtime > 1 for _, st in members if not stat.S_ISDIR(st.st_mode))


def layer_id(kind: Kind, unit: str, size: int) -> str:
    key = hashlib.sha1(kind.shard_key(unit).encode()).digest()
    if size >= BIG_UNIT:
        return f"{kind.name}-big-{hashlib.sha1(unit.encode()).hexdigest()[:12]}"
    return f"{kind.name}-{int.from_bytes(key[:4], 'big') % kind.shards:03d}"


def fingerprint(kind: Kind, members: list[Member]) -> str:
    h = hashlib.sha256()
    for rel, st in sorted(members):
        kind_char = "d" if stat.S_ISDIR(st.st_mode) else "l" if stat.S_ISLNK(st.st_mode) else "f"
        size = 0 if kind_char == "d" else st.st_size
        mtime = int(st.st_mtime) if kind.track_mtime and kind_char != "d" else 0
        mode = st.st_mode & 0o111
        h.update(f"{rel}\0{kind_char}\0{size}\0{mtime}\0{mode}\n".encode(errors="surrogateescape"))
    return h.hexdigest()


# --------------------------------------------------------------------------- state


def tags_for(kind: Kind, scopes: list[str]) -> list[str]:
    out: list[str] = []
    for scope in scopes:
        tag = re.sub(r"[^A-Za-z0-9_.-]", "-", f"{kind.name}-{scope}")[:128]
        if tag not in out:
            out.append(tag)
    return out


def scope() -> str:
    return os.environ.get("CACHE_SCOPE") or "latest"


def fallback_scopes() -> list[str]:
    return os.environ.get("CACHE_FALLBACK_SCOPES", "latest").split()


def load_state(kind: Kind) -> dict:
    try:
        return json.loads((STATE / f"{kind.name}.json").read_text())
    except FileNotFoundError:
        return {"manifest": None, "index": {"generation": 0, "units": {}}, "run_gen": 1}


def store_state(kind: Kind, st: dict) -> None:
    STATE.mkdir(parents=True, exist_ok=True)
    tmp = STATE / f"{kind.name}.json.tmp"
    tmp.write_text(json.dumps(st))
    tmp.replace(STATE / f"{kind.name}.json")


@contextlib.contextmanager
def locked():
    """Serialize saves: the final save must not overlap a running checkpoint."""
    STATE.mkdir(parents=True, exist_ok=True)
    with open(STATE / ".lock", "w") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        yield


# --------------------------------------------------------------------------- restore


def move_tree(src: Path, dest: Path) -> int:
    moved = 0
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        target = dest if rel == "." else dest / rel
        target.mkdir(parents=True, exist_ok=True)
        for name in list(filenames) + [d for d in dirnames if os.path.islink(os.path.join(dirpath, d))]:
            os.replace(os.path.join(dirpath, name), target / name)
            moved += 1
    return moved


def extract_layer(kind: Kind, layer: dict) -> int:
    title = layer["annotations"][TITLE]
    staging = kind.root.parent / f".{kind.root.name}.restore.{title}"
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        fetch = subprocess.Popen(
            ["oras", "blob", "fetch", *ORAS_FLAGS, "--output", "-", f"{IMAGE}@{layer['digest']}"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        tar = subprocess.Popen(["tar", "-x", "-f", "-", "-C", str(staging), "--no-same-owner"],
                               stdin=fetch.stdout, stderr=subprocess.PIPE)
        fetch.stdout.close()
        tar_err = tar.communicate()[1]
        fetch_err = fetch.communicate()[1]
        # oras verifies the digest only after streaming everything, so nothing
        # is moved into the cache until both sides succeeded.
        if fetch.returncode or tar.returncode:
            raise RuntimeError(f"{title}: oras rc={fetch.returncode} tar rc={tar.returncode}: "
                               f"{(fetch_err or tar_err).decode(errors='replace').strip()}")
        return move_tree(staging, kind.root)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def restore(kind: Kind) -> None:
    start = time.monotonic()
    kind.root.mkdir(parents=True, exist_ok=True)
    for tag in tags_for(kind, [scope(), *fallback_scopes()]):
        manifest = fetch_manifest(tag)
        if manifest:
            break
        log(f"no cache at {IMAGE}:{tag}")
    else:
        store_state(kind, {"manifest": None, "index": {"generation": 0, "units": {}}, "run_gen": 1})
        summary(f"- **{kind.name}**: no cache in the registry yet, building cold")
        return

    layers = {l["annotations"][TITLE]: l for l in manifest["layers"]}
    index = {"generation": 0, "units": {}}
    if "index.json.gz" in layers:
        index = json.loads(gzip.decompress(fetch_blob(layers["index.json.gz"]["digest"])))
    if "hashserv.db" in layers:
        db = kind.root / "hashserv.db"
        for stale in (db, db.with_name("hashserv.db-wal"), db.with_name("hashserv.db-shm")):
            stale.unlink(missing_ok=True)
        tmp = db.with_name("hashserv.db.restore")
        oras_check(["blob", "fetch"], ["--output", str(tmp), f"{IMAGE}@{layers['hashserv.db']['digest']}"])
        tmp.replace(db)

    shards = [l for l in manifest["layers"] if l["mediaType"] == MT_SHARD]
    files = failed = 0
    with cf.ThreadPoolExecutor(JOBS) as ex:
        futures = {ex.submit(extract_layer, kind, l): l for l in shards}
        for fut in cf.as_completed(futures):
            try:
                files += fut.result()
            except Exception as e:  # noqa: BLE001 - one bad shard must not sink the rest
                failed += 1
                warn(f"cache restore: {e}")
    # The next save is a new LRU generation. Recording the restored manifest
    # lets it skip re-tarring shards whose file list didn't change.
    store_state(kind, {"manifest": manifest, "index": index, "run_gen": index["generation"] + 1})
    size = sum(l["size"] for l in shards)
    took = time.monotonic() - start
    log(f"restored {kind.name} from {tag}: {len(shards) - failed}/{len(shards)} shards, "
        f"{files} files, {human(size)} in {took:.0f}s")
    summary(f"- **{kind.name}**: restored `{tag}`: {len(shards) - failed}/{len(shards)} shards, "
            f"{files} files, {human(size)} in {took:.0f}s")


# --------------------------------------------------------------------------- save


def build_layer(kind: Kind, lid: str, members: list[Member], prev: dict | None,
                workdir: Path) -> tuple[dict, str]:
    title = f"{lid}.tar"
    fp = fingerprint(kind, members)
    if prev and prev.get("annotations", {}).get(FINGERPRINT) == fp:
        return prev, "reused"
    listfile = workdir / f"{lid}.list"
    listfile.write_bytes(b"".join(os.fsencode(rel) + b"\0" for rel, _ in sorted(members)))
    out = workdir / title
    errfile = workdir / f"{lid}.err"  # a file, not a pipe: tar can't block on it
    h = hashlib.sha256()
    size = 0
    try:
        with open(out, "wb") as f, open(errfile, "wb") as err, subprocess.Popen(
                [*TAR_CREATE, "-C", str(kind.root), "--files-from", str(listfile)],
                stdout=subprocess.PIPE, stderr=err) as p:
            while chunk := p.stdout.read(1 << 20):
                h.update(chunk)
                f.write(chunk)
                size += len(chunk)
        if p.returncode not in (0, 1):  # 1 = a file changed while being read
            raise RuntimeError(f"tar {title} failed (rc={p.returncode}): "
                               f"{errfile.read_text(errors='replace').strip()}")
        digest = "sha256:" + h.hexdigest()
        uploaded = push_blob(out, digest)
    finally:
        for tmp in (out, listfile, errfile):
            tmp.unlink(missing_ok=True)
    desc = {"mediaType": MT_SHARD, "digest": digest, "size": size,
            "annotations": {TITLE: title, FINGERPRINT: fp, "yocto-cache.files": str(len(members))}}
    return desc, "uploaded" if uploaded else "unchanged"


def snapshot_hashserv(src: Path, dst: Path) -> None:
    # The live DB is WAL-mode and may be mid-write; the backup API gives a
    # consistent copy without stopping the server.
    s = sqlite3.connect(str(src), timeout=60)
    d = sqlite3.connect(str(dst))
    try:
        s.backup(d)
        d.execute("PRAGMA journal_mode=DELETE")
    finally:
        d.close()
        s.close()


def save(kind: Kind, prune: bool) -> None:
    start = time.monotonic()
    if not kind.root.is_dir():
        warn(f"cache save: {kind.root} does not exist; nothing to save for {kind.name}")
        return
    with locked():
        state = load_state(kind)
        run_gen = state["run_gen"]
        prev_layers = {l["annotations"][TITLE]: l for l in (state["manifest"] or {}).get("layers", [])}
        old_units: dict[str, int] = state["index"]["units"]

        units = scan(kind)
        if not units:
            # Never overwrite a good cache with an empty one. If this fires,
            # bitbake is writing somewhere else (check DL_DIR/SSTATE_DIR in
            # .config.yaml are still `?=` so the workflow's env wins).
            warn(f"cache save: {kind.root} has no cacheable {kind.name} files; not pushing")
            return
        lru = {u: run_gen if unit_used(m) or u not in old_units else old_units[u]
               for u, m in units.items()}
        used = sum(1 for u in units if lru[u] == run_gen)

        evicted = 0
        sizes = {u: unit_size(m) for u, m in units.items()}
        total = sum(sizes.values())
        if prune and total > kind.budget:
            for u in sorted((u for u in units if lru[u] < run_gen), key=lambda u: (lru[u], -sizes[u])):
                if total <= kind.budget:
                    break
                for rel, _ in units[u]:
                    path = kind.root / rel
                    if path.is_dir() and not path.is_symlink():
                        shutil.rmtree(path, ignore_errors=True)
                    else:
                        path.unlink(missing_ok=True)
                unit_dir = kind.root / u
                if unit_dir.is_dir() and not unit_dir.is_symlink():
                    shutil.rmtree(unit_dir, ignore_errors=True)
                total -= sizes[u]
                del units[u], lru[u]
                evicted += 1

        groups: dict[str, list[Member]] = {}
        for u, members in units.items():
            if sizes[u] > MAX_LAYER:
                warn(f"cache save: {u} is {human(sizes[u])}, over GHCR's per-layer limit; not cached")
                continue
            groups.setdefault(layer_id(kind, u, sizes[u]), []).extend(members)

        stats = {"reused": 0, "unchanged": 0, "uploaded": 0}
        uploaded_bytes = 0
        layers: list[dict] = []
        TMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TMP, prefix=f"yocto-cache-{kind.name}-") as wd:
            workdir = Path(wd)
            with cf.ThreadPoolExecutor(JOBS) as ex:
                futures = [ex.submit(build_layer, kind, lid, members, prev_layers.get(f"{lid}.tar"), workdir)
                           for lid, members in groups.items()]
                for fut in cf.as_completed(futures):
                    desc, how = fut.result()
                    layers.append(desc)
                    stats[how] += 1
                    if how == "uploaded":
                        uploaded_bytes += desc["size"]
            layers.sort(key=lambda l: l["annotations"][TITLE])

            index_path = workdir / "index.json.gz"
            index_path.write_bytes(gzip.compress(
                json.dumps({"generation": run_gen, "units": lru}, sort_keys=True).encode(), mtime=0))
            desc, _ = push_file(index_path, MT_INDEX, "index.json.gz")
            layers.append(desc)

            db = kind.root / "hashserv.db"
            if kind.name == "sstate":
                if db.exists():
                    snap = workdir / "hashserv.db"
                    snapshot_hashserv(db, snap)
                    desc, _ = push_file(snap, MT_HASHSERV, "hashserv.db")
                    layers.append(desc)
                else:
                    warn(f"cache save: {db} missing; hash-equivalence data won't be cached")

            empty = workdir / "config.json"
            empty.write_bytes(b"{}")
            push_blob(empty, EMPTY_CONFIG["digest"])

            annotations = {
                "org.opencontainers.image.created":
                    datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "org.opencontainers.image.description": f"Yocto {kind.name} cache",
            }
            if os.environ.get("GITHUB_REPOSITORY"):
                # Links the GHCR package to the repo (and its Actions access).
                annotations["org.opencontainers.image.source"] = (
                    f"{os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}/{os.environ['GITHUB_REPOSITORY']}")
            manifest = {
                "schemaVersion": 2,
                "mediaType": "application/vnd.oci.image.manifest.v1+json",
                "artifactType": ARTIFACT_TYPE,
                "config": EMPTY_CONFIG,
                "layers": layers,
                "annotations": annotations,
            }
            mpath = workdir / "manifest.json"
            mpath.write_text(json.dumps(manifest, indent=1))
            tags = tags_for(kind, [scope(), "latest"])
            oras_check(["manifest", "push"], [f"{IMAGE}:{','.join(tags)}", str(mpath)])

        state.update(manifest=manifest, index={"generation": run_gen, "units": lru})
        store_state(kind, state)

    took = time.monotonic() - start
    files = sum(len(m) for m in units.values())
    line = (f"saved `{tags[0]}`: {len(units)} units / {files} files / {human(total)}; "
            f"{used} used this run, {evicted} evicted; layers: {stats['uploaded']} uploaded "
            f"({human(uploaded_bytes)}), {stats['unchanged']} unchanged, {stats['reused']} reused; {took:.0f}s")
    log(f"{kind.name}: {line}")
    if prune:  # final save only; checkpoints would flood the summary
        summary(f"- **{kind.name}**: {line}")


# --------------------------------------------------------------------------- main


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("action", choices=["restore", "save"])
    ap.add_argument("--prune", action="store_true",
                    help="evict least-recently-used units beyond the budget (final save only)")
    ap.add_argument("kinds", nargs="*", default=["sstate", "downloads"])
    args = ap.parse_args()
    if not IMAGE:
        ap.error("CACHE_IMAGE must be set")
    all_kinds = kinds()
    rc = 0
    for name in args.kinds:
        if name not in all_kinds:
            ap.error(f"unknown cache kind {name!r}")
        kind = all_kinds[name]
        print(f"::group::{args.action} {name}", flush=True)
        try:
            if args.action == "restore":
                restore(kind)
            else:
                save(kind, args.prune)
        except Exception as e:  # noqa: BLE001 - report and keep going with the next kind
            warn(f"cache {args.action} of {name} failed: {e}")
            rc = 1
        finally:
            print("::endgroup::", flush=True)
    return rc


if __name__ == "__main__":
    sys.exit(main())
