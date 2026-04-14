Import("env")
import shutil
import os

def copy_firmware(source, target, env):
    bin_path = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
    shutil.copyfile(bin_path, "firmware_update.bin")
    
    # Copy littlefs.bin if it exists
    littlefs_path = os.path.join(env.subst("$BUILD_DIR"), "littlefs.bin")
    if os.path.exists(littlefs_path):
        shutil.copyfile(littlefs_path, "littlefs.bin")

def before_build_fs(*args, **kwargs):
    env.Execute("platformio run --target buildfs")

env.AddPostAction("$BUILD_DIR/firmware.bin", copy_firmware)
env.AddPostAction("buildprog", before_build_fs)