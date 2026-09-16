Import("env")

from pathlib import Path
import shutil

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
DEFAULTS = PROJECT_DIR / "sdkconfig.defaults"
SDKCONFIG = PROJECT_DIR / "sdkconfig"

# PlatformIO's generic ESP32-S3 DevKit board manifest describes a no-PSRAM
# board and can otherwise override ESP-IDF defaults. Make the benchmark's
# hardware config authoritative before CMake runs.
shutil.copyfile(DEFAULTS, SDKCONFIG)
print(f"esp-opus: authoritative sdkconfig <- {DEFAULTS.name}")
