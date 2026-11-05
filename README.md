# 《浮岛》Vulkan 自研渲染器

一个从零实现的 Vulkan 1.3 渲染器，用一座海岛漫游场景（植被、水面、昼夜光照）验证渲染框架与性能优化。

## 技术栈

- C++17、CMake、Ninja
- Vulkan 1.3（Dynamic Rendering、Synchronization2），macOS 上通过 MoltenVK 运行
- HLSL Shader，由 DXC 编译为 SPIR-V
- GLFW（窗口）、VMA（显存分配）、vk-bootstrap（初始化）、Dear ImGui（调试界面）
- 调试与性能分析：RenderDoc、Nsight Graphics（Windows），Xcode GPU Capture（macOS）

## 平台

| 平台 | 用途 |
| --- | --- |
| macOS（Apple M5 Max） | 主力开发 |
| Windows（GTX 1070 Ti） | 测试、RenderDoc / Nsight 抓帧与性能分析 |

## 构建

依赖：CMake ≥ 3.25、Ninja、Vulkan 1.3 Loader + 验证层、DXC、GLFW 3.4+（找不到时自动下载）。

```bash
cmake -S . -B build -G Ninja      # 默认 Debug，开启验证层
ninja -C build
./build/FloatingIsland            # Esc 退出
./build/FloatingIsland --frames 120 --screenshot shot.ppm   # 跑 120 帧，保存最后一帧后退出
./build/FloatingIsland --frames 600 --no-vsync              # 关闭垂直同步跑 600 帧，退出时打印各 Pass 的 GPU 耗时
```

性能基准测试（固定相机路径，用 Release 构建，结果记录在 `docs/perf-log.md`）：

```bash
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build-release
./build-release/FloatingIsland --benchmark 1500 --csv out.csv   # 每帧场景画 16 遍让 GPU 满载，报告单遍耗时
./build-release/FloatingIsland --benchmark 1500 --grass-density 4 --grass-shadows   # 加重草的负载；让草也写入阴影贴图（用于前后对比）
./build-release/FloatingIsland --benchmark 1500 --grass-density 4 --no-culling     # 关闭视锥剔除（用于前后对比）
```

操作：按住鼠标右键转视角，WASD 移动，E / Q 上升 / 下降，按住 Shift 加速；F1 显示 / 隐藏调试面板。

调试面板（Dear ImGui）：CPU 帧时间与各 Pass 的 GPU 耗时（时间戳查询）、帧时间曲线、Draw Call 与三角形数；视锥剔除开关和 Freeze（冻结剔除用的视锥，飞出去就能看到哪些区块没画）；物体列表里可以单独开关每类物体的显示和阴影（草默认不投射阴影）；可实时调节太阳方位角 / 高度角 / 强度、曝光、阴影深度偏移，以及开关垂直同步。

macOS 上用 Homebrew 安装的验证层时，需要让动态库加载器能找到它：

```bash
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib:/usr/lib"
```

## 代码结构

| 目录 | 内容 |
| --- | --- |
| `src/app` | 窗口与主循环、调试面板（`DebugUi`） |
| `src/rhi` | Vulkan 底层封装：Instance / Device / 队列 / VMA（`VulkanContext`）、交换链（`Swapchain`）、Buffer 与 Image（`Resources`）、Pipeline 构建（`Pipeline`） |
| `src/renderer` | 每帧的命令录制、同步与提交：阴影 Pass + 主 Pass + UI Pass（`Renderer`），GPU 时间戳计时（`GpuProfiler`） |
| `src/scene` | 相机（`Camera`）、程序化生成的浮空岛网格（`IslandMesh`）、基础几何体（`Primitives`）、按地形散布的植被与岩石（`Vegetation`） |
| `shaders` | HLSL Shader，构建时由 DXC 编译到 `build/shaders/*.spv`；`common.hlsli` 为共享的资源绑定定义 |

## 渲染流程（每帧）

场景：程序化浮空岛 + 低多边形松树、阔叶树、岩石与约 1.5 万丛草。每种植被一次实例化绘制（Instance Buffer 存位置 / 缩放 / 朝向 / 色调 / 风力），顶点着色器里做风吹摆动，阴影 Pass 用同一函数，所以影子跟着摆。

1. **阴影 Pass**：从太阳方向用正交投影渲染场景深度到 2048² 阴影贴图（深度偏移防止自阴影）。
2. **主 Pass**：GGX / Smith / Schlick 的金属度-粗糙度 PBR，3×3 PCF 阴影，半球环境光，输出线性 HDR 到离屏 RGBA16F 图像。
3. **合成 Pass**：全屏三角形读取 HDR 图像，背景处（alpha = 0）按视线方向画天空渐变和太阳，再做曝光和 ACES 色调映射，写入 sRGB 交换链。
4. **UI Pass**：在合成结果上叠加 ImGui（loadOp = LOAD）。

每个 Pass 前后写入时间戳；每帧的查询结果在该帧的 Fence 触发后读回，不会让 CPU 等待 GPU。

只有合成 Pass 和 UI Pass 会写交换链图像。在 Metal（MoltenVK）上，GPU 写交换链图像前要等显示器释放它，这段等待会落在合成 Pass 的计时里（关闭垂直同步时尤其明显）。所以比较优化效果时看 **GPU scene = 阴影 Pass + 主 Pass**，不看整帧。

资源绑定：Set 0 = 每帧 Uniform Buffer（相机、光源矩阵）+ 阴影贴图（比较采样器）；Push Constant = 每个物体的模型矩阵。

## 文档

- 详细设计说明：见项目设计文档
- 性能优化记录：`docs/perf-log.md`
