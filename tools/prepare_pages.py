"""Package released binaries and board-specific ESP Web Tools manifests."""
import json
import shutil
import sys
from pathlib import Path

BOARDS = {
    "m5stack_atoms3": ("ATOMS3 Lite", "ESP32-S3"),
    "m5stack_atom": ("ATOM Lite", "ESP32"),
    "esp32": ("Generic ESP32", "ESP32"),
    "esp32c3": ("Generic ESP32-C3", "ESP32-C3"),
    "esp32s3": ("Generic ESP32-S3", "ESP32-S3"),
}


def prepare(assets, output, version):
    # Fail before publishing if any release asset is missing or empty.
    names = [f"govee_party-{board}.bin" for board in BOARDS]
    names += [f"govee_party-{sdk}.fap" for sdk in ("momentum", "official")]
    for name in names:
        if not (assets / name).is_file() or not (assets / name).stat().st_size:
            raise ValueError(f"Missing or empty release asset: {name}")
    shutil.copytree("web", output, dirs_exist_ok=True)
    firmware = output / "firmware"
    firmware.mkdir(exist_ok=True)
    for name in names:
        shutil.copy2(assets / name, firmware / name)
    for board, (label, chip) in BOARDS.items():
        manifest = {
            "name": f"Govee Dance Party — {label}",
            "version": version,
            "new_install_prompt_erase": True,
            "new_install_improv_wait_time": 0,
            "builds": [{"chipFamily": chip, "parts": [
                {"path": f"govee_party-{board}.bin", "offset": 0}
            ]}],
        }
        (firmware / f"{board}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (output / "release.json").write_text(json.dumps({"version": version}) + "\n")


if __name__ == "__main__":
    prepare(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3])
