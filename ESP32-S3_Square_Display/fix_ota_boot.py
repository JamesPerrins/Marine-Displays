Import("env")
import os

# After every upload, write boot_app0.bin to the OTA data partition (0x29000)
# so the bootloader always boots from app0 instead of a stale app1 pointer.
def after_upload(source, target, env):
    boot_app0 = os.path.join(
        env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
        "tools", "partitions", "boot_app0.bin"
    )
    if not os.path.isfile(boot_app0):
        print("fix_ota_boot: boot_app0.bin not found at", boot_app0)
        return
    print("fix_ota_boot: writing boot_app0.bin to 0x29000 ...")
    env.Execute(
        " ".join([
            '"' + env.subst("$PYTHONEXE") + '"',
            "-m", "esptool",
            "--chip", "esp32s3",
            "--port", '"' + env.subst("$UPLOAD_PORT") + '"',
            "--baud", env.subst("$UPLOAD_SPEED"),
            "--before", "default_reset",
            "--after", "hard_reset",
            "write_flash",
            "-z",
            "0x29000",
            '"' + boot_app0 + '"',
        ])
    )
    print("fix_ota_boot: done")

env.AddPostAction("upload", after_upload)
