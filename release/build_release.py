#!/usr/bin/env python3
"""Package the tested U60 Pro screen candidate without changing the candidate."""

from __future__ import annotations

import gzip
import hashlib
import io
import json
import shutil
import tarfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "candidate"
OUTPUT = ROOT / "release" / "u60pro-ui-1.0.0"
UI = SOURCE / "ui"
BIN = SOURCE / "runtime-bin" / "u60pro-devui"
FILES = sorted(p for p in UI.rglob("*") if p.is_file() and p.suffix in {".html", ".css"})
MANIFEST = "\n".join(p.relative_to(UI).as_posix() for p in FILES) + "\n"
VERSION = {
    "schema": 1,
    "devui": {
        "version": "1.3.0-huang.1",
        "asset": "u60pro-devui-aarch64",
        "notes": "三页主界面、手势快捷面板、图标与时间显示修正",
    },
    "ui": {
        "version": "0.5.0-huang.1",
        "asset": "ui.tar.gz",
        "notes": "适配 320×480 屏幕的分级信息与设置中心",
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def add_bytes(archive: tarfile.TarFile, name: str, data: bytes) -> None:
    entry = tarfile.TarInfo(name)
    entry.size = len(data)
    entry.mode = 0o644
    entry.mtime = 0
    entry.uid = entry.gid = 0
    entry.uname = entry.gname = ""
    archive.addfile(entry, io.BytesIO(data))


def main() -> None:
    assert BIN.is_file()
    assert len(FILES) >= 20
    assert {"00-overview.html", "01-signal.html", "02-functions.html", "style.css"} <= set(MANIFEST.splitlines())
    assert (UI / ".devui-managed-files").read_text() == MANIFEST
    assert not any("fly" in p.name.lower() or "fm" in p.name.lower() for p in FILES)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    shutil.copy2(BIN, OUTPUT / "u60pro-devui-aarch64")
    (OUTPUT / "u60pro-devui-aarch64").chmod(0o755)

    with (OUTPUT / "ui.tar.gz").open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                add_bytes(archive, ".devui-managed-files", MANIFEST.encode())
                for path in FILES:
                    add_bytes(archive, path.relative_to(UI).as_posix(), path.read_bytes())

    with (OUTPUT / "source.tar.gz").open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                runtime = SOURCE / "runtime-src"
                for path in sorted(runtime.rglob("*")):
                    if path.is_file():
                        add_bytes(archive, "runtime-src/" + path.relative_to(runtime).as_posix(), path.read_bytes())
                add_bytes(archive, "ui/.devui-managed-files", MANIFEST.encode())
                for path in FILES:
                    add_bytes(archive, "ui/" + path.relative_to(UI).as_posix(), path.read_bytes())
                add_bytes(archive, "BUILDING.md", (ROOT / "release" / "BUILDING.md").read_bytes())
                add_bytes(archive, "LICENSE", (runtime / "LICENSE").read_bytes())
                add_bytes(archive, "NOTICE.md", (ROOT / "release" / "NOTICE.md").read_bytes())
                for path in sorted((ROOT / "release" / "licenses").iterdir()):
                    if path.is_file():
                        add_bytes(archive, "licenses/" + path.name, path.read_bytes())

    (OUTPUT / "version.json").write_text(json.dumps(VERSION, ensure_ascii=False, indent=2) + "\n")
    shutil.copy2(SOURCE / "runtime-src" / "LICENSE", OUTPUT / "LICENSE")
    licenses = OUTPUT / "licenses"
    if licenses.exists():
        shutil.rmtree(licenses)
    licenses.mkdir()
    for path in sorted((ROOT / "release" / "licenses").iterdir()):
        if path.is_file():
            shutil.copy2(path, licenses / path.name)
    with (OUTPUT / "licenses.tar.gz").open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as archive:
                for path in sorted(licenses.iterdir()):
                    if path.is_file():
                        add_bytes(archive, path.name, path.read_bytes())
    shutil.copy2(ROOT / "release" / "NOTICE.md", OUTPUT / "NOTICE.md")
    shutil.copy2(ROOT / "release" / "README.md", OUTPUT / "README.md")
    names = ["version.json", "u60pro-devui-aarch64", "ui.tar.gz", "source.tar.gz", "licenses.tar.gz", "LICENSE", "NOTICE.md", "README.md"]
    (OUTPUT / "SHA256SUMS").write_text("".join(f"{sha256(OUTPUT / name)}  {name}\n" for name in names))

    with tarfile.open(OUTPUT / "ui.tar.gz", mode="r:gz") as archive:
        archived = {item.name for item in archive.getmembers()}
        expected = set(MANIFEST.splitlines()) | {".devui-managed-files"}
        assert archived == expected, (sorted(archived - expected), sorted(expected - archived))
        assert archive.extractfile(".devui-managed-files").read().decode() == MANIFEST
    print(f"Packaged {len(FILES)} UI files in {OUTPUT}")
    for name in names:
        print(f"{name}: {sha256(OUTPUT / name)}")


if __name__ == "__main__":
    main()
