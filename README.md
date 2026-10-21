# Soundbox PoC – Azure Event Grid MQTT broker

Broker: `soundbox-poc-qbtg5s.centralus-1.ts.eventgrid.azure.net` (RG `rg-soundbox-poc`, subscription *DLP - Non-Prod*, Central US – the only region class the subscription's policy allows).

## Topics
| Topic | Who publishes | Who subscribes |
|---|---|---|
| `soundbox/<deviceId>/payments` | backend (`sb-backend`) | that device only |
| `soundbox/<deviceId>/status` | that device only (acks, heartbeat) | backend |

A device can only touch topics containing its own `authenticationName` (`${client.authenticationName}` in the topic space).

## Device connection settings
- Host: above, port **8883** (TLS 1.2+, mutual TLS). Port 443 with MQTT-over-WebSockets (path `/mqtt`) also works.
- Username: `sb-device-001` (must equal the client's authentication name). Client ID: anything unique.
- Client cert/key: `certs/sb-device-001.pem` / `.key` (EC P-256, self-signed, valid 365 days).
- Server trust: **DigiCert Global Root G3** (broker chain is `DigiCert Global G3 TLS ECC SHA384 2020 CA1`).
- MQTT 3.1.1 or 5, QoS 1 recommended for payment messages.

## Files
- `infra/main.json` – ARM template: namespace, client groups, clients, topic spaces, permission bindings.
- `scripts/gen-certs.sh <name>...` – makes a self-signed cert and prints its SHA-1 thumbprint.
- `scripts/roundtrip.py <host> [--ws]` – backend publishes, device receives. Needs `.venv` with `paho-mqtt`.

## Adding another device
```bash
scripts/gen-certs.sh sb-device-002
```
Then add a client to `infra/main.json` with that thumbprint and `"attributes": {"type": "soundbox"}` and redeploy (it joins the `soundboxDevices` group automatically).

## Teardown (stops all charges)
```bash
az group delete -n rg-soundbox-poc --yes
```
