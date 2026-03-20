Import("env")

import os
from pathlib import Path

# ESP32 Arduino 3.x injects this define in a format that breaks micro_ros_platformio's
# generated CMake toolchain.
_BROKEN_DEFINE = "CHIP_ADDRESS_RESOLVE_IMPL_INCLUDE_HEADER"
_LIBDEPS = Path(env["PROJECT_DIR"]) / ".pio" / "libdeps" / env["PIOENV"]


def _remove_broken_define(flag_key):
    flags = list(env.get(flag_key, []))
    if not flags:
        return False

    filtered = [flag for flag in flags if _BROKEN_DEFINE not in str(flag)]
    if len(filtered) == len(flags):
        return False

    env.Replace(**{flag_key: filtered})
    return True


def _patch_micro_ros_extra_script():
    target = _LIBDEPS / "micro_ros_platformio" / "extra_script.py"
    if not target.exists():
        return False

    text = target.read_text()
    changed = False

    if "def sanitize_flags(flags):" not in text:
        insert_block = """    def sanitize_flags(flags):
        sanitized = []
        for flag in flags:
            flag_text = str(flag)
            if "CHIP_ADDRESS_RESOLVE_IMPL_INCLUDE_HEADER" in flag_text:
                continue
            sanitized.append(flag_text.replace('"', '\\\\"'))
        return ' '.join(sanitized)

    # CMake picks up host CFLAGS/CXXFLAGS and appends them, so strip the
    # problematic ESP-IDF define from host environment flags as well.
    for host_flag in ("CFLAGS", "CXXFLAGS", "CPPFLAGS"):
        host_value = os.environ.get(host_flag)
        if not host_value:
            continue
        cleaned = ' '.join(
            token for token in host_value.replace(';', ' ').split()
            if "CHIP_ADDRESS_RESOLVE_IMPL_INCLUDE_HEADER" not in token
        )
        os.environ[host_flag] = cleaned

"""
        marker = "    cmake_toolchain = library_builder.CMakeToolchain("
        if marker in text:
            text = text.replace(marker, insert_block + marker, 1)
            changed = True

    replacements = (
        ("' '.join(env['CFLAGS'])", "sanitize_flags(env['CFLAGS'])"),
        ("' '.join(env['CXXFLAGS'])", "sanitize_flags(env['CXXFLAGS'])"),
        ("' '.join(env['CCFLAGS'])", "sanitize_flags(env['CCFLAGS'])"),
    )
    for old, new in replacements:
        if old in text:
            text = text.replace(old, new)
            changed = True

    if changed:
        target.write_text(text)
    return changed


changed_flags = any(_remove_broken_define(key) for key in ("CFLAGS", "CCFLAGS", "CXXFLAGS"))
changed_micro_ros = _patch_micro_ros_extra_script()

if changed_flags or changed_micro_ros:
    print("Applied ESP32 Arduino 3.x compatibility workarounds for micro-ROS")
