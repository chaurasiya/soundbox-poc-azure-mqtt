# Soundbox PoC – Azure Event Grid MQTT + ESP32 consumer

A backend publishes UPI payment events over MQTT; the soundbox subscribes, speaks the amount from SD card clips, and lets the customer replay recent payments with a button.

> **Status:** deployed in the personal subscription (*Azure for Students*) as `rupeeco-rg/rupeeco-pawan-mqtt` (Central India), host `rupeeco-pawan-mqtt.centralindia-1.ts.eventgrid.azure.net`, including the history topics. Publish → subscribe and retained history were verified from a laptop over WebSockets (443).
> Not yet verified: the ESP32-S3 sketch (never compiled or run on hardware), the A7670C mutual-TLS/MQTT sequence, and the device ack path.
> The registered certificate thumbprints on this namespace are **not** those of the files in `certs/`; use throwaway test identities (below) rather than `sb-device-001`/`sb-backend`.

## How it works

```
backend (publish.py)  ──TLS 8883/443──►  Azure Event Grid MQTT broker  ──TLS 8883──►  soundbox (ESP32)
   payments  ─► soundbox/<id>/payments       (client-certificate auth)                  speaks amount
   history   ─► soundbox/<id>/history (retained, last 3)                                keeps last 3
   ◄─ acks   ◄─ soundbox/<id>/status                                                    publishes acks
```

| Topic | Publisher | Subscriber | Purpose |
|---|---|---|---|
| `soundbox/<id>/payments` | backend | that device | live payment, announced immediately |
| `soundbox/<id>/history` | backend, **retained** | that device | last 3 transactions as `{"txns":[…]}`, newest first; silent |
| `soundbox/<id>/status` | that device | backend | `{"txnId":…,"event":"played"}` acks |

A device can only touch topics containing its own `authenticationName`. Permissions are in `infra/main.json`.

## Configuring the soundbox consumer (ESP32-S3 + A7670C 4G)
Firmware: [sketch/soundbox_consumer/soundbox_consumer.ino](sketch/soundbox_consumer/soundbox_consumer.ino), adapted from `ref_soundbox_consumer_code.ino`. The earlier Wi-Fi/PubSubClient variant is kept in `sketch/soundbox_consumer_wifi/` for bench testing without a SIM.

### 1. Hardware (pins are set at the top of the sketch)
| Module | Pins |
|---|---|
| SH1106 OLED (I2C) | SDA 8, SCL 9 |
| microSD (SPI/FSPI) | CS 10, MOSI 11, SCK 12, MISO 13 |
| MAX98357A (I2S) | BCLK 15, LRC 16, DIN 17 |
| A7670C (UART1) | ESP RX 18 ← modem TXD, ESP TX 7 → modem RXD (via 10k+10k divider), RST 6 |
| Touch buttons (active HIGH) | UP 1, OK 2, DOWN 42 |

### 2. Arduino setup
- Board: ESP32S3 Dev Module, **Arduino-ESP32 core 3.x** (the sketch uses `ESP_I2S.h`).
- Libraries: **U8g2** and **ArduinoJson v7** (Library Manager). `SD`, `Preferences`, `SPI`, `Wire` ship with the core.

### 3. Build the generic firmware (once, same for every unit)
```bash
scripts/build-firmware.sh        # needs arduino-cli; writes firmware/soundbox_consumer.merged.bin
```
No per-device secrets are compiled in. The device id is the **modem IMEI**, read at runtime with `AT+CGSN`; it is the MQTT client ID and username and the middle of every topic (`soundbox/<imei>/…`). Per-device settings (broker host and port, root CA, client cert + key, expected IMEI) live in the board's **NVS partition** (namespace `prov`) and are written by the onboarding script below. If the modem's IMEI differs from the provisioned one (certificate flashed onto the wrong unit) the firmware refuses to connect and the OLED shows `IMEI ERR!`; an unprovisioned board shows `NOT PROV.`.

