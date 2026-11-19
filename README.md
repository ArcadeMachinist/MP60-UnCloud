# MP60-UnCloud

Unofficial firmware for the discontinued **Datto MP60** power strip. 
It replaces the Open Mesh cloud-dependent services with a local daemon, web interface, HTTP API, MQTT client, and Home Assistant discovery.

This is not affiliated with Datto or Open Mesh. 
Flashing third-party firmware can damage the device; keep original backups and proceed at your own risk.

## Features

- Six individually controlled relays with hardware-reported state readback.
- Voltage, frequency, current, active/apparent/reactive power, and power-factor telemetry.
- Local HTTP UI and JSON API; optional UI password.
- Tasmota-style HTTP and MQTT commands and telemetry.
- Optional retained Home Assistant MQTT discovery.
- Wi-Fi client with SSID scanning; Ethernet has route priority when connected.
- Reset button, status LEDs, and external watchdog support.
- No Open Mesh cloud or dashboard services.

## Ready-made image

`dist/mp60-openwrt-25.12.6-r7.bin` is the native OpenWrt 25.12.6 CE recovery image. 
It is built **without an SSH authorized key**. 
Dropbear password login is disabled. 
Use the local web UI or UART unless you build with your own public key.

See [Flashing and first boot](docs/FLASHING.md) before using the image. The checksum is in `dist/SHA256SUMS.txt`; static container checks are in `dist/verification.json`.

## Build

A Linux environment or Ubuntu under WSL is required. Install Git, Python 3, the normal C build tools, and OpenWrt's build prerequisites. On Debian/Ubuntu, a typical starting point is:

```sh
sudo apt update
sudo apt install -y build-essential clang curl file flex gawk gettext git \
  libncurses-dev libssl-dev python3 python3-setuptools rsync swig unzip \
  wget zlib1g-dev
```

Then run:

```sh
./scripts/build.sh
```

The script clones the pinned OpenWrt 25.12.6 source into `.openwrt`, applies the MP60 target patch and device tree, installs the local `mp60d` package, builds the firmware, and verifies the resulting CE image. The first build downloads and compiles the complete toolchain and can take substantial time.

Set `JOBS` to change parallelism or `OPENWRT_DIR` to use an existing clean checkout of the pinned revision:

```sh
JOBS=8 ./scripts/build.sh
OPENWRT_DIR=/path/to/openwrt ./scripts/build.sh
```

## Add your SSH public key before building

Only a **public** key is needed. Never copy a private key into this project. The destination is gitignored so it will not accidentally be committed.

For an existing OpenSSH public key such as `~/.ssh/id_ed25519.pub`:

```sh
mkdir -p package/utils/mp60d/files/etc/dropbear
cp ~/.ssh/id_ed25519.pub \
  package/utils/mp60d/files/etc/dropbear/authorized_keys
chmod 600 package/utils/mp60d/files/etc/dropbear/authorized_keys
./scripts/build.sh
```

Or create a dedicated key first:

```sh
ssh-keygen -t ed25519 -f ~/.ssh/mp60
cp ~/.ssh/mp60.pub \
  package/utils/mp60d/files/etc/dropbear/authorized_keys
./scripts/build.sh
```

The file must contain one or more public-key lines, not the private key. After flashing, connect with the matching private key:

```sh
ssh -i ~/.ssh/mp60 root@<MP60-IP>
```

To return to a no-key build, remove `package/utils/mp60d/files/etc/dropbear/authorized_keys` and rebuild.

## Host-side tests

The daemon has a simulator that does not access relay hardware:

```sh
cd package/utils/mp60d/src
bash tests/run_host_tests.sh
```

The suite covers HTTP/API authentication and configuration, outlet commands, Wi-Fi configuration simulation, MQTT, Home Assistant discovery, and reconnect behavior.

## Source layout

- `package/utils/mp60d/src/` — C11 daemon, UI, build rules, and host tests.
- `package/utils/mp60d/files/` — OpenWrt defaults and release metadata.
- `openwrt/overlay/` — MP60 device tree and pristine default account database.
- `openwrt/patches/` — OpenWrt image, networking, upgrade, and CE packaging support.
- `openwrt/config.seed` — minimal ath79/MP60 build configuration.
- `scripts/build.sh` — reproducible pinned-source firmware build.
- `scripts/verify_image.py` — static CE, checksum, uImage, and flash-layout validation.
- `dist/` — ready-made no-key firmware and verification results.

## License

See `LICENSES/`.

## Warranty

Provided as-is, without warranty. Flashing may render a device inoperable.
Power strip is mains voltage device. I take no responsibility for any damage.
You are accepting all the risks associated with using this project and modified MP60 device.

## Project

https://github.com/ArcadeMachinist/MP60-UnCloud
