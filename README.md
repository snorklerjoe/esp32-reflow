# Reflow Oven Controller

Firmware for a NodeMCU ESP-32S v1.1 (ESP-WROOM-32) that turns a toaster oven into a reflow oven. The board starts an **open Wi-Fi access point** and serves a password-protected web app from it. The app covers manual heater control, PID temperature hold, reflow profile management, live-plotted reflow runs, PID autotune and admin settings.

## Hardware

| Signal | Default GPIO | Notes |
|---|---|---|
| SSR control | 25 | Drives the SSR input. Active-high by default; set active-low in Settings if needed. |
| MAX6675 SCK | 18 | Shared by both chips |
| MAX6675 SO | 19 | Shared by both chips |
| CS, ambient MAX6675 | 16 | |
| CS, aux MAX6675 | 17 | |

You can change every pin at runtime in **Settings** (admin only), and the choice is saved to flash.

### Which pins can I use?

The MAX6675 is a read-only SPI device with a 4.3 MHz maximum clock. The firmware reads it in software, so **SPI has no special pin requirements**: any general-purpose GPIO works, and the two chips share SCK and SO with their own CS lines. The real limits are general ESP32 ones, and the Settings page enforces them:

- **GPIO 6–11:** never usable, because they're wired to the module's internal flash.
- **GPIO 0, 2, 5, 12, 15:** strapping pins. They can stop the board from booting or change boot mode, and GPIO 12 pulled high selects 1.8 V flash. GPIO 2 is also the onboard LED.
- **GPIO 1, 3:** USB serial (programming and logs).
- **GPIO 34–39:** input only, with no internal pull-ups. They're fine for **SO** but can't drive SCK, CS or the SSR.

That leaves **4, 13, 14, 16–19, 21–23, 25–27, 32 and 33** for outputs. SO can also use 34, 35, 36 or 39.

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Create your secrets file:
   ```sh
   cp .env.example .env
   # edit WEB_PASSWORD, ADMIN_PASSWORD, and optionally AP_SSID
   ```
   These values are compiled into the firmware, so rebuild and reflash after you change them. Environment variables with the same names override `.env`.
3. Build and flash:
   ```sh
   pio run -t upload
   pio device monitor
   ```
   The web app in `web/` is gzipped and embedded into the firmware at build time by `scripts/embed_web.py`, so there's no filesystem image to upload. Profiles live in a LittleFS partition that's formatted automatically on first boot and seeded with SAC305 and Sn63/Pb37 profiles.
4. Join the open Wi-Fi network (**ReflowOven** by default). Most phones show a captive-portal sign-in sheet automatically. If yours doesn't, open <http://192.168.4.1/> or <http://reflow.local/>.

> **If the build fails with `No module named 'intelhex'`:** this is a PlatformIO tooling issue. Fix it with `~/.platformio/penv/bin/python -m pip install intelhex`.

## The web app

| Tab | What it does |
|---|---|
| **Home** | Ambient and aux temperatures, plus Heater ON / Heater OFF and a 10-minute trend. Heater OFF is a global stop. |
| **Hold Temp** | PID hold at a typed setpoint, using whichever thermocouple you pick for feedback. You can change the setpoint live. |
| **Profiles** | Upload CSV profiles, rename them, add descriptions, see them plotted, and delete them. |
| **Reflow** | Pick a profile and reference thermocouple (aux by default), then start. Live plot of profile vs. ambient/aux plus heater duty. Shows a big red **OPEN DOOR** when needed. |
| **Autotune** | Relay-method (Åström–Hägglund) PID autotune. Shows the resulting gains for three tuning rules with one-click Apply (admin). |
| **Settings** | Admin only: GPIO pins, SSR polarity, PID gains, SSR window, over-temperature limit, door margin. |
| **About** | Credits on a theatre marquee with chasing bulbs. |

### Profile CSV format

```csv
time_s,temp_c
0,25
90,150
180,180
210,217
240,245
270,217
360,80
```

- Each line is one point: time in seconds from the start, then the setpoint in °C. Times must strictly increase.
- A header line is optional, lines starting with `#` are ignored, and `,` `;` or tab can separate the values.
- 2–128 points per profile and up to 24 profiles.
- The setpoint is linearly interpolated between points. Before the first point it equals that point's temperature.

Sample files are in [profiles/](profiles/).

### When does it say "OPEN DOOR"?

During a reflow, the app tells you to open the door when **both** of these are true:
- The profile is in a falling segment (cooling).
- The reference thermocouple is more than *door margin* (default 5 °C) above the setpoint.

The heater is already off at that point, so the only way to follow the curve down is to let heat out. The warning clears once the oven gets back down to the setpoint.

## Control details

