# GPU runs

```{warning}
GPU support is experimental. The CUDA search path has not yet been
validated on hardware for this release, and the HIP backend has not been
compiled or tested. Check GPU results against a CPU run before relying on
them.
```

## Building with a GPU backend

One `frost` build supports the CPU and at most one GPU toolchain. With the
default `-DFROST_GPU=AUTO`, CMake uses CUDA when it finds `nvcc`, otherwise
HIP when it finds ROCm, otherwise neither. To require one:

::::{tab-set}
:::{tab-item} CUDA
:sync: cuda

Needs the CUDA Toolkit 12.2 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFROST_BUILD_TESTING=OFF -DFROST_GPU=CUDA
cmake --build build -j
cmake --install build --prefix ~/.local
```

`FROST_CUDA_ARCHITECTURES` (default `native`, the GPU in this machine)
selects the GPU architectures to compile for, such as `80;90`.
:::
:::{tab-item} HIP (AMD ROCm)
:sync: hip

Needs ROCm 6.2 or newer.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFROST_BUILD_TESTING=OFF -DFROST_GPU=HIP
cmake --build build -j
cmake --install build --prefix ~/.local
```

`FROST_HIP_ARCHITECTURES` (default `gfx90a;gfx942`) selects the GPU
architectures to compile for.
:::
::::

With `CUDA` or `HIP`, configuring fails if no GPU is visible. To build on a
machine without one, add `-DFROST_GPU_CHECK_DEVICE=OFF`.

`frost --version` shows the toolchain in its `build:` line.

## Running on a GPU

```bash
frost --file obs.fil --dm-max 1000 --device cuda --device-id 0
```

`--device` is `cpu` (default), `cuda` or `hip`; `--device-id` picks the GPU.

On a GPU, dedispersion and the search run on the device, and so does the
default RFI excision for integer data. The opt-in RFI strategies run on the
host and upload the cleaned data. Clustering and output always run on the
host.

`--memory-mib` limits device memory on a GPU run, not host memory (see
[Memory and performance](resources.md)).
