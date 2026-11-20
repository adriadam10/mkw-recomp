# tools/ffx/patches

| File | Purpose |
|---|---|
| `fsr3-1-6-vulkan.patch` | Corrects Vulkan storage-image declarations and equal-depth stationary-vector arbitration in the MIT FidelityFX SDK v2.4.0, commit `7539ae8b6568ad29ed78a753404351daf3b5cca7` |

The default production build uses the motion correction and compiles only the
game-motion-vector-field shader. The storage-image declarations remain required
by the optional `--provider` diagnostic build. The patch retains AMD's source
licenses and contains no game data. The motion correction preserves the existing
depth ordering, reserves priority 31 for strong stationary correspondences, and
caps moving vectors at priority 30 when depth and correspondence confidence tie.
