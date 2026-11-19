#!/usr/bin/env python3
"""Statically verify a native OpenWrt Open Mesh CE image for the Datto MP60."""

import argparse
import configparser
import hashlib
import json
import struct
import zlib
from pathlib import Path

HEADER_SIZE = 65536
SLOT_SIZE = 7 * 1024 * 1024
KERNEL_FLASH_ADDR = 0x1C0000
UIMAGE_MAGIC = 0x27051956


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(kind, data):
    return hashlib.new(kind, data).hexdigest()


def parse_ce(data):
    require(data[:2] == b"CE", "Missing CE magic")
    require(data[2:4] == b"01", "Unsupported CE version")
    image_type = data[4:36].decode("ascii").rstrip()
    count = int(data[36:38], 16)
    cursor = 38
    records = []
    payload_offset = HEADER_SIZE
    for _ in range(count):
        entry = data[cursor:cursor + 72]
        require(len(entry) == 72, "Truncated CE entry table")
        name = entry[:32].decode("ascii").rstrip()
        length = int(entry[32:40], 16)
        expected_md5 = entry[40:72].decode("ascii")
        payload = data[payload_offset:payload_offset + length]
        require(len(payload) == length, f"Truncated CE payload: {name}")
        actual_md5 = digest("md5", payload)
        require(actual_md5 == expected_md5, f"CE MD5 mismatch: {name}")
        records.append({
            "name": name,
            "length": length,
            "md5": actual_md5,
            "sha256": digest("sha256", payload),
            "offset": payload_offset,
        })
        payload_offset += length
        cursor += 72
    require(cursor <= HEADER_SIZE, "CE entry table exceeds its header")
    return image_type, records, payload_offset


def parse_uimage(kernel):
    require(len(kernel) >= 64, "Truncated uImage")
    fields = struct.unpack(">7I4B32s", kernel[:64])
    require(fields[0] == UIMAGE_MAGIC, "Bad uImage magic")
    header = bytearray(kernel[:64])
    struct.pack_into(">I", header, 4, 0)
    header_crc = zlib.crc32(header) & 0xFFFFFFFF
    require(header_crc == fields[1], "uImage header CRC mismatch")
    payload_size = fields[3]
    require(64 + payload_size <= len(kernel), "Truncated uImage payload")
    data_crc = zlib.crc32(kernel[64:64 + payload_size]) & 0xFFFFFFFF
    require(data_crc == fields[6], "uImage data CRC mismatch")
    require(not any(kernel[64 + payload_size:]), "Nonzero uImage padding")
    return {
        "name": fields[11].split(b"\0", 1)[0].decode("ascii", "replace"),
        "payload_size": payload_size,
        "load_address": f"0x{fields[4]:08x}",
        "entry_point": f"0x{fields[5]:08x}",
        "os_id": fields[7],
        "architecture_id": fields[8],
        "image_type_id": fields[9],
        "compression_id": fields[10],
        "header_crc_verified": True,
        "data_crc_verified": True,
    }


