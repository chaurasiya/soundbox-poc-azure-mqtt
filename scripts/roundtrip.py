"""Backend -> Event Grid MQTT -> device round trip.

Usage: python scripts/roundtrip.py <namespace-hostname> [--ws]
  --ws  use MQTT over WebSockets on 443 (for networks that block 8883)
"""
import json, ssl, sys, threading, time, uuid
from pathlib import Path
import paho.mqtt.client as mqtt

CERTS = Path(__file__).resolve().parent.parent / "certs"
host = sys.argv[1]
ws = "--ws" in sys.argv
DEVICE, BACKEND = "sb-device-001", "sb-backend"
TOPIC = f"soundbox/{DEVICE}/payments"

def make_client(name):
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=name,
                    transport="websockets" if ws else "tcp")
    c.username_pw_set(username=name)  # Event Grid: username must equal the client's authenticationName
    c.tls_set(certfile=str(CERTS / f"{name}.pem"), keyfile=str(CERTS / f"{name}.key"),
              cert_reqs=ssl.CERT_REQUIRED)  # server cert verified against the system CA store
    if ws:
        c.ws_set_options(path="/mqtt")
    return c

got = threading.Event()
subscribed = threading.Event()

device = make_client(DEVICE)
device.on_connect = lambda c, u, f, rc, p: (print("device connect:", rc), c.subscribe(TOPIC, qos=1))
device.on_subscribe = lambda c, u, mid, rcs, p: (print("device subscribe:", rcs), subscribed.set())
def on_msg(c, u, m):
    print("device received:", m.topic, m.payload.decode())
    got.set()
device.on_message = on_msg
device.connect(host, 443 if ws else 8883)
device.loop_start()
assert subscribed.wait(15), "device failed to subscribe"

backend = make_client(BACKEND)
backend.on_connect = lambda c, u, f, rc, p: print("backend connect:", rc)
backend.connect(host, 443 if ws else 8883)
backend.loop_start()
time.sleep(2)
payload = json.dumps({"txnId": uuid.uuid4().hex[:12], "amount": 150.00, "currency": "INR",
                      "status": "SUCCESS", "ts": int(time.time())})
info = backend.publish(TOPIC, payload, qos=1)
info.wait_for_publish(10)
print("backend published:", payload)
print("ROUNDTRIP OK" if got.wait(10) else "ROUNDTRIP FAILED")
backend.loop_stop(); device.loop_stop()
