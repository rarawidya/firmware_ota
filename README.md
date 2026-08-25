# ESP32 → STM32L432KC OTA Flasher

An ESP32-C3 (Seeed XIAO) firmware that acts as a Wi-Fi gateway for flashing an
**STM32L432KC** target. The ESP32 hosts a small web UI, downloads an STM32
firmware image over HTTP, and writes it into the STM32 through the built-in ST
UART bootloader (AN3155) — no ST-Link required.

```
 [ Browser ] --Wi-Fi--> [ ESP32-C3 ] --USART1 + BOOT0/NRST--> [ STM32L432KC ]
                             |
                             +--HTTP--> firmware server (firmware_url)
```

## Target: STM32L432KC

| Property                | Value                                        |
| ----------------------- | -------------------------------------------- |
| Core                    | Cortex-M4F, 80 MHz                           |
| Flash                   | 256 KB @ `0x08000000` – `0x08040000`         |
| Flash page size         | 2 KB (single bank)                           |
| SRAM                    | 64 KB (48 KB SRAM1 + 16 KB SRAM2)            |
| System memory (ROM b/l) | `0x1FFF0000`                                 |
| Option bytes            | `0x1FFF7800`                                 |
| Bootloader device ID    | `0x435` (STM32L43xxx/44xxx)                  |
| BOOT0 pin (UFQFPN32)    | pin 31, `PH3-BOOT0`                          |

Firmware images must therefore be **≤ 256 KB** and linked at `0x08000000`.
The SPIFFS partition on the ESP32 is ~1.4 MB, so the staged image always fits.

The L4 system bootloader implements **Extended Erase (`0x44`)**, not the older
standard erase (`0x43`) — which is what `STM32Bootloader::eraseFull()` sends
(`0xFFFF` = global mass erase). `getID()` returns `0x435` on this part and is a
cheap wiring sanity check if a flash fails to start.

## Wiring

The bootloader UART pins are fixed in ROM and cannot be remapped. This project
uses **USART1** on the STM32 side:

| ESP32-C3 pin | Direction | STM32L432KC pin           |
| ------------ | --------- | ------------------------- |
| GPIO 6 (TX)  | →         | `PA10` — USART1 RX        |
| GPIO 7 (RX)  | ←         | `PA9`  — USART1 TX        |
| GPIO 4       | →         | `PH3-BOOT0` (pin 31)      |
| GPIO 5       | →         | `NRST`                    |
| GND          | —         | GND (common ground)       |

Both parts run at 3.3 V, so the UART connects directly — no level shifter.

The bootloader also listens on **USART2 (`PA2`/`PA3`)**. On a NUCLEO-L432KC
those two pins are wired to the ST-Link virtual COM port, so leave them alone
and use USART1 for the ESP32 to avoid contention. Consult AN2606 Table
"STM32L43xxx/44xxx configuration in system memory boot mode" for the full list
of bootloader peripherals.

### ESP32-side control pins

| Signal     | ESP32-C3 pin | Notes                          |
| ---------- | ------------ | ------------------------------ |
| OTA button | GPIO 3       | Button to GND (`INPUT_PULLUP`) |
| Status LED | GPIO 2       | LED                            |

### Entering the bootloader

`enter_bootloader_hw()` drives BOOT0 high, pulses NRST low for 50 ms, and waits
120 ms for the ROM bootloader to come up. `exit_bootloader_hw()` drops BOOT0 and
resets again to run the freshly written application.

For BOOT0 to be sampled from the pin, the `nSWBOOT0` option bit must be `1`
(the factory default). If it has been cleared, boot mode comes from the `nBOOT0`
option bit instead and toggling the pin does nothing — reprogram the option
bytes over SWD to recover.

Note also that the L4 bootloader performs an **empty-flash check**: if the first
word at `0x08000000` reads `0xFFFFFFFF`, the part enters the bootloader on its
own regardless of BOOT0. A blank or mass-erased chip is therefore always
recoverable.

UART runs at 115200 baud, **8E1** — the even parity the STM32 system bootloader
expects. Flashing writes from `0x08000000` in 256-byte chunks after a full chip
erase.

## Build & flash the ESP32

