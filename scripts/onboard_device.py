#!/usr/bin/env python3
"""Onboard (or offboard) a soundbox identified by its 4G modem IMEI.

  python3 scripts/onboard_device.py --imei 356938035643809 \
      --host rupeeco-pawan-mqtt.centralindia-1.ts.eventgrid.azure.net --port 8883

What it does (idempotent; safe to re-run):
  1. validates the IMEI (15 digits, Luhn) and the Event Grid host/port
  2. finds the namespace in the current `az` subscription and checks the topic spaces exist
  3. generates a per-device key + self-signed client certificate (CN = IMEI) and its SHA-1 thumbprint
  4. registers the device as Event Grid client  sb-<imei>  (authenticationName = IMEI, attributes.type = soundbox)
     and verifies the broker stored the right thumbprint
  5. builds the per-device provisioning image (NVS partition: host, port, IMEI, root CA, cert, key)
  6. if a firmware image exists, combines firmware + provisioning into one flashable file
  7. records everything in devices/<imei>/manifest.json and devices/registry.csv

  --rotate   issue a new certificate for an already onboarded IMEI (old one is archived and replaced on the broker)
  --delete   offboard: remove the client from Event Grid and archive the local files
  --dry-run  validate and look things up (read-only), change nothing
  --flash PORT  write the result to the board with esptool afterwards

Requires: az CLI (logged in), openssl, and `pip install esptool esp-idf-nvs-partition-gen`.
Topics a device ends up with:  soundbox/<imei>/payments | history | status
"""
import argparse, csv, datetime, hashlib, json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
API = "2025-02-15"
ROOT_CA_SHA256 = "31AD6648F8104138C738F39EA4320133393E3A18CC02296EF97C2AC9EF6731D0"   # DigiCert Global Root G3
HOST_RE = re.compile(r"^([a-z0-9][a-z0-9-]{1,48}[a-z0-9])\.([a-z0-9]+)-1\.ts\.eventgrid\.azure\.net$")
REQUIRED_TOPIC_SPACES = ["ownPayments", "ownStatus", "ownHistory"]


# ---------------------------------------------------------------- helpers
def die(msg, code=1):
    print(f"\nERROR: {msg}", file=sys.stderr)
    raise SystemExit(code)


def info(msg):
    print(msg, flush=True)


def luhn_ok(digits: str) -> bool:
    total = 0
    for i, ch in enumerate(reversed(digits)):
        d = int(ch)
        if i % 2 == 1:
            d *= 2
            if d > 9:
                d -= 9
        total += d
    return total % 10 == 0


def valid_imei(imei: str) -> bool:
    return bool(re.fullmatch(r"\d{15}", imei)) and luhn_ok(imei)


def parse_host(host: str):
    m = HOST_RE.match(host.strip().lower())
    return (m.group(1), m.group(2)) if m else None


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def az_rest(method, path, body=None):
    """ARM call via `az rest`. Returns parsed JSON ({} if empty), or None on 404."""
    sep = "&" if "?" in path else "?"
    cmd = ["az", "rest", "--method", method, "--url", f"https://management.azure.com{path}{sep}api-version={API}", "-o", "json"]
    if body is not None:
        cmd += ["--body", json.dumps(body)]
    p = run(cmd)
    if p.returncode:
        if re.search(r"NotFound|could not be found|not found", p.stderr, re.I):
            return None
        die(f"az rest {method} {path} failed:\n{p.stderr.strip()}")
    return json.loads(p.stdout) if p.stdout.strip() else {}


def sha1_thumbprint(cert_pem: Path) -> str:
    p = run(["openssl", "x509", "-in", str(cert_pem), "-noout", "-fingerprint", "-sha1"])
    if p.returncode:
        die(f"openssl could not read {cert_pem}: {p.stderr.strip()}")
    return p.stdout.strip().split("=", 1)[1].replace(":", "").upper()


def python_with(module: str):
    """Interpreter that can import `module`: this one, or the repo's .venv."""
    for py in (sys.executable, str(ROOT / ".venv" / "bin" / "python")):
        if py and Path(py).exists() and run([py, "-c", f"import {module}"]).returncode == 0:
            return py
    return None


def to_crlf(src: Path, dst: Path):
    dst.write_bytes(b"".join(line.rstrip(b"\r\n") + b"\r\n" for line in src.read_bytes().splitlines() if line.strip()))


# ---------------------------------------------------------------- steps
def check_azure():
    if run(["az", "account", "show"]).returncode:
        die("az is not logged in. Run `az login` (and `az account set -s <subscription>`).")


