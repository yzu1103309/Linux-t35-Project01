#!/usr/bin/env bash
# Install the newest t35 Linux kernel release in the Ubuntu course VM.
# Requires: python3, sudo, apt-get, dpkg-deb; no third-party Python packages.
set -euo pipefail
exec python3 - "$@" <<'PY'
import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from pathlib import Path

REPO = "yzu1103309/Linux-t35-Project01"
PREFIX = "5.15.135-t35-v"
TAG_RE = re.compile(r"^5\.15\.135-t35-v([0-9]+)$")
HEADERS = {"Accept": "application/vnd.github+json", "User-Agent": "t35-kernel-installer/1.0"}


def die(message):
    raise RuntimeError(message)


def request(url):
    return urllib.request.Request(url, headers=HEADERS)


def get_json(url):
    with urllib.request.urlopen(request(url), timeout=30) as response:
        return json.load(response)


def download(url, dest):
    print(f"  Downloading {dest.name} ...", flush=True)
    with urllib.request.urlopen(request(url), timeout=90) as response, dest.open("wb") as out:
        shutil.copyfileobj(response, out, length=1024 * 1024)


def command(*args, cwd=None, capture=False):
    return subprocess.run(args, cwd=cwd, check=True, text=True,
                          capture_output=capture)


def one_asset(assets, prefix, tag):
    matching = [a for a in assets
                if a["name"].startswith(prefix + tag + "_")
                and a["name"].endswith("_amd64.deb")]
    if len(matching) != 1:
        die(f"Expected exactly one {prefix}{tag}_*_amd64.deb; found {len(matching)}")
    return matching[0]


def validate_deb(path, package, tag):
    name = command("dpkg-deb", "-f", str(path), "Package", capture=True).stdout.strip()
    version = command("dpkg-deb", "-f", str(path), "Version", capture=True).stdout.strip()
    arch = command("dpkg-deb", "-f", str(path), "Architecture", capture=True).stdout.strip()
    if name != package or arch != "amd64" or not version.startswith(tag + "-"):
        die(f"Unexpected .deb metadata: {name} {version} {arch} ({path.name})")
    print(f"  Valid package: {name} {version} ({arch})")


