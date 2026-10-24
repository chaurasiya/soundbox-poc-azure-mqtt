"""Consume-only: act as a soundbox and print payments sent to it (does not publish).

  python scripts/consume.py [--device sb-device-001] [--ws]
Also subscribes to soundbox/<device>/history, where the retained last-3 list arrives on connect.
Use a test device ID: with the default limit of 1 session per authentication name, running this
as a real device's name will disconnect (or be refused by) the board. Ctrl+C to stop.
"""
import argparse, time
from mqtt_common import default_host, make_client, connect

p = argparse.ArgumentParser()
p.add_argument("--device", default="sb-device-001")
p.add_argument("--client-id", default=None)
p.add_argument("--host", default=default_host())
p.add_argument("--ws", action="store_true")
a = p.parse_args()

topic = f"soundbox/{a.device}/payments"
c = make_client(a.device, client_id=a.client_id or f"{a.device}-local", ws=a.ws)
history_topic = f"soundbox/{a.device}/history"
c.on_connect = lambda c, u, f, rc, props: (print("connect:", rc),
                                           rc.is_failure or c.subscribe([(topic, 1), (history_topic, 1)]))
c.on_subscribe = lambda c, u, mid, rcs, props: print("subscribed:", topic, history_topic, rcs)
c.on_message = lambda c, u, m: print(time.strftime("%H:%M:%S"), m.topic,
                                     "(retained)" if m.retain else "", m.payload.decode())
connect(c, a.host, a.ws)
try:
    c.loop_forever()
except KeyboardInterrupt:
    c.disconnect()