### 3b. Onboard a unit (per device)
```bash
pip install -r requirements.txt          # esptool, esp-idf-nvs-partition-gen, paho-mqtt
az login                                 # subscription that holds the Event Grid namespace
python3 scripts/onboard_device.py --imei 356938035643809 \
        --host rupeeco-pawan-mqtt.centralindia-1.ts.eventgrid.azure.net --port 8883
```
It validates the IMEI (15 digits + Luhn), finds the namespace from the host, checks the topic spaces exist, creates a per-device RSA-2048 key and self-signed certificate (CN = IMEI, 10 years), registers Event Grid client `sb-<imei>` (authentication name = IMEI, `type=soundbox`, SHA-1 thumbprint) and **verifies the broker stored it**, builds the NVS provisioning image, and, if `firmware/soundbox_consumer.merged.bin` exists, produces one flashable file. Everything lands in `devices/<imei>/` (`key.pem`, `cert.pem`, `manifest.json`, `nvs.bin`, `<imei>-flash.bin`) and `devices/registry.csv`.

Flash the printed command, or let the tool do it:
```bash
python3 scripts/onboard_device.py --imei <imei> --host <host> --flash /dev/cu.usbmodemXXXX
# full image (firmware + provisioning):     esptool --chip esp32s3 -p <PORT> write_flash 0x0 devices/<imei>/<imei>-flash.bin
# provisioning only (board already flashed): esptool --chip esp32s3 -p <PORT> write_flash 0x9000 devices/<imei>/nvs.bin
```
Other options: `--dry-run` (validate and look up, change nothing), `--rotate` (new certificate for an onboarded IMEI; the old files are archived and the broker entry replaced), `--delete` (offboard: removes the client from Event Grid and archives the files), `--key-type ec`, `--days N`, `--nvs-offset/--nvs-size` (must match the partition scheme; default 0x9000/0x5000), `--firmware PATH`. Re-running for an onboarded IMEI is safe: it reuses the existing certificate and rebuilds the artifacts. Keep `devices/` private: it holds the private keys.

### 4. SD card layout
Folders `/HIND` and `/ENG`, each containing 16-bit PCM `.wav` files (mono or stereo, 8–48 kHz; extra RIFF chunks are fine). Names used by the firmware:

`num0`…`num99`, `hazaar`, `lakh`, `crore`, `sau`, `ding`, `prefix`, `rupaye`, `prapt_thx`, `replay`, `no_txn`, `mqtt_con`, `net_start`, `net_con`, `net_dcon`, `dev_rdy`, `sim_err`, `boot_tone`, `power_on`, `power_off`, `off_tone`, `Beep`, `menu_open`, `menu_exit`, `menu_vol`, `lang_menu`, `lang_hi`, `lang_en`, `first_setup`, `vol_up`, `vol_down`, `max_vol`, `min_vol`

Missing or non-PCM files are logged to the serial monitor and skipped.

### 5. Flash and verify
Serial Monitor at 115200. A healthy boot shows `Provisioning: OK`, `Modem IMEI: …`, `History (n)`, the modem `>> AT:` / `<<` exchange, `Connected to Azure Event Grid MQTT`, and `History synced (…)`. On the OLED the home screen shows `AZURE: OK`. Serial commands: `M` menu, `ID` show IMEI / provisioning state, `AZURE` reconnect, `H` print history, `L1`/`L3` replay, any number simulates a payment, `RESET` wipes settings and history.