def verify(image_path):
    data = image_path.read_bytes()
    image_type, records, payload_end = parse_ce(data)
    require(image_type == "P60", f"Unexpected CE type: {image_type}")
    require([r["name"] for r in records] == ["fwupgrade.cfg", "kernel", "rootfs"],
            "Unexpected native Open Mesh payload set")
    config_data = data[records[0]["offset"]:records[0]["offset"] + records[0]["length"]]
    kernel = data[records[1]["offset"]:records[1]["offset"] + records[1]["length"]]
    rootfs = data[records[2]["offset"]:records[2]["offset"] + records[2]["length"]]

    config = configparser.ConfigParser()
    config.read_string(config_data.decode("ascii"))
    require(config.has_section("vmlinux") and config.has_section("rootfs"),
            "Missing fwupgrade.cfg sections")
    require(config["vmlinux"]["flashaddr"] == "0x1c0000", "Wrong kernel flash address")
    require(config["vmlinux"]["filemd5sum"] == records[1]["md5"], "Kernel config MD5 mismatch")
    require(config["vmlinux"]["filesha256sum"] == records[1]["sha256"], "Kernel config SHA-256 mismatch")
    require(config["rootfs"]["filemd5sum"] == records[2]["md5"], "Rootfs config MD5 mismatch")
    require(config["rootfs"]["filesha256sum"] == records[2]["sha256"], "Rootfs config SHA-256 mismatch")

    require(len(kernel) % (256 * 1024) == 0, "Kernel is not padded to 256 KiB")
    expected_rootfs_addr = KERNEL_FLASH_ADDR + len(kernel)
    require(int(config["rootfs"]["flashaddr"], 0) == expected_rootfs_addr,
            "Rootfs does not follow the padded kernel")
    require(len(kernel) + len(rootfs) <= SLOT_SIZE, "Image exceeds the 7 MiB firmware slot")

    uimage = parse_uimage(kernel)
    require(rootfs[:4] == b"hsqs", "Rootfs is not little-endian SquashFS")
    squashfs_size = struct.unpack_from("<Q", rootfs, 40)[0]
    require(squashfs_size + 4 <= len(rootfs), "Invalid SquashFS size")
    require(rootfs[-4:] == bytes.fromhex("deadc0de"), "Missing rootfs deadc0de trailer")
    require(rootfs[squashfs_size:-4] == b"\xff" * (len(rootfs) - squashfs_size - 4),
            "Unexpected rootfs padding")
    squashfs_size_field = int(config["rootfs"]["checksize"], 0)
    expected_check_size = (squashfs_size // 65536) * 65536
    require(squashfs_size_field == expected_check_size,
            "Rootfs check must cover only complete immutable SquashFS blocks")
    check_blocks = squashfs_size_field // 65536
    flash_md5 = digest("md5", rootfs[:check_blocks * 65536])
    require(config["rootfs"]["md5sum"] == flash_md5,
            f"Rootfs flash MD5 mismatch: config={config['rootfs']['md5sum']}, "
            f"computed={flash_md5}, blocks={check_blocks}")

    kernel_kb = len(kernel) // 1024
    rootfs_kb = 7168 - kernel_kb
    require(f"setenv kernel_size_1 {kernel_kb}" in config["vmlinux"]["cmd_success"],
            "Bootloader kernel size is not updated")
    require(f"setenv rootfs_size_1 {rootfs_kb}" in config["rootfs"]["cmd_success"],
            "Bootloader rootfs size is not updated")
    require(f"setenv vmlinux_start_addr 0x9f1c0000" in config["vmlinux"]["cmd_success"],
            "Bootloader kernel start address is not updated")
    require(f"setenv vmlinux_size 0x{len(kernel):x}" in config["vmlinux"]["cmd_success"],
            "Bootloader data-check kernel size is not updated")
    require(f"setenv vmlinux_checksum {records[1]['md5']}" in config["vmlinux"]["cmd_success"],
            "Bootloader data-check kernel MD5 is not updated")
    runtime_rootfs_addr = 0x9F000000 + expected_rootfs_addr
    require(f"setenv rootfs_start_addr 0x{runtime_rootfs_addr:x}" in config["rootfs"]["cmd_success"],
            "Bootloader data-check rootfs address is not updated")
    require(f"setenv rootfs_size 0x{expected_check_size:x}" in config["rootfs"]["cmd_success"],
            "Bootloader data-check rootfs size includes writable overlay blocks")
    require(f"setenv rootfs_checksum {config['rootfs']['md5sum']}" in config["rootfs"]["cmd_success"],
            "Bootloader data-check rootfs MD5 is not updated")

    return {
        "image": str(image_path.resolve()),
        "size": len(data),
        "sha256": digest("sha256", data),
        "ce_type": image_type,
        "payload_md5_checks_passed": len(records),
        "payload_end": payload_end,
        "metadata_trailing_bytes": len(data) - payload_end,
        "slot_payload_bytes": len(kernel) + len(rootfs),
        "slot_free_bytes": SLOT_SIZE - len(kernel) - len(rootfs),
        "kernel_flash_addr": config["vmlinux"]["flashaddr"],
        "rootfs_flash_addr": config["rootfs"]["flashaddr"],
        "kernel_padded_size": len(kernel),
        "rootfs_packaged_size": len(rootfs),
        "rootfs_squashfs_size": squashfs_size,
        "rootfs_padding_verified": True,
        "rootfs_deadc0de_trailer_verified": True,
        "config_hashes_verified": True,
        "bootloader_sizes_verified": True,
        "kernel_uimage": uimage,
        "parts": records,
        "runtime_status": "Statically verified; not booted or flashed",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    report = verify(args.image)
    text = json.dumps(report, indent=2) + "\n"
    if args.json:
        args.json.write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
