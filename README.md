## KomputeRasterizer
This demo attempts to render scenes with a big amount of triangles in a GPU-driven fashion using the compute shader rasterization, both via vanilla compute shaders, and the work graphs. It also attempts to simulate game engine geometry load with cascaded shadows and culling.

It comes with two scenes, the Buddha - about 100M of really small triangles, and the Plant - about 40M of triangles of various sizes. Even with the most simplistic rasterization approach, software rasterizer outperforms hardware rasterizer on the Buddha scene, but the Plant scene is somewhat unstable in terms of performance, due to presence of alot of "big" triangles.

Demo attemps to distribute load over threads  with the notion of a big triangle - how big the triangle's screen area should be to rasterize it with a single thread, or to offload it to a multiple-threads rasterizer, or a hardware rasterizer?

## Features
* Fully GPU-driven rendering using indirect draws and compute dispatches.
* Compute-based software rasterization of depth, opaque geometry, and cascaded shadow maps.
* Configurable scanline or bounding-box traversal, according to the [top-left fill rule](https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-rasterizer-stage-rules#triangle-rasterization-rules-without-multisampling).
* Near-plane clipping support.
* Adaptive triangle processing: small triangles use a single-thread path, while large triangles are divided into configurable tiles and processed cooperatively.
* Runtime comparison between software and hardware rasterization by overdraw visualization and GPU performance/statistics reporting.
* Compares true overdraw - compute threads vs. pixel shader 2x2 quad threads (ordinary + helper ones).
* Traditional compute-shader and DirectX 12 Work Graphs paths on Windows.
* Meshlet frustum, backface-cone, and temporal Hi-Z occlusion culling for the camera and shadow cascades.
* Optional per-triangle Hi-Z culling during software rasterization.
* Adjustable cascaded shadow maps, with meshlet and cascade debug views.
* Windows DirectX 12 and macOS Metal versions.

## System requirements
* Windows 10, 64-bit.
* DirectX 12 compatible GPU.
* The Work Graphs codepath requires shader model 6.8 and a GPU/driver reporting D3D12 Work Graphs support. Unsupported systems automatically use the compute-shader rasterizer.

## How to build and run
* `git clone --recursive https://github.com/komputeshader/KomputeRasterization.git`
  * In the case you have cloned repo without the `--recursive` flag, perform the `git submodule update --init --recursive`.
* Download https://casual-effects.com/g3d/data10/index.html#mesh3 and place it into the `assets/buddha/` folder.
* Download https://casual-effects.com/g3d/data10/index.html#mesh25 and place it into the `assets/powerplant/` folder.

Windows:

* Open `windows/KomputeRasterization.sln` with Visual Studio.
* Right-click the solution and select **Restore NuGet Packages**. The required packages and versions are declared in `packages.config`.
* Build and run.

Mac:

* Open `mac/KomputeRasterization.xcodeproj` with Xcode.
* Select the `KomputeRasterization` scheme.
* Build and run.

## Papers and other resources used
* [A Parallel Algorithm for Polygon Rasterization](https://www.cs.drexel.edu/~david/Classes/Papers/comp175-06-pineda.pdf).
* [Optimizing the Graphics Pipeline with Compute](https://frostbite-wp-prd.s3.amazonaws.com/wp-content/uploads/2016/03/29204330/GDC_2016_Compute.pdf).
* The **Show Overdraw** views are based on Stephen Hill's [Overdraw in Overdrive](https://blog.selfshadow.com/publications/overdraw-in-overdrive/).
* Models downloaded from Morgan McGuire's [Computer Graphics Archive](https://casual-effects.com/data).
* Mesh loading is done with [Rapidobj](https://github.com/guybrush77/rapidobj).
* Mesh processing is done with [Meshoptimizer](https://github.com/zeux/meshoptimizer).
* GUI is done using the [IMGUI](https://github.com/ocornut/imgui).
