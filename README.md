# ets2-telemetry-udp
Native ETS2 / ATS telemetry plugin (Linux &amp; Windows) streaming real-time data over UDP for web dashboards and LAN tools.

# ETS2 / ATS Telemetry UDP Plugin

Native telemetry plugin for **Euro Truck Simulator 2** and **American Truck Simulator**  
that streams real-time game data over **UDP** for dashboards, LAN tools, and custom apps.

✔ Native C++ (no Python, no external runtimes)  
✔ Linux **and** Windows  
✔ Uses official **SCS Telemetry SDK**  
✔ Simple JSON over UDP  
✔ Designed for local & LAN usage  

---

## What this is

This project provides a **native telemetry plugin** for ETS2 / ATS that:

- Runs inside the game via the SCS SDK
- Collects truck telemetry (speed, RPM, gears, inputs, etc.)
- Streams data as **JSON packets over UDP**
- Can be consumed by:
  - Web dashboards
  - LAN tools
  - Custom desktop apps
  - Embedded displays

This avoids broken third-party telemetry servers and heavy dependencies.

---

## What data is sent

Current JSON output (example):

```json
{
  "speed": 0.540,
  "rpm": 550.5,
  "gear": 1,
  "dgear": 1,
  "steer": 0.000,
  "throttle": 0.000,
  "brake": 0.000,
  "clutch": 0.000,
  "cruise": 0.000
}
```
## Installation

### Linux (ETS2 / ATS)

```bash
git clone https://github.com/MrHokss/ets2-telemetry-udp.git
cd ets2-telemetry-udp

mkdir build
cd build
cmake ..
cmake --build . -j
```

```bash
mkdir -p ~/.steam/steam/steamapps/common/Euro\ Truck\ Simulator\ 2/bin/linux_x64/plugins
cp libets2_telemetry_udp.so \
~/.steam/steam/steamapps/common/Euro\ Truck\ Simulator\ 2/bin/linux_x64/plugins/
```

Start the game and confirm the SDK prompt.

---

### Windows (ETS2 / ATS)

```bash
git clone https://github.com/MrHokss/ets2-telemetry-udp.git
cd ets2-telemetry-udp
```

Build the DLL (MinGW):

```bash
x86_64-w64-mingw32-g++ ^
-shared ^
-o ets2_telemetry_udp.dll ^
src/telemetry.cpp ^
-Iinclude ^
-lws2_32
```

Copy the DLL to:

```
<SteamLibrary>\steamapps\common\Euro Truck Simulator 2\bin\win_x64\plugins\
```

Start the game and confirm the SDK prompt.

---

### Verify telemetry output

```bash
nc -klu 49001
```

Expected output:

```json
{"speed":0.540,"rpm":550.5,"gear":1,"dgear":1,"steer":0.000,"throttle":0.000,"brake":0.000,"clutch":0.000,"cruise":0.000}
```
