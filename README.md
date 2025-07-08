## KomputeRasterizer
This demo attempts to render scenes with a big amount of triangles using the compute shader rasterization. It also attempts to simulate game engine geometry load with cascaded shadows and culling.

It comes with two scenes, the Buddha - about 100M of really small triangles, and the Plant - about 40M of triangles of various sizes. Even with the most simplistic rasterization approach, software rasterizer outperforms hardware rasterizer on the Buddha scene, but the Plant scene is quite unstable for now in terms of performance, due to presence of alot of "big" triangles.

Demo attemps to distribute load over threads  with the notion of big triangle - how big the triangle's screen area should be to rasterize it with a single thread, or to offload it to multiple-threads rasterizer, or hardware rasterizer?

## System requirements
1. Windows 10, 64-bit.
2. DirectX 12 compatible GPU.
3. The WinPixEventRuntime is required to build, see instructions: https://devblogs.microsoft.com/pix/winpixeventruntime/.

## How to build and run
1. `git clone --recursive https://github.com/komputeshader/KomputeRasterization.git`
2. Download https://casual-effects.com/g3d/data10/index.html#mesh3 and place it into the `Buddha/` folder.
3. Download https://casual-effects.com/g3d/data10/index.html#mesh25 and place it into the `powerplant/` folder.
4. Open `.sln` file with the Visual Studio.
5. Build and run using Visual Studio.

## Papers and other resources used
* [A Parallel Algorithm for Polygon Rasterization](https://www.cs.drexel.edu/~david/Classes/Papers/comp175-06-pineda.pdf)
* [Optimizing the Graphics Pipeline with Compute](https://frostbite-wp-prd.s3.amazonaws.com/wp-content/uploads/2016/03/29204330/GDC_2016_Compute.pdf)
* Models downloaded from Morgan McGuire's [Computer Graphics Archive](https://casual-effects.com/data)
* Mesh loading is done with [Rapidobj](https://github.com/guybrush77/rapidobj)
* Mesh processing is done with [Meshoptimizer](https://github.com/zeux/meshoptimizer)
* GUI is done using the [IMGUI](https://github.com/ocornut/imgui)