- **Sensors:** each MAX6675 is read every 250 ms (a conversion takes up to 220 ms). Open thermocouples and missing chips are detected. Closed-loop modes ride out up to 2 s of bad readings, then abort with the heater off.
- **Heater:** time-proportioning output. The SSR is on for *duty %* of each *SSR window* (1 s by default), which works well with zero-cross SSRs.
- **PID:** output is 0–100 % duty. It uses derivative on measurement with a 2 s low-pass filter, plus conditional-integration anti-windup. Units are Kp in %/°C, Ki in %/(°C·s) and Kd in %·s/°C.
- **Autotune:** the heater runs fully on below *target − hysteresis* and fully off above *target + hysteresis*. The first full cycle is discarded, then period T<sub>u</sub> and amplitude *a* are averaged over N cycles, giving K<sub>u</sub> = 4·d / (π·√(a² − h²)) with d = 50 %. Gains are offered for these rules:
  - **No overshoot** (0.2 K<sub>u</sub>), usually the best fit for an oven.
  - **Some overshoot** (0.33 K<sub>u</sub>).
  - **Classic Ziegler–Nichols** (0.6 K<sub>u</sub>).
- **Safety:**
  - If either thermocouple reads above *Max temp* (280 °C by default), the controller stops whatever is running and turns the heater off.
  - Reflow refuses to start a profile whose peak is within 5 °C of that limit.
  - **Manual "Heater ON" has no setpoint.** It stays on until you turn it off or the over-temperature limit trips, and that limit only works if a thermocouple is reading. Don't leave it unattended.

## Security notes

- The access point is open and the app uses plain HTTP, so anyone in Wi-Fi range can sniff the password when someone logs in. Treat the password as a guard against casual use, not real security.
- Passwords are compiled into the firmware binary, so anyone with physical access to the flash can read them.
- Sessions are in-memory tokens, sent as an HttpOnly cookie or a `Bearer` header. They expire after 12 h idle and are cleared on reboot. Five wrong passwords trigger a 30 s lockout.

## HTTP API

The web app uses this JSON API for everything, and you can call it from scripts. Authenticate with the cookie from `/api/login` or with `Authorization: Bearer <token>`.

```sh
TOKEN=$(curl -s -X POST http://192.168.4.1/api/login \
  -H 'Content-Type: application/json' -d '{"password":"changeme"}' | jq -r .token)
curl -s http://192.168.4.1/api/status -H "Authorization: Bearer $TOKEN"
```

| Method | Path | Role | Body / query | Description |
|---|---|---|---|---|
| POST | `/api/login` | – | `{password}` | Returns `{token, role}` and sets a cookie. The admin password grants `admin`. |
| POST | `/api/elevate` | user | `{password}` | Upgrades the current session to admin. |
| POST | `/api/logout` | – | | Ends the session. |
| GET | `/api/session` | – | | Returns `{role, version, ssid}`. |
| GET | `/api/status` | user | | Mode, temperatures, faults, heater state and duty, setpoint, PID terms, gains, current or last run, autotune state, latest event, history `seq`. |
| GET | `/api/history` | user | `?since=SEQ&max=N` | 1 Hz samples `[ambient, aux, setpoint, duty]` from `since` (≤900 per call, 1 h retained). Keep polling with `since=next`. |
| POST | `/api/heater` | user | `{on: bool}` | Manual heater. `false` stops everything. |
| POST | `/api/stop` | user | | Stops any mode, heater off. |
| POST | `/api/hold` | user | `{setpoint, sensor}` | Starts or updates a PID hold. `sensor` is `"aux"` or `"ambient"`. |
| GET | `/api/profiles` | user | | All profiles with points. |
| POST | `/api/profiles` | user | `{name, description, csv}` | Creates a profile from CSV text. |
| GET | `/api/profile` | user | `?id=N` | One profile. |
| PUT | `/api/profile` | user | `?id=N`, `{name?, description?, csv?}` | Renames, re-describes or replaces points. |
| DELETE | `/api/profile` | user | `?id=N` | Deletes a profile. |
| POST | `/api/reflow/start` | user | `{profileId, sensor}` | Starts a reflow. `sensor` defaults to `"aux"`. |
| POST | `/api/autotune/start` | user | `{target, hysteresis?, cycles?, sensor?}` | Starts the relay autotune. |
| GET | `/api/autotune` | user | | Autotune progress and result. |
| POST | `/api/autotune/apply` | admin | `{rule}` | Applies `"none"`, `"some"` or `"classic"` gains and saves them. |
| GET | `/api/settings` | admin | | Current settings, defaults and allowed pins. |
| PUT | `/api/settings` | admin | partial settings object | Validates, applies and saves. Pin changes stop any run and re-initialize the I/O. |

Errors come back as `{"error": "message"}` with a 4xx or 5xx status.

## Project layout

```
platformio.ini         board/framework/libs
.env.example           secrets template (copy to .env)
scripts/load_env.py    injects .env values as compile-time defines
scripts/embed_web.py   gzips web/ into src/generated/web_assets.h
src/main.cpp           Wi-Fi AP, captive DNS, mDNS, main loop
src/controller.*       modes, PID, SSR time-proportioning, reflow, autotune, safety, history
src/max6675.*          bit-banged MAX6675 driver
src/pid.h              PID controller
src/profiles.*         LittleFS profile storage and CSV parsing
src/settings.*         NVS-backed settings and pin validation
src/auth.*             sessions and password checks
src/web.*              HTTP routes / JSON API
web/                   single-page web app (HTML/CSS/JS, no external dependencies)
profiles/              sample profile CSVs
```
