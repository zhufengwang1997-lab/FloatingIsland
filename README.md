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

> 项目骨架搭建中，构建说明随第 2 步补充。

## 文档

- 详细设计说明：见项目设计文档
- 性能优化记录：`docs/perf-log.md`（开发中补充）
