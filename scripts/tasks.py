Import("env")

import os
from SCons.Script import DefaultEnvironment

def build_all_in_one(source, target, env):
    platform = env.PioPlatform()
    esptool_path = os.path.join(platform.get_package_dir("tool-esptoolpy"), "esptool.py")

    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware = os.path.join(build_dir, "firmware.bin")
    littlefs = os.path.join(build_dir, "littlefs.bin")
    boot_app0 = os.path.join(platform.get_package_dir("framework-arduinoespressif32"), "tools", "partitions", "boot_app0.bin")

    all_in_one = os.path.join(project_dir, "all-in-one.bin")

    print("=== ACTIVE PARTITIONS ===")
    print(env.PrintConfiguration())

    cmd = (
        f"python \"{esptool_path}\" --chip esp32s3 merge_bin -o \"{all_in_one}\" "
        f"--flash_mode dio --flash_freq 80m --flash_size 8MB "
        f"0x0000 \"{bootloader}\" "
        f"0x8000 \"{partitions}\" "
        f"0x10000 \"{firmware}\" "
        f"0xe000 \"{boot_app0}\" "
        f"0x670000 \"{littlefs}\""
    )

    print("Merging binaries into:", all_in_one, cmd)
    os.system(cmd)


env.AddCustomTarget(
    name="build_allinone",
    dependencies=["buildfs","buildprog"],   # build firmware + FS image
    actions=[build_all_in_one],    # then merge bins
    title="Build & Bundle All",
    description="Compiles and bundles into one binary"
)
