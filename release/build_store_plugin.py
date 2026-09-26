#!/usr/bin/env python3
"""Embed the reviewed installer into the UFI-TOOLS single-file JS format."""
import hashlib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "u60pro-ui-1.0.0"
URL = "https://github.com/huangtengsz-ui/u60pro-screen-ui/releases/download/v1.0.0"

def sha(name: str) -> str:
    return hashlib.sha256((OUT / name).read_bytes()).hexdigest()

def main() -> None:
    installer = (HERE / "install.sh").read_text().replace("@@BASE_URL@@", URL)
    installer = installer.replace("@@BIN_SHA@@", sha("u60pro-devui-aarch64"))
    installer = installer.replace("@@UI_SHA@@", sha("ui.tar.gz"))
    restore = (HERE / "restore.sh").read_text()
    template = (HERE / "store-plugin.template.js").read_text()
    result = template.replace("@@RELEASE_URL_JSON@@", json.dumps(URL))
    result = result.replace("@@INSTALL_SCRIPT_JSON@@", json.dumps(installer, ensure_ascii=False))
    result = result.replace("@@RESTORE_SCRIPT_JSON@@", json.dumps(restore, ensure_ascii=False))
    assert "@@" not in result
    old_target = OUT / "U60Pro-ThreePage-Screen-UI.js"
    old_target.unlink(missing_ok=True)
    target = OUT / "U60Pro三页屏幕UI-基于33333s及scoltzero改版.js"
    target.write_text(result)
    assert target.stat().st_size < 1145 * 1024
    sums = OUT / "SHA256SUMS"
    lines = [line for line in sums.read_text().splitlines() if not line.endswith(".js")]
    lines.append(f"{sha(target.name)}  {target.name}")
    sums.write_text("\n".join(lines) + "\n")
    print(f"Store plugin: {target} ({target.stat().st_size} bytes)")

if __name__ == "__main__":
    main()