def locate_namespace(ns_name, host):
    p = run(["az", "resource", "list", "--resource-type", "Microsoft.EventGrid/namespaces", "--name", ns_name, "-o", "json"])
    if p.returncode:
        die(f"could not list namespaces:\n{p.stderr.strip()}")
    for res in json.loads(p.stdout or "[]"):
        ns = az_rest("GET", res["id"])
        cfg = (ns or {}).get("properties", {}).get("topicSpacesConfiguration", {})
        if cfg.get("state") == "Enabled" and cfg.get("hostname", "").lower() == host.lower():
            return res["id"]
    acct = run(["az", "account", "show", "--query", "name", "-o", "tsv"]).stdout.strip()
    die(f"no MQTT-enabled Event Grid namespace with host {host} in subscription '{acct}'.")


def check_infra(ns_id):
    group = az_rest("GET", f"{ns_id}/clientGroups/soundboxDevices")
    if group is None:
        die("client group 'soundboxDevices' is missing: deploy infra/main.json first.")
    missing = [t for t in REQUIRED_TOPIC_SPACES if az_rest("GET", f"{ns_id}/topicSpaces/{t}") is None]
    if missing:
        die(f"topic spaces missing: {', '.join(missing)}. Deploy infra/main.json (see README) before onboarding.")


