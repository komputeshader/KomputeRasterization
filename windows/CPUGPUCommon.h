#ifndef CPU_GPU_COMMON
#define CPU_GPU_COMMON

#define CULLING_THREADS_X 64
#define CULLING_THREADS_Y 1
#define CULLING_THREADS_Z 1

#define HIZ_THREADS_X 8
#define HIZ_THREADS_Y 8
#define HIZ_THREADS_Z 1

// for consoles, i.e. PS5/PS4/XBOXes, group sizes should be 64
//#define SWR_TRIANGLE_THREADS_X 64
#define SWR_TRIANGLE_THREADS_X 256
#define SWR_TRIANGLE_THREADS_Y 1
#define SWR_TRIANGLE_THREADS_Z 1

#define SWR_BIG_TRIANGLE_THREADS_X 8
#define SWR_BIG_TRIANGLE_THREADS_Y 8
#define SWR_BIG_TRIANGLE_THREADS_Z 1

#define MESHLET_SIZE 256

#define TRIANGLES_PER_THREAD ((MESHLET_SIZE) / (SWR_TRIANGLE_THREADS_X))
#define SWR_THREAD_GROUPS_Y ((MESHLET_SIZE) / (SWR_TRIANGLE_THREADS_X))

#define MAX_CASCADES_COUNT 8
#define CAMERAS_COUNT 1
#define MAX_FRUSTUMS_COUNT ((CAMERAS_COUNT) + (MAX_CASCADES_COUNT))
#define BIG_TRIANGLES_BUFFERS (2 + MAX_CASCADES_COUNT)

#define GPU_SOA_BUFFERS
#ifdef GPU_SOA_BUFFERS
#define INDICES_STRIDE 3
#endif

#define TILE_OFFSET_FLOAT 0
#define P0_WS_FLOAT3 1
#define P1_WS_FLOAT3 4
#define P2_WS_FLOAT3 7
#define N0_PACKED_UINT 10
#define N1_PACKED_UINT 11
#define N2_PACKED_UINT 12
#define C0_PACKED_UINT2 13
#define C1_PACKED_UINT2 15
#define C2_PACKED_UINT2 17
#define UV0_PACKED_UINT 19
#define UV1_PACKED_UINT 20
#define UV2_PACKED_UINT 21

// we don't need any other attributes than position and a tile offset for the depth pass
#define BIG_TRIANGLE_DEPTH_FIELDS (N0_PACKED_UINT)

#define BIG_TRIANGLE_OPAQUE_FIELDS (UV2_PACKED_UINT + 1)

// work graphs specific macros

#define USE_WORK_GRAPHS

// for consoles, i.e. PS5/PS4/XBOXes, group sizes should be 64
//#define SWR_WG_TRIANGLE_THREADS_X 64
#define SWR_WG_TRIANGLE_THREADS_X 256
#define SWR_WG_TRIANGLE_THREADS_Y 1
#define SWR_WG_TRIANGLE_THREADS_Z 1

#define WG_TRIANGLES_PER_THREAD ((MESHLET_SIZE) / (SWR_WG_TRIANGLE_THREADS_X))
#define SWR_WG_THREAD_GROUPS_Y ((MESHLET_SIZE) / (SWR_WG_TRIANGLE_THREADS_X))

// a broadcasting node dispatch is limited to 65535 groups per dimension and 2^24 - 1 groups in total
// use Y to extend the command range and reserve Z for configurations where a meshlet needs more than one triangle group
#define SWR_WG_MAX_DISPATCH_GRID_X 65535
#define SWR_WG_MAX_DISPATCH_GRID_Y (256 / (SWR_WG_THREAD_GROUPS_Y))
#define SWR_WG_MAX_COMMANDS ((SWR_WG_MAX_DISPATCH_GRID_X) * (SWR_WG_MAX_DISPATCH_GRID_Y))

#define SWR_WG_BIG_TRIANGLE_THREADS_X 8
#define SWR_WG_BIG_TRIANGLE_THREADS_Y 8
#define SWR_WG_BIG_TRIANGLE_THREADS_Z 1

#endif // CPU_GPU_COMMON
