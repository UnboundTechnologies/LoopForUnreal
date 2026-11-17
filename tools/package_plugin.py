#!/usr/bin/env python3
"""Package with an installed Unreal engine on the current Mac or Windows host."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path,
                        help="Unreal installation directory (containing Engine)")
    parser.add_argument("--build-root", type=Path, default=None,
                        help="Staging directory to build in before the result is moved "
                             "into dist/. Windows needs a short path because "
                             "UnrealBuildTool rejects action paths over 260 characters; "
                             "defaults to ~/.ulbuild there.")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    engine = args.engine.resolve()
    target = {"Darwin": "Mac", "Windows": "Win64"}.get(platform.system())
    if not target:
        parser.error("Run on macOS or Windows; cross-compilation is not supported.")
    version = json.loads((engine / "Engine/Build/Build.version").read_text())
    engine_version = ".".join(str(version[k]) for k in
                              ("MajorVersion", "MinorVersion", "PatchVersion"))
    descriptor = root / "Plugins/UbstreamModeler/UbstreamModeler.uplugin"
    plugin_version = json.loads(descriptor.read_text())["VersionName"]
    label = f"UnboundLoop-{plugin_version}-UE{engine_version}-{target}"
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
    output = root / "dist" / f"{label}-{stamp}"
    build_root = args.build_root
    if build_root is None and target == "Win64":
        build_root = Path.home() / ".ulbuild"
    work = (build_root.resolve() / stamp) if build_root else output
    work.mkdir(parents=True, exist_ok=False)
    package = work / "UbstreamModeler"
    uat = engine / "Engine/Build/BatchFiles" / (
        "RunUAT.bat" if target == "Win64" else "RunUAT.sh")
    command = [str(uat), "BuildPlugin", f"-Plugin={descriptor}",
               f"-Package={package}", f"-TargetPlatforms={target}"]
    if target == "Mac":
        command.append("-Architecture_Mac=arm64+x64")
    print(f"Building {label}; log: {work / 'build.log'}", flush=True)
    with (work / "build.log").open("w") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                env=os.environ.copy())
    if result.returncode:
        raise SystemExit(f"Build failed ({result.returncode}); see {work / 'build.log'}")
    installation = root / "docs/PLUGIN_INSTALLATION.txt"
    if installation.is_file():
        shutil.copy2(installation, package / "INSTALLATION.txt")
    else:
        print(f"Warning: {installation} is missing; packaging without INSTALLATION.txt")
    (package / "BUILD_INFO.json").write_text(json.dumps({
        "engine": version, "plugin_version": plugin_version, "platform": target,
        "architectures": ["arm64", "x86_64"] if target == "Mac" else ["x86_64"],
        "validation": "BuildPlugin succeeded; runtime validation is separate.",
    }, indent=2) + "\n")
    archive = Path(shutil.make_archive(str(work / label), "zip", work, "UbstreamModeler"))
    hasher = hashlib.sha256()
    with archive.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            hasher.update(chunk)
    digest = hasher.hexdigest()
    archive.with_suffix(".zip.sha256").write_text(f"{digest}  {archive.name}\n")
    if work != output:
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(work), str(output))
        archive = output / archive.name
    print(f"Created {archive}")


if __name__ == "__main__":
    main()
