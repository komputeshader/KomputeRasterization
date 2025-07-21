## KomputeRasterizer
This demo attempts to render scenes with a big amount of triangles in a GPU-driven fashion using the compute shader rasterization, both via vanilla compute shaders, and the work graphs. It also attempts to simulate game engine geometry load with cascaded shadows and culling.

It comes with two scenes, the Buddha - about 100M of really small triangles, and the Plant - about 40M of triangles of various sizes. Even with the most simplistic rasterization approach, software rasterizer outperforms hardware rasterizer on the Buddha scene, but the Plant scene is somewhat unstable in terms of performance, due to presence of alot of "big" triangles.

Demo attemps to distribute load over threads  with the notion of a big triangle - how big the triangle's screen area should be to rasterize it with a single thread, or to offload it to a multiple-threads rasterizer, or a hardware rasterizer?

## System requirements
1. Windows 10, 64-bit.
2. DirectX 12 compatible GPU.
3. In order to build the Work Graphs codepath, a GPU supporting shader model 6.8 is required (comment out the USE_WORK_GRAPHS macro otherwise).
4. The WinPixEventRuntime is required to build, see instructions: https://devblogs.microsoft.com/pix/winpixeventruntime/.
5. The Microsoft.Direct3D.DXC is required to build, see https://www.nuget.org/packages/Microsoft.Direct3D.DXC/1.8.2505.32?_src=template.
6. The Microsoft.Direct3D.D3D12, version 1.616.* is required to build, see https://www.nuget.org/packages/Microsoft.Direct3D.D3D12/1.616.1?_src=template.

## How to build and run
1. `git clone --recursive https://github.com/komputeshader/KomputeRasterization.git`
2. In the case you have cloned repo without the `--recursive` flag, perform `git submodule update --init --recursive`.
3. Download https://casual-effects.com/g3d/data10/index.html#mesh3 and place it into the `KomputeRasterization/Buddha/` folder.
4. Download https://casual-effects.com/g3d/data10/index.html#mesh25 and place it into the `KomputeRasterization/powerplant/` folder.
5. Open `.sln` file with the Visual Studio.
6. If not installed already, right-click on the project and select "Manage nuGet Packages", and install the following packages:
  * WinPixEventRuntime.
  * Microsoft.Direct3D.DXC.
  * Microsoft.Direct3D.D3D12, version 1.616.*.
8. Build and run using Visual Studio.

## Papers and other resources used
* [A Parallel Algorithm for Polygon Rasterization](https://www.cs.drexel.edu/~david/Classes/Papers/comp175-06-pineda.pdf)
* [Optimizing the Graphics Pipeline with Compute](https://frostbite-wp-prd.s3.amazonaws.com/wp-content/uploads/2016/03/29204330/GDC_2016_Compute.pdf)
* Models downloaded from Morgan McGuire's [Computer Graphics Archive](https://casual-effects.com/data)
* Mesh loading is done with [Rapidobj](https://github.com/guybrush77/rapidobj)
* Mesh processing is done with [Meshoptimizer](https://github.com/zeux/meshoptimizer)
* GUI is done using the [IMGUI](https://github.com/ocornut/imgui)
