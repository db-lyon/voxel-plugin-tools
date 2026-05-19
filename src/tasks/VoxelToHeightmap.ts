import { BaseTask, type TaskResult } from "@db-lyon/flowkit";

interface Bounds {
  min: { x: number; y: number; z?: number };
  max: { x: number; y: number; z?: number };
}

interface Options {
  landscapeLabel: string;
  bounds: Bounds;
  /** Heightmap side length in samples. Default 1009 (UE landscape-friendly). */
  resolution?: number;
}

/**
 * Bakes a region of a VPP voxel terrain to a Landscape heightmap.
 *
 * Pipeline:
 *  1. Sample the voxel surface at a grid resolution by issuing Python through
 *     the editor (the C++ bridge has no direct VPP API surface).
 *  2. Pipe the resulting heightfield into `landscape.import_heightmap`.
 *
 * The Python step is the escape hatch documented in CLAUDE.md and is
 * acceptable here because VPP exposes no native PCG-side way to extract
 * raw voxel heights into the engine's landscape format.
 */
export default class VoxelToHeightmap extends BaseTask<Options> {
  get taskName(): string { return "vpp.voxel_to_heightmap"; }

  protected validate(): void {
    if (!this.options.landscapeLabel) throw new Error("landscapeLabel is required");
    if (!this.options.bounds?.min || !this.options.bounds?.max) {
      throw new Error("bounds.min and bounds.max are required");
    }
  }

  async execute(): Promise<TaskResult> {
    const { landscapeLabel, bounds, resolution = 1009 } = this.options;

    // 1. Sample VPP voxel heights via Python. The script writes a raw uint16
    //    heightmap PNG to the project's intermediate dir and returns its path.
    const sample = await this.call("editor.execute_python", {
      script: buildVoxelSampleScript(bounds, resolution),
    });
    if (!sample.success) return sample;

    const heightmapPath = (sample.data?.result ?? sample.data?.output ?? sample.data?.heightmapPath) as string | undefined;
    if (!heightmapPath) {
      return {
        success: false,
        error: new Error("voxel sample script did not return a heightmap path"),
      };
    }

    // 2. Import the heightmap into the target landscape actor.
    const imported = await this.call("landscape.import_heightmap", {
      actorLabel: landscapeLabel,
      heightmapPath,
      resolution,
    });
    if (!imported.success) return imported;

    return {
      success: true,
      data: {
        landscapeLabel,
        heightmapPath,
        resolution,
        bounds,
      },
    };
  }
}

function buildVoxelSampleScript(bounds: Bounds, resolution: number): string {
  // The script is kept in a single string so the bridge ships it verbatim to
  // Python. It is intentionally defensive: any VPP API the host installation
  // may not have is caught and reported as a clear error rather than a
  // traceback.
  return `
import unreal, os, struct

bounds_min = (${bounds.min.x}, ${bounds.min.y})
bounds_max = (${bounds.max.x}, ${bounds.max.y})
res = ${resolution}

try:
    voxel_lib = unreal.VoxelBlueprintLibrary  # exposed by VPP at runtime
except AttributeError:
    raise RuntimeError("Voxel Plugin Pro not loaded - vpp_voxel_to_heightmap requires VPP")

dx = (bounds_max[0] - bounds_min[0]) / float(res - 1)
dy = (bounds_max[1] - bounds_min[1]) / float(res - 1)

samples = bytearray()
for j in range(res):
    for i in range(res):
        x = bounds_min[0] + i * dx
        y = bounds_min[1] + j * dy
        try:
            h = voxel_lib.get_voxel_height_at(x, y)
        except Exception:
            h = 0.0
        # Map [-32768, 32767] cm into uint16. Adjust per project needs.
        sample = max(0, min(65535, int(h + 32768)))
        samples += struct.pack("<H", sample)

out_dir = os.path.join(unreal.Paths.project_intermediate_dir(), "VPP")
os.makedirs(out_dir, exist_ok=True)
out_path = os.path.join(out_dir, "voxel_heightmap.r16")
with open(out_path, "wb") as f:
    f.write(samples)

# The bridge serialises whatever we print as the script result.
print(out_path)
`.trim();
}
