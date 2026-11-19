# Flashing and first boot

## Safety and backups

This firmware is only for Datto MP60. Firmware flashing does NOT require opening the powerstrip.

Use stable power and do not interrupt erase/write operations. 

## Supplied image

`dist/mp60-openwrt-25.12.6-r7.bin` is an Open Mesh CE/P60 dual-slot recovery image. 
The supplied image contains no SSH authorized key - so SSH effectively is disabled, until you build your own image with your key. 
Without rebuilding with a public key, use the HTTP UI for initial access.

Verify the download before flashing:

```sh
cd dist
sha256sum -c SHA256SUMS.txt
```

## Network recovery flash

Use the original `open-mesh-flash-ng.exe` on Windows with Npcap. Connect an isolated direct Ethernet link or isolated dumb switch. 
Alternatively use AP521 Flasher project https://github.com/ap51-flash/ap51-flash

List the flasher options and interfaces:

```bat
open-mesh-flash-ng.exe --help
```

Replace the placeholders with the MP60 MAC address and the interface reported by the flasher:

```bat
open-mesh-flash-ng.exe -m <MP60_MAC> <INTERFACE> mp60-openwrt-25.12.6-r7.bin
```

Start the flasher, then power-cycle the MP60 to enter network recovery. 

## First boot

Ethernet requests DHCP and takes default-route priority whenever connected. Wi-Fi can be configured from the web UI and uses DHCP with route metric 20. The local UI listens on HTTP port 80 and initially has no password.

If you embedded your public key during the build, SSH is available on port 22 with that key. Password login remains disabled.

After flashing, verify:

1. HTTP UI/API access through Ethernet and Wi-Fi.
2. Live, non-stale voltage and frequency telemetry.
3. Each outlet's physical relay and reported state; then All On and All Off.
4. MQTT and Home Assistant discovery if enabled.
5. Reset button, status LEDs, watchdog operation, and reboot persistence.

Rapid relay commands can take about one second each because controller writes are deliberately spaced. Current and power scaling should be checked with known safe loads before relying on those measurements.