def generate_identity(imei, key_type, days, dest: Path):
    dest.mkdir(parents=True, exist_ok=True)
    key, cert = dest / "key.pem", dest / "cert.pem"
    if key_type == "ec":
        cmd = ["openssl", "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(key)]
        if run(cmd).returncode:
            die("openssl could not generate the EC key")
    else:
        p = run(["openssl", "genrsa", "-traditional", "-out", str(key), "2048"])
        if p.returncode:                                      # LibreSSL has no -traditional (and already emits PKCS#1)
            p = run(["openssl", "genrsa", "-out", str(key), "2048"])
        if p.returncode:
            die(f"openssl could not generate the RSA key: {p.stderr.strip()}")
    os.chmod(key, 0o600)
    p = run(["openssl", "req", "-new", "-x509", "-sha256", "-key", str(key), "-out", str(cert),
             "-days", str(days), "-subj", f"/CN={imei}"])
    if p.returncode:
        die(f"openssl could not create the certificate: {p.stderr.strip()}")
    return sha1_thumbprint(cert)


def client_body(imei, thumbprint):
    return {"properties": {
        "authenticationName": imei,
        "description": f"Soundbox, modem IMEI {imei}",
        "state": "Enabled",
        "attributes": {"type": "soundbox", "imei": imei},
        "clientCertificateAuthentication": {"validationScheme": "ThumbprintMatch", "allowedThumbprints": [thumbprint]},
    }}


def register_client(ns_id, name, imei, thumbprint):
    az_rest("PUT", f"{ns_id}/clients/{name}", client_body(imei, thumbprint))
    for _ in range(15):
        got = az_rest("GET", f"{ns_id}/clients/{name}")
        props = (got or {}).get("properties", {})
        thumbs = [t.upper() for t in props.get("clientCertificateAuthentication", {}).get("allowedThumbprints", [])]
        if props.get("provisioningState") == "Succeeded" and thumbs == [thumbprint]:
            return
        time.sleep(2)
    die(f"client {name} did not reach the expected state on the broker (thumbprint {thumbprint}).")


def build_nvs(dev_dir: Path, imei, host, port, root_ca: Path, nvs_size: int):
    prov = dev_dir / "prov"
    prov.mkdir(exist_ok=True)
    to_crlf(root_ca, prov / "ca.pem")                     # CRLF: that is what the modem's cert upload is fed
    to_crlf(dev_dir / "cert.pem", prov / "cert.pem")
    to_crlf(dev_dir / "key.pem", prov / "key.pem")
    os.chmod(prov / "key.pem", 0o600)
    csv_path = dev_dir / "nvs.csv"
    csv_path.write_text("\n".join([
        "key,type,encoding,value",
        "prov,namespace,,",
        f"host,data,string,{host}",
        f"port,data,u16,{port}",
        f"imei,data,string,{imei}",
        f"ca,file,string,{prov / 'ca.pem'}",
        f"cert,file,string,{prov / 'cert.pem'}",
        f"key,file,string,{prov / 'key.pem'}",
    ]) + "\n")
    py = python_with("esp_idf_nvs_partition_gen")
    if not py:
        die("esp-idf-nvs-partition-gen is not installed. Run: pip install esp-idf-nvs-partition-gen esptool")
    out = dev_dir / "nvs.bin"
    p = run([py, "-m", "esp_idf_nvs_partition_gen", "generate", str(csv_path), str(out), hex(nvs_size)])
    if p.returncode or not out.exists() or out.stat().st_size != nvs_size:
        die(f"NVS image generation failed:\n{p.stdout}{p.stderr}")
    return out


def merge_image(firmware: Path, nvs_bin: Path, nvs_offset: int, out: Path):
    """Arduino's *.merged.bin spans 0x0..end of app, so the NVS region sits inside it as 0xFF padding
    (esptool merge-bin would reject that overlap). Copy the image and write the provisioning data there,
    refusing if the region is not blank (wrong --nvs-offset for this firmware's partition table)."""
    img = bytearray(firmware.read_bytes())
    nvs = nvs_bin.read_bytes()
    end = nvs_offset + len(nvs)
    if len(img) < end:
        img.extend(b"\xff" * (end - len(img)))
    if any(b != 0xFF for b in img[nvs_offset:end]):
        die(f"firmware image is not blank at {hex(nvs_offset)}..{hex(end)}: --nvs-offset/--nvs-size do not match its partition table.")
    img[nvs_offset:end] = nvs
    out.write_bytes(img)


def flash(port, image: Path, offset: int):
    py = python_with("esptool")
    if not py:
        die("esptool is not installed")
    info(f"Flashing {image.name} @ {hex(offset)} on {port} ...")
    p = subprocess.run([py, "-m", "esptool", "--chip", "esp32s3", "-p", port, "write_flash", hex(offset), str(image)])
    if p.returncode:
        die("flashing failed")


def append_registry(devices_dir: Path, row):
    reg = devices_dir / "registry.csv"
    new = not reg.exists()
    with reg.open("a", newline="") as f:
        w = csv.writer(f)
        if new:
            w.writerow(["timestamp", "imei", "client", "thumbprint", "host", "port", "status"])
        w.writerow(row)
    os.chmod(reg, 0o600)


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--imei", required=True, help="15-digit modem IMEI (AT+CGSN)")
    ap.add_argument("--host", required=True, help="Event Grid MQTT host, e.g. <ns>.<region>-1.ts.eventgrid.azure.net")
    ap.add_argument("--port", type=int, default=8883, help="MQTT TLS port (default 8883)")
    ap.add_argument("--key-type", choices=["rsa", "ec"], default="rsa", help="client key type (default rsa-2048: most compatible with modem TLS stacks)")
    ap.add_argument("--days", type=int, default=3650, help="client certificate validity in days (default 10 years)")
    ap.add_argument("--rotate", action="store_true", help="issue a new certificate for an already onboarded IMEI")
    ap.add_argument("--delete", action="store_true", help="offboard: delete the client and archive local files")
    ap.add_argument("--dry-run", action="store_true", help="validate and look up only; change nothing")
    ap.add_argument("--out-dir", default=str(ROOT / "devices"))
    ap.add_argument("--root-ca", default=str(ROOT / "certs" / "DigiCertGlobalRootG3.pem"))
    ap.add_argument("--firmware", default=str(ROOT / "firmware" / "soundbox_consumer.merged.bin"),
                    help="generic firmware image (merged.bin from scripts/build-firmware.sh)")
    ap.add_argument("--nvs-offset", type=lambda x: int(x, 0), default=0x9000, help="NVS partition offset (default 0x9000)")
    ap.add_argument("--nvs-size", type=lambda x: int(x, 0), default=0x5000, help="NVS partition size (default 0x5000)")
    ap.add_argument("--flash", metavar="PORT", help="flash the result to this serial port with esptool")
    a = ap.parse_args()

    imei = a.imei.strip()
    if not valid_imei(imei):
        die(f"'{imei}' is not a valid IMEI (need 15 digits with a correct Luhn check digit).")
    parsed = parse_host(a.host)
    if not parsed:
        die(f"'{a.host}' does not look like an Event Grid MQTT host (<namespace>.<region>-1.ts.eventgrid.azure.net).")
    ns_name, region = parsed
    host = a.host.strip().lower()
    if not 1 <= a.port <= 65535:
        die("invalid port")
    if a.port != 8883:
        info(f"WARNING: port {a.port}. Event Grid serves MQTT/TLS on 8883; the modem cannot use the 443 WebSocket endpoint.")
    if a.nvs_size % 4096 or a.nvs_size < 0x3000:
        die("--nvs-size must be a multiple of 4096 and at least 0x3000")

    client = f"sb-{imei}"
    devices = Path(a.out_dir)
    dev_dir = devices / imei

    check_azure()
    ns_id = locate_namespace(ns_name, host)
    info(f"Namespace: {ns_id}")
    remote = az_rest("GET", f"{ns_id}/clients/{client}")

    # ---------------- offboarding
    if a.delete:
        if remote is None:
            die(f"client {client} does not exist on the broker.")
        if a.dry_run:
            info(f"[dry-run] would delete client {client} and archive {dev_dir}")
            return
        az_rest("DELETE", f"{ns_id}/clients/{client}")
        if az_rest("GET", f"{ns_id}/clients/{client}") is not None:
            time.sleep(5)
        if dev_dir.exists():
            dest = devices / "_revoked" / f"{imei}-{int(time.time())}"
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(dev_dir), str(dest))
            info(f"Local files archived to {dest}")
        append_registry(devices, [now_iso(), imei, client, "", host, a.port, "revoked"])
        info(f"Client {client} deleted. The unit can no longer connect.")
        return

    if not a.dry_run:
        check_infra(ns_id)
    local_cert = dev_dir / "cert.pem"
    local_thumb = sha1_thumbprint(local_cert) if local_cert.exists() and (dev_dir / "key.pem").exists() else None
    remote_thumb = None
    if remote:
        t = remote.get("properties", {}).get("clientCertificateAuthentication", {}).get("allowedThumbprints", [])
        remote_thumb = t[0].upper() if t else None

    if remote and not a.rotate:
        if local_thumb and local_thumb == remote_thumb:
            action = "reuse"                                   # fully onboarded already: just rebuild the artifacts
        elif local_thumb:
            die(f"{client} is registered with a different certificate than devices/{imei}/. Use --rotate to replace it.")
        else:
            die(f"{client} is already registered but its private key is not in {dev_dir}. Use --rotate to issue a new certificate.")
    elif a.rotate:
        if not remote and not local_thumb:
            die(f"--rotate needs an existing onboarded device; {client} is unknown. Drop --rotate to onboard it.")
        action = "rotate"
    else:
        action = "reuse-unregistered" if local_thumb else "new"

    info(f"IMEI {imei} -> client {client}, topics soundbox/{imei}/{{payments,history,status}}")
    info(f"Plan: {action}; key={a.key_type}, cert valid {a.days} days; broker {host}:{a.port}")
    if a.dry_run:
        info("[dry-run] nothing changed.")
        return

    root_ca = Path(a.root_ca)
    if not root_ca.exists():
        die(f"root CA not found: {root_ca}")
    ca_fp = run(["openssl", "x509", "-in", str(root_ca), "-noout", "-fingerprint", "-sha256"]).stdout.strip().split("=", 1)[-1].replace(":", "").upper()
    if ca_fp != ROOT_CA_SHA256:
        die(f"{root_ca} is not DigiCert Global Root G3 (SHA-256 {ca_fp}).")

    devices.mkdir(parents=True, exist_ok=True)
    os.chmod(devices, 0o700)

    # ---------------- identity
    if action in ("new", "rotate"):
        tmp = Path(tempfile.mkdtemp(prefix=f".{imei}-", dir=devices))
        try:
            thumb = generate_identity(imei, a.key_type, a.days, tmp)
            register_client(ns_id, client, imei, thumb)          # broker first: if this fails nothing local changes
            if dev_dir.exists():
                arch = devices / "_archive" / f"{imei}-{int(time.time())}"
                arch.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(dev_dir), str(arch))
                info(f"Previous files archived to {arch}")
            shutil.move(str(tmp), str(dev_dir))
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    else:
        thumb = local_thumb
        if action == "reuse-unregistered":
            register_client(ns_id, client, imei, thumb)
    os.chmod(dev_dir, 0o700)
    info(f"Registered {client}; thumbprint {thumb}")

    # ---------------- provisioning + flashable image
    nvs_bin = build_nvs(dev_dir, imei, host, a.port, root_ca, a.nvs_size)
    info(f"Provisioning image: {nvs_bin}")
    firmware = Path(a.firmware)
    flash_image, flash_offset = nvs_bin, a.nvs_offset
    if firmware.exists():
        out = dev_dir / f"{imei}-flash.bin"
        merge_image(firmware, nvs_bin, a.nvs_offset, out)
        flash_image, flash_offset = out, 0
        info(f"Flashable image (firmware + provisioning): {out}")
    else:
        info(f"No firmware at {firmware} (run scripts/build-firmware.sh). Only the provisioning image was produced.")

    manifest = {
        "imei": imei, "client": client, "host": host, "port": a.port, "region": region,
        "thumbprint_sha1": thumb, "key_type": a.key_type, "cert_days": a.days, "action": action,
        "created": now_iso(), "topics": {k: f"soundbox/{imei}/{k}" for k in ("payments", "history", "status")},
        "nvs_offset": hex(a.nvs_offset), "nvs_size": hex(a.nvs_size),
        "flash_image": flash_image.name, "flash_offset": hex(flash_offset),
    }
    (dev_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    append_registry(devices, [now_iso(), imei, client, thumb, host, a.port, action])

    cmd = f"esptool --chip esp32s3 -p <PORT> write_flash {hex(flash_offset)} {flash_image}"
    info("\nDone. Flash with:\n  " + cmd)
    if flash_offset != 0:
        info("(provisioning image only: the board must already run the generic firmware)")
    if a.flash:
        flash(a.flash, flash_image, flash_offset)
    info("\nKeep devices/ private: it holds the device private keys.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        die("interrupted", 130)
