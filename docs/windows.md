# 在 Windows（GTX 1070 Ti）上构建和测试

这是第 20 步：在第二台机器上跑同一套 benchmark，和 Mac（M5 Max）的结果对比。
下面的命令都在 **PowerShell** 里执行。

## 1. 安装工具（一次性）

| 工具 | 说明 |
| --- | --- |
| [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/) 或 Visual Studio 2022 | 安装时勾选「使用 C++ 的桌面开发」（MSVC 编译器、Windows SDK、CMake、Ninja 都在里面） |
| [Vulkan SDK](https://vulkan.lunarg.com/sdk/home#windows) 1.3 或更新 | 包含 Vulkan 头文件、验证层和 DXC（HLSL → SPIR-V）。装完会设置 `VULKAN_SDK` 环境变量，需要重新打开终端才生效 |
| [Git for Windows](https://git-scm.com/download/win) | 克隆仓库；CMake 也用它下载 GLFW、GLM、VMA、ImGui |
| NVIDIA 显卡驱动 | 用较新的驱动（GTX 10 系列从 2022 年左右的驱动起支持 Vulkan 1.3） |

检查：

```powershell
vulkaninfo --summary     # 应能看到 GeForce GTX 1070 Ti，apiVersion 1.3.x 或更高
dxc --version            # 来自 Vulkan SDK
```

## 2. 获取代码

仓库是私有的，第一次克隆会弹出 GitHub 登录窗口（Git Credential Manager），用你自己的账号登录即可。

```powershell
git clone https://github.com/zhufengwang1997-lab/FloatingIsland.git
cd FloatingIsland
```

## 3. 构建

打开开始菜单里的 **Developer PowerShell for VS 2022**（这样 `cl`、`cmake`、`ninja` 都在 PATH 里），进入仓库目录：

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

```powershell
cmake -S . -B build -G Ninja          # Debug：带验证层，用来检查 Vulkan 用法错误
cmake --build build
```

第一次配置会从 GitHub 下载 GLFW、GLM、VMA、Dear ImGui，需要几分钟。

## 4. 先用 Debug 版确认没有验证层报错

```powershell
.\build\FloatingIsland.exe --frames 300 --day-cycle --ibl-every-frame
```

输出里应有 `GPU: NVIDIA GeForce GTX 1070 Ti` 和 `Validation layer: on`，并且没有 `[Vulkan ERROR]` 行。
有报错的话把整段输出发给我。

再打开窗口实际看一下（右键拖动转视角，WASD 移动，H 开关面板），在面板里拖动 Hour 看昼夜变化：

```powershell
.\build\FloatingIsland.exe
```

## 5. 跑标准 benchmark

关掉所有 FloatingIsland 窗口，接上电源，然后：

```powershell
powershell -ExecutionPolicy Bypass -File tools\benchmark.ps1
```

大约 10 分钟（5 项 × 3 轮）。结果会打印出来，并保存为 `build-release\benchmark-<计算机名>.md`。
Mac 上的同一套结果在 `docs/perf-log.md` 的「第 20 步」一节，把 Windows 的表格贴到它旁边即可对比。

## 6. 可选：用 RenderDoc / Nsight 抓一帧

- [RenderDoc](https://renderdoc.org/)：Launch Application 选 `build-release\FloatingIsland.exe`，运行后按 F12 抓帧，
  可以逐个 Pass 看阴影贴图、HDR 图像、IBL 的三张立方体贴图（Resource Inspector 里的 `envCube` 等）。
- [Nsight Graphics](https://developer.nvidia.com/nsight-graphics)：GPU Trace 可以看每个 Pass 的显卡单元占用，适合继续做性能优化。

## 可能遇到的问题

| 现象 | 处理 |
| --- | --- |
| CMake 报 `Could NOT find Vulkan` 或找不到 `dxc` | Vulkan SDK 装完没重开终端；或 `VULKAN_SDK` 环境变量没设置 |
| `cl` / `ninja` 不是命令 | 没有用 Developer PowerShell for VS 2022 |
| 运行时 `No Vulkan device found` 或选到了核显 | 更新 NVIDIA 驱动；笔记本上在 NVIDIA 控制面板里指定用独显 |
| 中文显示成乱码 | 终端执行 `chcp 65001` 切到 UTF-8 |