Requires [PlatformIO](https://platformio.org/).

```bash
pio run                      # build
pio run -t upload            # flash the ESP32 firmware
pio run -t uploadfs          # flash the SPIFFS image (data/ → web UI)
pio device monitor           # serial monitor @ 115200
```

`uploadfs` is mandatory: the web pages in `data/` are served straight from
SPIFFS, and the ESP32 returns 404 for every page if the filesystem is empty.

Board/env: `seeed_xiao_esp32c3`, Arduino framework, `ArduinoJson@^7.4.2`.

## Power / boot behaviour

At boot the device waits 5 seconds:

- **Button pressed during that window** → 3 short LED blinks, then deep sleep.
  It wakes again when the button is pulled low. This is the on/off toggle.
- **Otherwise** → normal start. The LED blinks at 1 Hz while in AP mode and
  goes solid once station credentials have been submitted.

A fast LED blink at startup means SPIFFS failed to mount.

## Usage

1. Power on. The ESP32 comes up as an access point named after `deviceName`
   (default **`ESP32-OTA`**, password **`esp32pass`**), also reachable over
   mDNS as `<device-name>.local`.
2. Join that AP and open `http://192.168.4.1/`. In AP mode this serves
   `index_ap.html` — a form to hand the device your Wi-Fi SSID and password.
3. Submitting the form switches the ESP32 to station mode (auto-reconnect on
   drop). Reconnect to your normal network and reopen the device by IP or
   mDNS name.
4. In station mode `/` serves `index_sta.html`: **Check firmware** queries the
   firmware server, **Update** downloads the image to SPIFFS and starts the
   flash. A progress bar polls `/flash_status` while a live log streams from
   `/log_events` (SSE).
5. `/setting.html` edits the device name and AP password (stored in NVS under
   the `esp_cfg` namespace).

The firmware index is fetched from the endpoint hard-coded in
`handleCheckFirmware()` (`src/main.cpp`), which must answer with JSON
containing a `firmware_url` field:

```json
{ "firmware_url": "https://example.com/firmware.bin", "version": "1.0.0" }
```

Change that URL in the source if you host your own firmware server.

## HTTP API

| Method | Path              | Purpose                                                |
| ------ | ----------------- | ------------------------------------------------------ |
| GET    | `/`, `/index.html`| Web UI (AP or STA page, depending on current Wi-Fi mode)|
| GET    | `/setting.html`   | Settings page                                          |
| POST   | `/update_sta`     | `{"staSsid":…,"staPass":…}` — switch to station mode    |
| GET    | `/check_firmware` | Proxy the firmware server, cache `firmware_url`         |
| POST   | `/flash`          | Download firmware and start flashing (409 if busy)      |
| GET    | `/flash_status`   | `{running, done, success, percent}`                     |
| GET    | `/log_events`     | Server-sent events stream of flash log lines            |
| GET    | `/settings`       | `{deviceName, apPass}`                                  |
| POST   | `/settings`       | Persist `deviceName` / `apPass`                         |

`/check_firmware` must be called before `/flash` — the flash handler uses the
`firmware_url` cached by that request.

## Python helper scripts

`settings.py` pushes a settings payload to `/settings`:

```bash
python3 ./settings.py -d STM32Flasher -m ap  -s ESP32-OTA -p esp32pass -H 192.168.4.1
python3 ./settings.py -d STM32Flasher -m sta -s MY_WIFI   -p MY_PASSWORD -H stm32flasher.local
```

`-H/--host` defaults to `192.168.4.1` (the AP address) and `--port` to `80`.

> **Note:** `flash.py` uploads a local `.bin` to `/upload_bin` and then calls
> `/flash`. The current firmware has no `/upload_bin` route — flashing is
> driven by the firmware server instead — so the script is stale until that
> endpoint is restored or the script is repointed.

## Repository layout

```
src/main.cpp                  ESP32 application: Wi-Fi, web server, flash task
src/stm32_uart_bootloader.*   AN3155 UART bootloader client (erase/write/go)
data/index_ap.html            Wi-Fi provisioning page (AP mode)
data/index_sta.html           OTA page: check / update / progress / log
data/setting.html             Device name + AP password
flash.py, settings.py         Host-side helper scripts
blink_1s.bin, blink_100ms.bin Sample L432KC images for testing the flash path
platformio.ini                Build configuration
main.cpp                      Stale top-level copy of an older variant (unused)
```

## References

- [AN2606 — STM32 microcontroller system memory boot mode](https://www.st.com/resource/en/application_note/cd00167594-stm32-microcontroller-system-memory-boot-mode-stmicroelectronics.pdf)
  (bootloader pins, versions and activation patterns per device)
- [AN3155 — USART protocol used in the STM32 bootloader](https://www.st.com/resource/en/application_note/an3155-usart-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf)
  (the command set implemented in `stm32_uart_bootloader.cpp`)
- [RM0394 — STM32L43xxx/44xxx/45xxx/46xxx reference manual](https://www.st.com/resource/en/reference_manual/rm0394-stm32l41xxx42xxx43xxx44xxx45xxx46xxx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)
