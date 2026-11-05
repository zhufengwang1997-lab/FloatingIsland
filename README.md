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
```

操作：按住鼠标右键转视角，WASD 移动，E / Q 上升 / 下降，按住 Shift 加速。

macOS 上用 Homebrew 安装的验证层时，需要让动态库加载器能找到它：

```bash
export DYLD_FALLBACK_LIBRARY_PATH="/opt/homebrew/lib:/usr/local/lib:/usr/lib"
```

## 代码结构

| 目录 | 内容 |
| --- | --- |
| `src/app` | 窗口与主循环 |
| `src/rhi` | Vulkan 底层封装：Instance / Device / 队列 / VMA（`VulkanContext`）、交换链（`Swapchain`）、Buffer 与 Image（`Resources`）、Pipeline 构建（`Pipeline`） |
| `src/renderer` | 每帧的命令录制、同步与提交：阴影 Pass + 主 Pass（`Renderer`） |
| `src/scene` | 相机（`Camera`）、程序化生成的浮空岛网格（`IslandMesh`）、基础几何体（`Primitives`） |
| `shaders` | HLSL Shader，构建时由 DXC 编译到 `build/shaders/*.spv`；`common.hlsli` 为共享的资源绑定定义 |

## 渲染流程（每帧）

1. **阴影 Pass**：从太阳方向用正交投影渲染场景深度到 2048² 阴影贴图（深度偏移防止自阴影）。
2. **主 Pass**：GGX / Smith / Schlick 的金属度-粗糙度 PBR，3×3 PCF 阴影，半球环境光，ACES 色调映射，输出到 sRGB 交换链。

资源绑定：Set 0 = 每帧 Uniform Buffer（相机、光源矩阵）+ 阴影贴图（比较采样器）；Push Constant = 每个物体的模型矩阵。

## 文档

- 详细设计说明：见项目设计文档
- 性能优化记录：`docs/perf-log.md`（开发中补充）
