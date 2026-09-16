Import("env")

from pathlib import Path
import shutil
import tempfile
import urllib.request
import zipfile

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
CACHE_DIR = PROJECT_DIR / ".pio" / "esp_audio_codec_2_5_0"
ARCHIVE_URL = (
    "https://components.espressif.com/api/downloads/"
    "?object_id=4df9ba26-0974-4985-8584-7a29d7d65937&object_type=component"
)
EXPECTED_VERSION = "2.5.0"
LIB_NAME = "libesp_audio_codec.a"


def valid_component(root: Path) -> bool:
    manifest = root / "idf_component.yml"
    lib = root / "lib" / "esp32s3" / LIB_NAME
    if not manifest.is_file() or not lib.is_file():
        return False
    try:
        text = manifest.read_text(encoding="utf-8")
    except Exception:
        return False
    return f"version: {EXPECTED_VERSION}" in text or f"version: '{EXPECTED_VERSION}'" in text


if not valid_component(CACHE_DIR):
    print(f"esp_audio_codec: downloading pinned component v{EXPECTED_VERSION}")
    if CACHE_DIR.exists():
        shutil.rmtree(CACHE_DIR)
    CACHE_DIR.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="vs_esp_audio_codec_") as td:
        td = Path(td)
        archive = td / "component.zip"
        urllib.request.urlretrieve(ARCHIVE_URL, archive)
        extract = td / "extract"
        extract.mkdir()
        with zipfile.ZipFile(archive, "r") as zf:
            zf.extractall(extract)

        component_root = None
        for manifest in extract.rglob("idf_component.yml"):
            candidate = manifest.parent
            if valid_component(candidate):
                component_root = candidate
                break
        if component_root is None:
            raise RuntimeError(
                "esp_audio_codec v2.5.0 archive did not contain the expected "
                "ESP32-S3 prebuilt library"
            )
        shutil.copytree(component_root, CACHE_DIR)

if not valid_component(CACHE_DIR):
    raise RuntimeError("esp_audio_codec cache validation failed")

include = CACHE_DIR / "include"
libdir = CACHE_DIR / "lib" / "esp32s3"
env.Append(
    CPPPATH=[
        str(include),
        str(include / "encoder"),
        str(include / "encoder" / "impl"),
    ],
    LIBPATH=[str(libdir)],
    LIBS=["esp_audio_codec"],
)
print(f"esp_audio_codec: pinned v{EXPECTED_VERSION} from {CACHE_DIR}")
