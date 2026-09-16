Import("env")

from pathlib import Path
import shutil
import subprocess

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
COMPONENT_DIR = PROJECT_DIR / "components" / "micro-opus"
EXPECTED_SHA = "8354085908683c6130e32a832aeec8a7ca115c51"
REPO = "https://github.com/esphome-libs/micro-opus.git"


def run(*args, cwd=None):
    print("micro-opus:", " ".join(str(a) for a in args))
    subprocess.check_call([str(a) for a in args], cwd=str(cwd) if cwd else None)


def current_sha():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(COMPONENT_DIR), text=True
        ).strip()
    except Exception:
        return ""


COMPONENT_DIR.parent.mkdir(parents=True, exist_ok=True)

if current_sha() != EXPECTED_SHA:
    if COMPONENT_DIR.exists():
        shutil.rmtree(COMPONENT_DIR)
    run(
        "git", "clone", "--recursive", "--depth", "1", "--branch", "v0.4.1",
        REPO, COMPONENT_DIR,
    )
    actual = current_sha()
    if actual != EXPECTED_SHA:
        raise RuntimeError(
            f"micro-opus v0.4.1 resolved to {actual}, expected {EXPECTED_SHA}"
        )
else:
    run("git", "submodule", "update", "--init", "--recursive", cwd=COMPONENT_DIR)

print(f"micro-opus: pinned at {EXPECTED_SHA}")
