"""Freeze janblade's baseline and compile independent literal-sigma GPU references."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BASE = "b276cbb4"
OUT = ROOT / "artifacts/validation/refinement-reference"
SHADERS = "OptiScaler/shaders/dlssnr/"


def original(path):
    return subprocess.check_output(["git", "show", BASE + ":" + path], cwd=ROOT)


def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    review = ROOT / "artifacts/validation/review-reference"
    review.mkdir(parents=True, exist_ok=True)
    baseline_paths = subprocess.check_output(["git", "ls-tree", "-r", "--name-only", "7362c5f6",
                                             SHADERS + "superior/precompile"], cwd=ROOT,text=True).splitlines()
    for stem in [Path(path).name.removesuffix("_Shader.cso") for path in baseline_paths if path.endswith("_Shader.cso")]:
        data = subprocess.check_output(["git", "show", "7362c5f6:" + SHADERS +
                                        "superior/precompile/" + stem + "_Shader.cso"], cwd=ROOT)
        (review / (stem + "_Shader.cso")).write_bytes(data)
    paths = subprocess.check_output(
        ["git", "ls-tree", "-r", "--name-only", BASE, SHADERS + "superior/precompile"], cwd=ROOT, text=True
    ).splitlines()
    for path in paths:
        if path.endswith("_Shader.cso"):
            (OUT / Path(path).name).write_bytes(original(path))
    for name in ["dlssnr_spatial_guides_Shader.cso", "dlssnr_spatial_Shader.cso"]:
        (OUT / name).write_bytes(original(SHADERS + "precompile/" + name))
    files = ["precompile/dlssnr.hlsl", "precompile/dlssnr_whitepoint.hlsli",
             "precompile/dlssnr_replace_curve.hlsli", "precompile/dlssnr_pq.hlsli",
             "precompile/dlssnr_spatial_warp.hlsli", "superior/WarpMap.hlsli"]
    for name in files:
        path = OUT / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(original(SHADERS + name))
    host = original(SHADERS + "superior/HostResolve.hlsli").decode("utf-8")
    dxc = ROOT / "OptiScaler/shaders/shader_tools/dxc.exe"
    assert host.count("max(abs(0.015), 1e-5)") == 1, "Literal-sigma reference pattern changed"
    for percent in [50, 65, 70, 88, 100]:
        t = max(0.0, min(1.0, (percent - 50) / 50))
        sigma = 0.015 - 0.012 * t ** 3
        (OUT / "superior/HostResolve.hlsli").write_text(
            host.replace("max(abs(0.015), 1e-5)", f"max(abs({sigma:.12f}), 1e-5)"), encoding="utf-8"
        )
        for packed in [False, True]:
            for sdr in [False, True]:
                name = f"literal_{percent}_" + ("packed_" if packed else "") + ("sdr" if sdr else "hdr")
                defines = ["-D", "DLSSNR_SUPERIOR_HOST_RESOLVE=1"]
                if packed:
                    defines += ["-D", "DLSSNR_SUPERIOR_HOST_PACKED=1"]
                if sdr:
                    defines += ["-D", "DLSSNR_SUPERIOR_HOST_SDR=1"]
                subprocess.run([str(dxc), "-T", "cs_6_0", "-E", "CSMain", "-O3", "-Qstrip_debug",
                                "-Qstrip_reflect", *defines, str(OUT / "precompile/dlssnr.hlsl"),
                                "-Fo", str(OUT / (name + "_Shader.cso"))], check=True, cwd=ROOT)
    print("Prepared janblade b276cbb4 baseline and 20 literal cubic-sigma references")


if __name__ == "__main__":
    prepare()