def main():
    parser = argparse.ArgumentParser(description="Install latest 5.15.135-t35-vN kernel (including pre-releases).")
    parser.add_argument("--tag", help="Install an explicit release tag, e.g. 5.15.135-t35-v1")
    parser.add_argument("-y", "--yes", action="store_true", help="Skip the final installation prompt")
    args = parser.parse_args()

    if os.geteuid() == 0:
        die("Run this script as a regular user with sudo privileges, not as root.")
    if platform.machine() != "x86_64":
        die(f"Expected x86_64 Ubuntu VM, got {platform.machine()}")
    if not all(shutil.which(c) for c in ("sudo", "apt-get", "dpkg-deb", "update-grub")):
        die("Requires Ubuntu/Debian tools: sudo, apt-get, dpkg-deb, update-grub")
    try:
        virt = subprocess.run(["systemd-detect-virt", "--vm"], text=True,
                              capture_output=True, check=False)
        if virt.returncode != 0:
            die("VM not detected. Refusing to modify a possibly physical Host kernel.")
        print(f"Detected VM: {virt.stdout.strip()}")
    except FileNotFoundError:
        die("systemd-detect-virt not installed; cannot verify VM environment")

    print(f"GitHub repository: {REPO}")
    if args.tag:
        if not TAG_RE.fullmatch(args.tag):
            die("Invalid --tag format; expected 5.15.135-t35-vN")
        release = get_json(f"https://api.github.com/repos/{REPO}/releases/tags/{args.tag}")
    else:
        # /releases/latest excludes pre-releases, so use /releases instead.
        releases = get_json(f"https://api.github.com/repos/{REPO}/releases?per_page=100")
        matching = [r for r in releases if not r.get("draft") and TAG_RE.fullmatch(r.get("tag_name", ""))]
        if not matching:
            die("No matching 5.15.135-t35-vN releases found (including pre-releases)")
        # Highest project version (vN), not GitHub's stable-only latest endpoint.
        release = max(matching, key=lambda r: int(TAG_RE.fullmatch(r["tag_name"]).group(1)))

    if release.get("draft"):
        die("Refusing to install draft release")
    tag = release["tag_name"]
    assets = release.get("assets", [])
    image = one_asset(assets, "linux-image-", tag)
    headers = one_asset(assets, "linux-headers-", tag)
    by_name = {a["name"]: a for a in assets}
    if "SHA256SUMS" not in by_name or "KERNEL_RELEASE" not in by_name:
        die("Release is missing SHA256SUMS or KERNEL_RELEASE")

    current = platform.release()
    print(f"\nRunning kernel: {current}")
    print(f"Target release: {tag}" + (" (pre-release)" if release.get("prerelease") else ""))
    print(f"Release URL: https://github.com/{REPO}/releases/tag/{tag}")
    print(f"Packages to install:\n  {image['name']}\n  {headers['name']}")
    print("linux-libc-dev will NOT be installed.")
    if current == tag:
        print("\nThe selected kernel is already running; no installation needed.")
        return

    with tempfile.TemporaryDirectory(prefix="t35-kernel-") as directory:
        work = Path(directory)
        chosen = [image, headers, by_name["SHA256SUMS"], by_name["KERNEL_RELEASE"]]
        for item in chosen:
            name = item["name"]
            if name != Path(name).name:
                die(f"Unsafe release asset filename: {name!r}")
            download(item["browser_download_url"], work / name)

        reported = (work / "KERNEL_RELEASE").read_text(encoding="utf-8").strip()
        if reported != tag:
            die(f"KERNEL_RELEASE mismatch: expected {tag}, got {reported!r}")

        manifest = {}
        for line in (work / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if len(parts) == 2 and re.fullmatch(r"[0-9a-fA-F]{64}", parts[0]):
                manifest[parts[1].lstrip("*")] = parts[0].lower()
        print("\nChecking SHA256SUMS:")
        for item in (image, headers, by_name["KERNEL_RELEASE"]):
            name = item["name"]
            expected = manifest.get(name)
            if not expected:
                die(f"SHA256SUMS has no entry for {name}")
            sha = hashlib.sha256()
            with (work / name).open("rb") as f:
                for chunk in iter(lambda: f.read(1024 * 1024), b""):
                    sha.update(chunk)
            if sha.hexdigest() != expected:
                die(f"SHA256 mismatch: {name}")
            print(f"  OK  {name}")

        validate_deb(work / image["name"], "linux-image-" + tag, tag)
        validate_deb(work / headers["name"], "linux-headers-" + tag, tag)

        if not Path("/boot/grub/grub.cfg").exists():
            die("GRUB config /boot/grub/grub.cfg missing; cannot safely set up boot menu")
        print("\nWill install the selected kernel; old kernel packages will not be removed.")
        print("VM disk snapshot recommended BEFORE proceeding. Reboot will NOT be automatic.")
        if not args.yes:
            answer = input("Type YES to install: ")
            if answer != "YES":
                print("Cancelled.")
                return

        command("sudo", "-v")
        command("sudo", "apt-get", "install", "-y", "--no-install-recommends",
                "./" + image["name"], "./" + headers["name"], cwd=work)
        command("sudo", "update-grub")

    kernel_image = Path("/boot") / ("vmlinuz-" + tag)
    initrd = Path("/boot") / ("initrd.img-" + tag)
    if not kernel_image.is_file():
        die(f"Install finished but kernel image is missing: {kernel_image}")
    if not initrd.is_file():
        print(f"WARNING: initramfs not found at {initrd} -- verify before rebooting!", file=sys.stderr)
    print(f"\nInstalled successfully: {tag}")
    print("Existing kernels retained. Reboot MANUALLY with Cockpit/noVNC ready:")
    print("  sudo reboot")
    print("After reboot, check:")
    print("  uname -r")
    print(f"Expected: {tag}")
    print("If boot fails, use GRUB > Advanced options for Ubuntu to select the previous kernel.")


try:
    main()
except (RuntimeError, urllib.error.URLError, subprocess.CalledProcessError, OSError, ValueError, EOFError) as error:
    print(f"ERROR: {error}", file=sys.stderr)
    sys.exit(1)
PY
