#!/usr/bin/env python3
"""Check generated API without touching the checkout or depending on DEFLATE bytes."""

import base64
from pathlib import Path
import re
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[1]


def canonical(source):
    pattern = r"var swaggerSpec = \[\]string\{(.*?)\n\}"
    match = re.search(pattern, source, re.S)
    if not match:
        raise ValueError("Generated API has no embedded specification")
    encoded = "".join(re.findall(r'"([^"\n]+)"', match[1]))
    raw = zlib.decompress(base64.b64decode(encoded), -15)
    return source[:match.start()] + "<embedded-spec>" + source[match.end():], raw


def main():
    api = ROOT / "user/api"
    with tempfile.TemporaryDirectory(prefix="mapple-api-") as directory:
        output = Path(directory) / "server.gen.go"
        config = Path(directory) / "codegen.yaml"
        text = (api / "codegen.yaml").read_text()
        config.write_text(re.sub(r"(?m)^output:.*$", f"output: {output}", text))
        subprocess.run(["go", "tool", "oapi-codegen", "-config", str(config), "openapi.yaml"],
                       cwd=api, check=True)
        if canonical(output.read_text()) != canonical((api / "server.gen.go").read_text()):
            raise SystemExit("Generated API is stale. Run make api and review user/api/server.gen.go.")
    print("Generated API matches the contract (checkout unchanged).")


if __name__ == "__main__":
    main()