### Behaviour that was hardened versus the reference sketch
- One reader for all modem bytes; MQTT pushes (`+CMQTTRX*`) are parsed **by length** and queued, so a payment arriving during an AT command or while audio is playing is announced, not lost (UART RX buffer is 4 KB, drained during playback).
- Payments are parsed with ArduinoJson (`amount` or `amt`, `txnId` or `txn`, `status`); non-`SUCCESS`, non-positive or > 99 crore amounts are rejected.
- Duplicate protection by transaction ID (QoS 1 redelivery); history sync never suppresses a live announcement.
- Waits for the modem's `>` prompt before sending cert/topic/payload data and checks `+CMQTTCONNECT/SUB/PUB` result codes.
- Automatic reconnect with back-off (15 s → 2 min) and automatic modem re-initialisation; signal strength refreshes every 30 s and the clock every 10 min. Background retries are silent.
- Certificates are uploaded to the modem only when they change (or after repeated connect failures), not on every connect.
- The WAV parser walks RIFF chunks instead of assuming a 44-byte header. Replays no longer add to "today total".

## Transaction history and repeat gestures
| Gesture (home screen) | Result |
|---|---|
| OK long press (≥ 800 ms) | speaks the last transaction (shown on the OLED) |
| OK double press | speaks the last 3 transactions, newest first, with a list on the OLED |
| OK single press | system menu (also has "3. Replay Last Txn" and "4. Last 3 Txns") |
| UP / DOWN | volume |
| No history yet | plays `no_txn` |

On power-up the device does **not** announce anything it missed while off; it only syncs its list silently.

### How offline payments reach the device
The modem uses a clean MQTT session, so a live message published while it is off is lost. To cover that, after every successful payment the backend also publishes the *whole* last-3 list as one **retained** message on `soundbox/<id>/history`. On connect the device subscribes to it, receives it immediately, and **merges** it with its own flash copy (union by transaction ID, newest `ts` first, keep 3). The merged list is stored in flash (`Preferences`), so replay also works after a reboot with no signal. A live payment is also added locally even if the history update never arrives.

Notes:
- Retained messages work on Event Grid (verified from a laptop). Limits: 64 KB per message and a quota per throughput unit.
- Backend responsibility: always publish the payment first, then the history list (`scripts/publish.py` shows the format, including `ts`). `publish.py` keeps its list in `.history/<device>.json` as a stand-in for your real transaction store.
- Only `SUCCESS` payments belong in the history.

## Testing locally (no hardware needed)
Needs `az login` on the namespace's subscription and a venv with `paho-mqtt`. `--ws` uses WebSockets on 443, for networks that block 8883.
The namespace allows **1 session per authentication name**, so test with a throwaway device ID, never the real board's.

```bash
# one-time setup
python3 -m venv .venv && .venv/bin/pip install paho-mqtt

# variables
ID=/subscriptions/1d1837f7-1a22-43ac-9fff-0849379a5b9f/resourceGroups/rupeeco-rg/providers/Microsoft.EventGrid/namespaces/rupeeco-pawan-mqtt
H=rupeeco-pawan-mqtt.centralindia-1.ts.eventgrid.azure.net
DEV=sb-t$(LC_ALL=C tr -dc a-f0-9 </dev/urandom | head -c6)     # unique device ID
BE=sb-test-backend

# 1. make certs and register both identities with the broker
scripts/gen-certs.sh $DEV $BE
put(){ az rest --method put --url "https://management.azure.com$ID/clients/$1?api-version=2025-02-15" \
  --body "{\"properties\":{\"authenticationName\":\"$1\",\"state\":\"Enabled\",\"attributes\":{\"type\":\"$2\"},\"clientCertificateAuthentication\":{\"validationScheme\":\"ThumbprintMatch\",\"allowedThumbprints\":[\"$(cat certs/$1.thumbprint)\"]}}}" \
  --query properties.provisioningState -o tsv; }
put $DEV soundbox; put $BE backend; sleep 5

# 2. simple pub/sub: terminal 1 = soundbox, terminal 2 = backend (repeat step 2's second command for more payments)
.venv/bin/python -u scripts/consume.py --device $DEV --host $H --ws
.venv/bin/python scripts/publish.py --amount 321 --device $DEV --backend $BE --host $H --ws --wait
#    (--wait expects an ack from a real device, so "no ack within 10 s" is normal here)

# 3. "device was off" test: publish first, then start the consumer; the last 3 arrive as "(retained)"
for a in 100 200 350; do .venv/bin/python scripts/publish.py --amount $a --device $DEV --backend $BE --host $H --ws; done
.venv/bin/python -u scripts/consume.py --device $DEV --host $H --ws

# 4. cleanup
for c in $DEV $BE; do az rest --method delete --url "https://management.azure.com$ID/clients/$c?api-version=2025-02-15"; done
rm -rf .history
```
Expected in step 3: `subscribed: … Granted QoS 1, Granted QoS 1` then a `…/history (retained)` line with 3 transactions, newest first. If the second SUBACK is `Unspecified error`, the history permissions are missing; redeploy (below).

