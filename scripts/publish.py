"""Publish-only: act as the backend and send a payment to one soundbox.

  python scripts/publish.py --amount 250.50 [--device sb-device-001] [--ws] [--wait]
  --wait  also listen for the device's ack on soundbox/<device>/status for 10 s

Each SUCCESS payment is also added to a per-device list of the last 3 transactions, published
retained on soundbox/<device>/history so a device that was off can fetch it when it reconnects.
(.history/<device>.json is a stand-in for the transaction store your real backend would have.)
"""
import argparse, json, threading, time, uuid
from pathlib import Path
from mqtt_common import ROOT, default_host, make_client, connect

HISTORY_SIZE = 3

p = argparse.ArgumentParser()
p.add_argument("--amount", type=float, required=True)
p.add_argument("--device", default="sb-device-001")
p.add_argument("--status", default="SUCCESS")
p.add_argument("--backend", default="sb-backend", help="authentication name / cert of the publisher")
p.add_argument("--no-history", action="store_true", help="skip the retained history publish")
p.add_argument("--host", default=default_host())
p.add_argument("--ws", action="store_true", help="MQTT over WebSockets on 443 (if 8883 is blocked)")
p.add_argument("--wait", action="store_true")
a = p.parse_args()

c = make_client(a.backend, ws=a.ws)
connected, acked = threading.Event(), threading.Event()

def on_connect(c, u, f, rc, props):
    print("connect:", rc)
    if rc.is_failure:
        return
    if a.wait:
        c.subscribe(f"soundbox/{a.device}/status", qos=1)
    connected.set()

c.on_connect = on_connect
c.on_message = lambda c, u, m: (print("ack:", m.topic, m.payload.decode()), acked.set())
connect(c, a.host, a.ws)
c.loop_start()
if not connected.wait(15):
    raise SystemExit("could not connect")
time.sleep(1)  # let the SUBACK land before publishing when --wait is used

payload = json.dumps({"txnId": uuid.uuid4().hex[:12], "amount": a.amount, "currency": "INR",
                      "status": a.status, "ts": int(time.time())})
topic = f"soundbox/{a.device}/payments"
c.publish(topic, payload, qos=1).wait_for_publish(10)
print("published:", topic, payload)

if a.status == "SUCCESS" and not a.no_history:
    store = ROOT / ".history" / f"{a.device}.json"
    store.parent.mkdir(exist_ok=True)
    txns = json.loads(store.read_text()) if store.exists() else []
    txns = ([json.loads(payload)] + txns)[:HISTORY_SIZE]
    store.write_text(json.dumps(txns))
    hist = json.dumps({"txns": [{"txnId": t["txnId"], "amount": t["amount"], "ts": t["ts"]} for t in txns]})
    c.publish(f"soundbox/{a.device}/history", hist, qos=1, retain=True).wait_for_publish(10)
    print("history (retained):", hist)
if a.wait:
    print("device acked" if acked.wait(10) else "no ack within 10 s")
c.loop_stop(); c.disconnect()
