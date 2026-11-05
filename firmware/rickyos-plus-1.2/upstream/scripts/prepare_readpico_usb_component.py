"""Install the pinned Arduino TinyUSB component for the Read Pico USB trial.

PioArduino's custom-core build omits the TinyUSB source component, while the
Read Pico's known-good IDF settings require that custom core. This helper adds
Espressif's matching component under the project's ignored .pio/ tree and
registers it only with this experimental PlatformIO environment.
It never changes an existing component; a mismatch fails closed.
"""

import hashlib
from pathlib import Path
import subprocess
import tempfile
from urllib.request import urlopen

Import("env")  # type: ignore[name-defined]  # PlatformIO injects this symbol.

PROJECT = Path(env.subst("$PROJECT_DIR"))
COMPONENTS = PROJECT / ".pio" / "rickyos_usb_components"
DESTINATION = COMPONENTS / "arduino_tinyusb"
LIB_BUILDER_COMMIT = "ee57070cc8571c488a64b8ef028684acdc6ff5e2"
TINYUSB_COMMIT = "53f8c53c2cbd73a91a172f1ae35e9abc00eb5075"
SOURCE_ROOT = (
    "https://raw.githubusercontent.com/espressif/esp32-arduino-lib-builder/"
    f"{LIB_BUILDER_COMMIT}/components/arduino_tinyusb"
)
COMPONENT_FILES = {
    "CMakeLists.txt": "2cb28274878aa525abdf3e6c22f8cfd7f7f5233187c13a138337fc64515bab6f",
    "Kconfig.projbuild": "8096e2a0369df9a70eacf40982b15ec1c22691ccd10857700a33b5bde940ad40",
    "include/tusb_config.h": "93bbb9c4fc4140b06f5f40e58fcfdc42eaf0fd1bb97317a6250e80b6d426df40",
}


def verify_component(path: Path) -> bool:
    if not path.is_dir():
        return False
    for name, digest in COMPONENT_FILES.items():
        file = path / name
        if not file.is_file() or hashlib.sha256(file.read_bytes()).hexdigest() != digest:
            return False
    try:
        actual = subprocess.check_output(
            ["git", "-C", str(path / "tinyusb"), "rev-parse", "HEAD"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return False
    return actual == TINYUSB_COMMIT


if DESTINATION.exists():
    if not verify_component(DESTINATION):
        raise RuntimeError(f"Unexpected TinyUSB component at {DESTINATION}; refusing to overwrite")
else:
    COMPONENTS.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="rickyos-tinyusb-", dir=COMPONENTS) as temp_name:
        staged = Path(temp_name) / "arduino_tinyusb"
        staged.mkdir()
        for name, digest in COMPONENT_FILES.items():
            target = staged / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with urlopen(f"{SOURCE_ROOT}/{name}", timeout=30) as response:
                contents = response.read()
            if hashlib.sha256(contents).hexdigest() != digest:
                raise RuntimeError(f"TinyUSB component source hash mismatch: {name}")
            target.write_bytes(contents)

        tinyusb = staged / "tinyusb"
        subprocess.run(["git", "init", "-q", str(tinyusb)], check=True)
        subprocess.run(
            ["git", "-C", str(tinyusb), "remote", "add", "origin", "https://github.com/hathach/tinyusb.git"],
            check=True,
        )
        subprocess.run(["git", "-C", str(tinyusb), "fetch", "-q", "--depth", "1", "origin", TINYUSB_COMMIT], check=True)
        subprocess.run(["git", "-C", str(tinyusb), "checkout", "-q", "--detach", "FETCH_HEAD"], check=True)
        if not verify_component(staged):
            raise RuntimeError("Pinned Arduino TinyUSB component failed verification")
        staged.rename(DESTINATION)

# PioArduino's custom-core pass supplies .dummy and the Arduino framework as
# EXTRA_COMPONENT_DIRS. Preserve both while adding this trial-only component.
# Keeping it out of PROJECT/components prevents a prior USB trial from changing
# later release builds or other device variants.
framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
if not framework.is_dir():
    raise RuntimeError("Arduino framework directory is unavailable")
component_dirs = (PROJECT / ".dummy", framework, DESTINATION)
env.BoardConfig().update(
    "build.cmake_extra_args",
    "-DEXTRA_COMPONENT_DIRS:PATH=" + ";".join(str(path) for path in component_dirs),
)
