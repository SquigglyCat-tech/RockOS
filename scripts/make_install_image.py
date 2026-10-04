import argparse
import math
import os
import struct


SECTOR_SIZE = 512
IMAGE_SIZE = 16 * 1024 * 1024
PARTITION_START = 2048
SECTORS_PER_CLUSTER = 1
ROOT_ENTRY_COUNT = 512
FAT_COUNT = 2
RESERVED_SECTORS = 1
END_OF_CHAIN = 0xFFFF


def short_entry(name, extension, attributes, cluster, size):
    entry = bytearray(32)
    entry[0:8] = name.encode("ascii").ljust(8, b" ")
    entry[8:11] = extension.encode("ascii").ljust(3, b" ")
    entry[11] = attributes
    struct.pack_into("<H", entry, 26, cluster)
    struct.pack_into("<I", entry, 28, size)
    return entry


def long_entries(name, short_name):
    encoded = name.encode("utf-16le")
    units = list(struct.unpack("<" + "H" * (len(encoded) // 2), encoded))
    units.append(0)
    while len(units) % 13:
        units.append(0xFFFF)
    checksum = 0
    for value in short_name:
        checksum = (((checksum & 1) << 7) + (checksum >> 1) + value) & 0xFF

    result = []
    count = len(units) // 13
    for sequence in range(count, 0, -1):
        entry = bytearray(32)
        entry[0] = sequence | (0x40 if sequence == count else 0)
        entry[11] = 0x0F
        entry[13] = checksum
        chunk = units[(sequence - 1) * 13:sequence * 13]
        for index, value in enumerate(chunk[:5]):
            struct.pack_into("<H", entry, 1 + index * 2, value)
        for index, value in enumerate(chunk[5:11]):
            struct.pack_into("<H", entry, 14 + index * 2, value)
        struct.pack_into("<H", entry, 26, 0)
        for index, value in enumerate(chunk[11:13]):
            struct.pack_into("<H", entry, 28 + index * 2, value)
        result.append(entry)
    return result


def make_image(kernel_path, bios_path, efi_path, config_path, output_path):
    total_sectors = IMAGE_SIZE // SECTOR_SIZE
    partition_sectors = total_sectors - PARTITION_START
    root_sectors = ROOT_ENTRY_COUNT * 32 // SECTOR_SIZE

    fat_sectors = 1
    while True:
        data_start = RESERVED_SECTORS + FAT_COUNT * fat_sectors + root_sectors
        cluster_count = (partition_sectors - data_start) // SECTORS_PER_CLUSTER
        required_fat_sectors = math.ceil((cluster_count + 2) * 2 / SECTOR_SIZE)
        if required_fat_sectors == fat_sectors:
            break
        fat_sectors = required_fat_sectors
    if not 4085 <= cluster_count < 65525:
        raise ValueError("Calculated FAT16 cluster count is invalid")

    image = bytearray(IMAGE_SIZE)
    mbr_partition = bytearray(16)
    mbr_partition[0] = 0x80
    mbr_partition[1:4] = b"\xFE\xFF\xFF"
    mbr_partition[4] = 0xEF
    mbr_partition[5:8] = b"\xFE\xFF\xFF"
    struct.pack_into("<II", mbr_partition, 8,
        PARTITION_START, partition_sectors)
    image[446:462] = mbr_partition
    image[510:512] = b"\x55\xAA"

    boot_sector = bytearray(SECTOR_SIZE)
    boot_sector[0:3] = b"\xEB\x3C\x90"
    boot_sector[3:11] = b"ROCKOS  "
    struct.pack_into("<HBHBHHBHHHII", boot_sector, 11,
        SECTOR_SIZE, SECTORS_PER_CLUSTER, RESERVED_SECTORS, FAT_COUNT,
        ROOT_ENTRY_COUNT, partition_sectors, 0xF8, fat_sectors,
        63, 255, PARTITION_START, 0)
    boot_sector[36] = 0x80
    boot_sector[38] = 0x29
    struct.pack_into("<I", boot_sector, 39, 0x524F4B31)
    boot_sector[43:54] = b"ROCKOS     "
    boot_sector[54:62] = b"FAT16   "
    boot_sector[510:512] = b"\x55\xAA"
    partition_offset = PARTITION_START * SECTOR_SIZE
    image[partition_offset:partition_offset + SECTOR_SIZE] = boot_sector

    with open(kernel_path, "rb") as source:
        kernel = source.read()
    with open(bios_path, "rb") as source:
        bios = source.read()
    with open(efi_path, "rb") as source:
        efi = source.read()
    with open(config_path, "rb") as source:
        config = source.read()

    items = [
        {"name": "boot", "short": b"BOOT       ", "attributes": 0x10,
         "parent": (), "size": 0, "data": None, "cluster": 0},
        {"name": "EFI", "short": b"EFI        ", "attributes": 0x10,
         "parent": (), "size": 0, "data": None, "cluster": 0},
        {"name": "rockos.bin", "short": b"ROCKOS  BIN", "attributes": 0x20,
         "parent": ("boot",), "size": len(kernel), "data": kernel, "cluster": 0},
        {"name": "limine-bios.sys", "short": b"LIMINE~1SYS", "attributes": 0x20,
         "parent": ("boot",), "size": len(bios), "data": bios, "cluster": 0},
        {"name": "limine.conf", "short": b"LIMINE  CFG", "attributes": 0x20,
         "parent": ("boot",), "size": len(config), "data": config, "cluster": 0},
        {"name": "BOOT", "short": b"BOOT       ", "attributes": 0x10,
         "parent": ("efi",), "size": 0, "data": None, "cluster": 0},
        {"name": "BOOTX64.EFI", "short": b"BOOTX64 EFI", "attributes": 0x20,
         "parent": ("efi", "boot"), "size": len(efi), "data": efi,
         "cluster": 0},
    ]

    fat = [0] * (cluster_count + 2)
    fat[0] = 0xFFF8
    fat[1] = END_OF_CHAIN
    next_cluster = 2
    cluster_size = SECTOR_SIZE * SECTORS_PER_CLUSTER
    directory_clusters = {}

    for item in items:
        if item["attributes"] & 0x10:
            item["cluster"] = next_cluster
            directory_path = item["parent"] + (item["name"].lower(),)
            directory_clusters[directory_path] = next_cluster
            fat[next_cluster] = END_OF_CHAIN
            next_cluster += 1
            continue
        data = item["data"]
        cluster_count_for_file = math.ceil(len(data) / cluster_size)
        if next_cluster + cluster_count_for_file >= len(fat):
            raise ValueError("Install files do not fit in the FAT16 volume")
        if cluster_count_for_file:
            item["cluster"] = next_cluster
            for index in range(cluster_count_for_file):
                cluster = next_cluster + index
                fat[cluster] = (cluster + 1 if index + 1 < cluster_count_for_file
                    else END_OF_CHAIN)
                source_start = index * cluster_size
                destination_sector = data_start + cluster - 2
                destination = partition_offset + destination_sector * SECTOR_SIZE
                chunk = data[source_start:source_start + cluster_size]
                image[destination:destination + len(chunk)] = chunk
            next_cluster += cluster_count_for_file

    def directory_cluster(path):
        return directory_clusters[path] if path else 0

    for item in items:
        if item["attributes"] & 0x10:
            path = item["parent"] + (item["name"].lower(),)
            item["cluster"] = directory_clusters[path]

    fat_bytes = struct.pack("<" + "H" * len(fat), *fat)
    fat_offset = partition_offset + RESERVED_SECTORS * SECTOR_SIZE
    fat_area_size = fat_sectors * SECTOR_SIZE
    image[fat_offset:fat_offset + len(fat_bytes)] = fat_bytes
    second_fat_offset = fat_offset + fat_area_size
    image[second_fat_offset:second_fat_offset + len(fat_bytes)] = fat_bytes
    root_offset = partition_offset + (
        RESERVED_SECTORS + FAT_COUNT * fat_sectors) * SECTOR_SIZE

    directories = [
        ((), 0),
        (("boot",), directory_cluster(("boot",))),
        (("efi",), directory_cluster(("efi",))),
        (("efi", "boot"), directory_cluster(("efi", "boot"))),
    ]
    for path, cluster in directories:
        entries = []
        if path:
            parent_path = path[:-1]
            parent_cluster = directory_cluster(parent_path)
            entries.extend([
                short_entry(".", "", 0x10, cluster, 0),
                short_entry("..", "", 0x10, parent_cluster, 0),
            ])
        for item in items:
            if item["parent"] != path:
                continue
            short_name = item["short"]
            entries.extend(long_entries(item["name"], short_name))
            entries.append(short_entry(
                short_name[:8].decode("ascii").rstrip(),
                short_name[8:].decode("ascii").rstrip(),
                item["attributes"], item["cluster"], item["size"]))
        entries.append(bytearray(32))
        directory_data = b"".join(entries)
        if path:
            start_sector = data_start + cluster - 2
            offset = partition_offset + start_sector * SECTOR_SIZE
            if len(directory_data) > cluster_size:
                raise ValueError("Directory exceeds its allocated cluster")
            image[offset:offset + len(directory_data)] = directory_data
        else:
            if len(directory_data) > root_sectors * SECTOR_SIZE:
                raise ValueError("FAT16 root directory is full")
            image[root_offset:root_offset + len(directory_data)] = directory_data

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "wb") as output:
        output.write(image)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--bios", required=True)
    parser.add_argument("--efi", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--output", required=True)
    arguments = parser.parse_args()
    make_image(arguments.kernel, arguments.bios, arguments.efi,
        arguments.config, arguments.output)


if __name__ == "__main__":
    main()
