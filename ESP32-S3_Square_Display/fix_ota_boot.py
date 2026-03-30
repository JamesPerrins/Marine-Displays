Import("env")
import os
from os.path import join

# ── PHY guard: patch sections.ld to add a 256-byte guard zone ─────────────
#
# chip7_phy_init_ctrl (from libphy.a, the closed-source WiFi PHY library) is
# the last BSS symbol before the DRAM heap starts. It is declared 42 bytes
# but the PHY writes past its end during normal WiFi packet processing (rate
# adaptation, channel calibration). With only 2 bytes of padding between the
# end of chip7_phy_init_ctrl and _heap_start, that overflow reaches
# multi_heap_info_t.lock (first 4 bytes of the heap), corrupting the heap
# spinlock pointer. Any subsequent calloc/malloc then crashes with
# StoreProhibited when it tries to acquire the heap lock at the bad address.
#
# GNU ld 2.35 (the toolchain in this framework) does not support INSERT BEFORE
# so we patch sections.ld at build-configure time: add ". += 256;" before the
# "_heap_start = ABSOLUTE(.);" line, then write the patched copy to BUILD_DIR
# and prepend "-L BUILD_DIR" to LINKFLAGS so the linker finds our copy first.
#
# The result is a 256-byte gap between BSS end and heap start. The PHY
# overflow lands in this harmless gap instead of in the heap metadata.

def patch_sections_ld():
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    memory_type = env.BoardConfig().get(
        "build.arduino.memory_type",
        env.BoardConfig().get("build.flash_mode", "dio") + "_qspi"
    )
    orig = join(framework_dir, "tools", "sdk", "esp32s3", memory_type, "sections.ld")
    if not os.path.isfile(orig):
        print("fix_ota_boot: WARNING — sections.ld not found at", orig)
        return

    with open(orig, "r") as f:
        content = f.read()

    MARKER = "_heap_start = ABSOLUTE(.);"
    if MARKER not in content:
        print("fix_ota_boot: WARNING — marker not found in sections.ld; guard not applied")
        return

    GUARD = (
        "/* PHY overflow guard: chip7_phy_init_ctrl (libphy.a BSS) writes past\n"
        "         * its declared 42-byte size into heap metadata. 256 bytes of padding\n"
        "         * absorbs the overflow before _heap_start. See fix_ota_boot.py. */\n"
        "        . += 256;\n"
        "        "
    )
    patched = content.replace(MARKER, GUARD + MARKER, 1)

    build_dir = env.subst("$BUILD_DIR")
    os.makedirs(build_dir, exist_ok=True)
    out = join(build_dir, "sections.ld")
    with open(out, "w") as f:
        f.write(patched)

    # Prepend our build dir so "-T sections.ld" resolves to our patched copy.
    env.Prepend(LINKFLAGS=["-L", build_dir])
    print("fix_ota_boot: PHY guard applied — sections.ld patched, +256 bytes before _heap_start")

patch_sections_ld()

# ── OTA: restore boot_app0 after every upload ──────────────────────────────
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