`scripts/roundtrip.py <host> [--ws]` is a one-shot self-test that connects as `sb-device-001`, so it only works with that device's registered cert.

**Session limit:** `maximumClientSessionsPerAuthenticationName` is 1, so a second connection with the same authentication name (even with a different client ID) takes over or is refused. Never run `consume.py` as the real device while the board is online.

## Device connection settings (for non-Arduino clients)
- Host `<namespace>.centralus-1.ts.eventgrid.azure.net`, port **8883** (TLS 1.2+, mutual TLS), or 443 with MQTT-over-WebSockets, path `/mqtt`.
- Username = the client's authentication name (e.g. `sb-device-001`), no password. Client ID: anything unique.
- Client cert/key: `certs/<id>.pem` / `.key` (EC P-256, self-signed, valid 365 days). Server trust: **DigiCert Global Root G3**.
- MQTT 3.1.1 or 5; QoS 1 for payments.

## Files
- `infra/main.json` – ARM template: namespace, client groups, topic spaces, permission bindings (incl. history).
- `scripts/onboard_device.py` – IMEI onboarding / rotation / offboarding (see 3b). `tests/test_onboard.py` covers IMEI and host validation.
- `scripts/build-firmware.sh` – builds the generic firmware with arduino-cli.
- `scripts/publish.py`, `consume.py`, `roundtrip.py`, `mqtt_common.py`, `gen-certs.sh` – local test tools (they also find certs of onboarded devices under `devices/<imei>/`).
- `sketch/soundbox_consumer/soundbox_consumer.ino` – ESP32-S3 + A7670C firmware.
- `sketch/soundbox_consumer_wifi/` – earlier Wi-Fi variant for bench testing (has its own `secrets.h`).
- `ref_soundbox_consumer_code.ino` – the reference sketch the firmware was adapted from (its embedded certs are not used).

## Deploying / updating the broker
Region is restricted by policy on the corporate subscription (US regions only; Central US worked); the personal one runs in Central India.
```bash
# new namespace
az group create -n <rg> -l <region>
az deployment group create -g <rg> --template-file infra/main.json \
  --parameters namespaceName=<ns> location=<region> \
               deviceThumbprint=<sha1 of device cert> backendThumbprint=<sha1 of backend cert>

# add/update child resources on an EXISTING namespace without re-PUTting it
# (pass the thumbprints already registered, or this overwrites them)
az deployment group what-if -g rupeeco-rg --template-file infra/main.json \
  --parameters namespaceName=rupeeco-pawan-mqtt location=centralindia createNamespace=false \
               deviceThumbprint=<registered> backendThumbprint=<registered>
```
Swap `what-if` for `create` to apply. The broker hostname for the local test scripts lives in `.host`; onboarding takes the host as an argument. Devices onboarded against an old host must be re-onboarded (`--rotate`) for a new one.

## Adding another device
Just onboard its IMEI (step 3b). No template edit or redeploy is needed: the client carries `type=soundbox`, so it joins the `soundboxDevices` group and gets its own topics (`soundbox/<imei>/…`) automatically.

## Teardown
```bash
az group delete -n rg-soundbox-poc --yes
```
