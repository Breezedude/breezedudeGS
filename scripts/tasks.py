Import("env")

import os
import shutil
from SCons.Script import DefaultEnvironment

INSTALL_DIR = os.path.join(os.path.dirname(os.environ.get("PYTHONPATH", "").split(os.pathsep)[0]),
                           "..", "..", "install.breezedude", "esp_flasher")

def get_install_dir(project_dir):
    return os.path.normpath(os.path.join(project_dir, "..", "install.breezedude", "esp_flasher"))

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

    cmd = (
        f"python \"{esptool_path}\" --chip esp32s3 merge_bin -o \"{all_in_one}\" "
        f"--flash_mode dio --flash_freq 80m --flash_size 8MB "
        f"0x0000 \"{bootloader}\" "
        f"0x8000 \"{partitions}\" "
        f"0x10000 \"{firmware}\" "
        f"0xe000 \"{boot_app0}\" "
        f"0x670000 \"{littlefs}\""
    )

    print("Merging binaries into:", all_in_one)
    os.system(cmd)

    # Copy individual parts to install directory for web flasher
    install_dir = get_install_dir(project_dir)
    os.makedirs(install_dir, exist_ok=True)

    files = [
        (bootloader, "bootloader.bin"),
        (partitions, "partitions.bin"),
        (boot_app0,  "boot_app0.bin"),
        (firmware,   "firmware.bin"),
        (littlefs,   "littlefs.bin"),
    ]
    for src, name in files:
        dst = os.path.join(install_dir, name)
        if os.path.exists(src):
            shutil.copyfile(src, dst)
            print(f"Copied {name} → {dst}")
        else:
            print(f"Warning: {src} not found, skipping")


env.AddCustomTarget(
    name="build_allinone",
    dependencies=["buildfs","buildprog"],
    actions=[build_all_in_one],
    title="Build & Bundle All",
    description="Compiles firmware + FS, merges all-in-one.bin, copies parts to install dir"
)
