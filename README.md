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
./build/FloatingIsland            # Esc 退出；--frames N 跑 N 帧后自动退出
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
| `src/rhi` | Vulkan 底层封装：Instance / Device / 队列 / VMA（`VulkanContext`）、交换链（`Swapchain`）、Buffer 与 Image（`Resources`） |
| `src/renderer` | 每帧的命令录制、同步与提交（`Renderer`） |
| `src/scene` | 相机（`Camera`）、程序化生成的浮空岛网格（`IslandMesh`） |
| `shaders` | HLSL Shader，构建时由 DXC 编译到 `build/shaders/*.spv` |

## 文档

- 详细设计说明：见项目设计文档
- 性能优化记录：`docs/perf-log.md`（开发中补充）
