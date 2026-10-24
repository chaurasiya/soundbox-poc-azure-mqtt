import ssl
from pathlib import Path
import paho.mqtt.client as mqtt

ROOT = Path(__file__).resolve().parent.parent
CERTS = ROOT / "certs"


def default_host():
    return (ROOT / ".host").read_text().strip()


def make_client(auth_name, client_id=None, ws=False):
    """auth_name = Event Grid client authenticationName (also selects the cert/key files)."""
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id or auth_name,
                    transport="websockets" if ws else "tcp")
    c.username_pw_set(username=auth_name)
    certfile, keyfile = CERTS / f"{auth_name}.pem", CERTS / f"{auth_name}.key"
    if not certfile.exists():                       # devices onboarded with scripts/onboard_device.py
        certfile, keyfile = ROOT / "devices" / auth_name / "cert.pem", ROOT / "devices" / auth_name / "key.pem"
    c.tls_set(certfile=str(certfile), keyfile=str(keyfile), cert_reqs=ssl.CERT_REQUIRED)
    if ws:
        c.ws_set_options(path="/mqtt")
    return c


def connect(c, host, ws):
    c.connect(host, 443 if ws else 8883)
