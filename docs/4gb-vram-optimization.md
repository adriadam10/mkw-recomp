# 4 GB VRAM & Laptop GPU Optimization Guide

Performance tuning guide and configuration profile for GPUs with 4 GB of VRAM (such as the NVIDIA GeForce RTX 3050 Laptop, RTX 3050 Ti Laptop, GTX 1650, and RTX 2050).

---

## 1. Background & Root Causes

On 4 GB VRAM systems running Windows, the Desktop Window Manager (DWM) and background applications typically consume 600–900 MiB of VRAM, leaving approximately 3,000–3,300 MiB available for Mario Kart World. 

Under the default configuration, 4 GB systems encounter three distinct bottlenecks:

1. **Slow-Motion at 0.50x (Half Speed):**
   * If `framegen` is left at default (2x interpolation) while baseline framerates dip below 60 FPS, the game's internal frame timer locks to 30 FPS interval pacing. The internal clock treats each tick as 1/30 s while presenting interpolated frames, halving the game's simulation speed to 0.50x (~33 FPS).
2. **First-Time Shader Compilation Hitches (1.5s – 2.2s):**
   * New track geometry, lighting passes, and cutscenes generate new Vulkan pipeline variants that are not yet cached in `pipeline-keys.bin`. Without draw call batch slicing, synchronous pipeline builds stall the command recording queue for 1,500 to 2,200 ms.
3. **Synchronous VMA Pool Allocations:**
   * Allocating 256 MiB Vulkan Memory Allocator (VMA) blocks synchronously mid-race causes `pool_fini` and `map_virtual` stalls.
4. **ASTC Texture Decode Overhead:**
   * Decoding ASTC textures on CPU software worker threads causes thread starvation on mobile CPUs.

---

## 2. Recommended 4 GB Profile (`mkw.ini`)

To apply this profile, place the following settings in your `mkw.ini` (located in the recompiler executable directory or the game ROM directory):

```ini
[frame]
fps = 60
framegen = off           # Disables 2x interpolation; locks native 1.00x game speed
timer_1ms = 1           # Enables 1 ms precision multimedia timer (timeBeginPeriod)

[performance]
async_preempt = 1       # Allows Vulkan render queues to preempt compute workloads
split_draws = 16        # Slices complex draw bursts into micro-batches (default 64)
split_us = 100          # Coarse draw duration cap in microseconds (default 2000)
pipe_skip = fullscreen  # Skips full-screen passes whose pipeline is still building
pipe_prebuild = 1       # Pre-warms shaders recorded in pipeline-keys.bin at boot
astc = gpu              # Offloads ASTC texture decompression to GPU compute shaders
tex_alias = 1           # Reuses live texture allocations to minimize VRAM churn
vram_headroom = 1       # Pre-allocates next 256 MB VMA block on a background thread

[display]
resolution = 1080       # Native 1080p (use 720 with upscaler = fsr on entry-level GPUs)
upscaler = fsr
sharpness = 40
vk_present = mailbox    # Triple-buffered low-latency display presentation
perf_overlay = fps

[graphics]
quality = console
shadow_size = console   # Restricts shadow maps to the 250 MB pool (prevents VRAM exhaustion)
```

---

## 3. Low-Latency Environment Flags

For launcher scripts or command-line execution, the following environment flags further reduce queue contention:

* **`MKW_FENCE_EARLY=1`**: Signals guest NVN fence waits as soon as command buffers are submitted to the queue, eliminating 90–130 ms synchronous waits for physical GPU execution.
* **`MKW_SUBMIT_THREAD=1`**: Offloads `vkQueueSubmit2` submissions to an independent worker thread, preventing queue lock contention on the draw recording thread.

---

## 4. Benchmark Measurements

Measurements taken on a physical laptop matching this hardware tier:

* **GPU:** NVIDIA GeForce RTX 3050 Ti Laptop GPU (4 GB GDDR6, 75W TGP, Driver 621.01, Vulkan 1.4.357)
* **CPU:** 11th Gen Intel Core i7-11800H (8 cores / 16 threads, 2.30 GHz base / 4.60 GHz boost)
* **RAM:** 16 GB DDR4-3200
* **OS:** Windows 11 Home 64-bit

| Metric | Default / Baseline | With 4 GB Profile | Improvement |
| :--- | :--- | :--- | :--- |
| **Game Speed** | 0.50x – 0.53x *(Slow Motion)* | **0.97x – 0.98x *(Full Real-Time)*** | **+85% Speed Increase** |
| **Average Framerate** | 33.2 FPS | **58.6 FPS** | **+76% Framerate Jump** |
| **1% Low Framerate** | 22.7 FPS | **51.3 FPS** | **+126% Consistency** |
| **Worst Racing Frame** | 2,209.6 ms *(2.2s Freeze)* | **44.0 ms** | **98% Stutter Duration Drop** |
| **Mid-Race Shader Builds** | 110 waiting draws stalled | **0 draws stalled *(Pre-warmed at boot)*** | **Seamless racing** |
| **ASTC Texture Decode** | CPU emulation bottlenecks | **GPU Compute: 11.1B texels (0.0 ms stall)** | **Hardware accelerated** |
