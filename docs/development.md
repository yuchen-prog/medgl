# MedGL 开发与验证

Author: yuchen.sun

本文提供源码构建、测试和诊断入口。产品功能与基本操作见 [README](../README.md)。

## 环境要求

- Windows x64、兼容的 NVIDIA GPU 与驱动。
- Visual Studio，安装 x64 C++ 工具以及 CMake/Ninja 组件。
- 与 MSVC 和驱动兼容的 NVIDIA CUDA Toolkit。
- Node.js 20.19+ 或 22.12+，以及 npm。

本机验证组合为 CUDA 13.2.86、MSVC 19.51、NVIDIA RTX 2000 Ada 和驱动 595.79；其他组合需另行验证。`nvidia-smi` 显示的是驱动支持的 CUDA 版本，不代表已安装 Toolkit 编译器。可参考 [CUDA 下载](https://developer.nvidia.com/cuda-downloads)和 [Windows 安装说明](https://docs.nvidia.com/cuda/cuda-installation-guide-microsoft-windows/index.html)。

当前预设以 Ada `sm_89` 为目标。使用其他 GPU 时需调整 `CMAKE_CUDA_ARCHITECTURES`；本地覆盖可放在不提交的 `CMakeUserPresets.json` 中，不使用 `allow-unsupported-compiler` 绕过编译器兼容检查。

## 构建源码

从项目根目录的 PowerShell 执行。脚本通过 vswhere 发现 Visual Studio，初始化 x64 编译环境，并使用 VS 自带的 CMake/Ninja。

```powershell
.\scripts\setup-dependencies.ps1
.\scripts\build.ps1 -Preset v1-release -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2'
npm --prefix web ci
npm --prefix web run build
```

首次原生依赖安装需要网络和较长构建时间，不需管理员权限。依赖使用 vcpkg manifest 与 npm lockfile；原生依赖包括 GDCM、Boost.Beast/Asio、nlohmann/json、libpng 和 libjpeg-turbo。

仅检查主机代码，或单独验证 CUDA 环境：

```powershell
.\scripts\build.ps1 -Preset host-only
.\scripts\build.ps1 -Preset cuda-debug
.\scripts\build.ps1 -Preset cuda-release
```

`host-only` 不验证 GPU。CUDA 预设在 Toolkit 缺失时明确失败，不会自动降级；探针需完成设备查询、H2D、真实 kernel、D2H 及 1,048,576 个结果比对，但探针本身不是 VR 性能基准。

## 启动服务

```powershell
New-Item -ItemType Directory -Force .\data | Out-Null
.\scripts\start-medgl.ps1 -Port 8081
```

访问 `http://127.0.0.1:8081/`。启动器检查目录、端口、前端产物和 CUDA，`Ctrl+C` 停止服务；加 `-CheckOnly` 仅执行预检。默认配置见 [部署配置](../deployment/medgl.config.json)，相对数据路径按项目或运行包根目录解析。

加载已授权的 CT 目录：

```powershell
.\scripts\start-medgl.ps1 -DataRoot 'D:\AuthorizedCT' -Port 8081 -PngCompression fast
```

省略 `-PngCompression fast` 使用默认 PNG 压缩；二者均为无损精细帧，JPEG 预览不变。页面优先自动加载目录中第一个受支持的 CT；没有可用 CT 时直接显示合成体。首次 CT 加载失败时，先确认旧会话释放，再尝试一次合成体，界面保留明确提示；手动加载失败不自动回退，刷新序列目录不替换当前场景。CT 和合成体都失败时保留错误，不循环重试。

默认严格检查 DICOM UID。对于已确认来源、文件头和数据集实例 UID 不一致的本机研发输入，可显式添加 `-AllowInstanceUidMismatch`；它不放宽其他格式、重复实例或几何检查，不修复或改写原始文件，界面会显示兼容警告。不要把该开关作为通用导入配置。原测试数据的原因和只读验证见 [验证记录](../.spec/MedGL-v1-validation.md)。

## 前端与浏览器回归

```powershell
npm --prefix web run typecheck
npm --prefix web run lint
npm --prefix web test
npm --prefix web run test:ui
npm --prefix web run test:real
```

`test:ui` 使用模拟服务验证交互与错误恢复；`test:real` 在独立 8082 服务上使用合成 DICOM 和真实 CUDA。不能用模拟后端结果替代 GPU 验证。

真实浏览器回归要求已生成 `out/validation/synthetic-ct`。首次可单独运行以下服务生成合成序列，然后停止它，再运行测试；生成器拒绝覆盖非空目录：

```powershell
.\out\build\v1-release\medgl_server.exe --port 8082 --fixture-root out/validation/synthetic-ct --data-root out/validation/synthetic-ct --web-root web/dist
```

首次安装 Chromium：

```powershell
$env:PLAYWRIGHT_BROWSERS_PATH = Join-Path $PWD 'web\node_modules\.cache\ms-playwright'
Push-Location web
npx playwright install chromium
Pop-Location
```

`npm ci` 会移除项目依赖目录内的浏览器缓存，之后需重新安装。快速 PNG 模式回归可先设置 `$env:MEDGL_PNG_COMPRESSION = 'fast'`，完成后用 `Remove-Item Env:MEDGL_PNG_COMPRESSION` 清除。

## CLI 与诊断

合成体离屏出图和已授权目录的序列列表：

```powershell
.\out\build\v1-release\medgl_cli.exe --output .\out\validation\phantom-vr.png
.\out\build\v1-release\medgl_cli.exe --data-root 'D:\AuthorizedCT' --list
.\out\build\v1-release\medgl_cli.exe --data-root 'D:\AuthorizedCT' --series series-1 --output .\out\validation\ct-vr.png
```

离线强度/几何审计和分段资源基准：

```powershell
.\out\build\v1-release\medgl_dicom_audit.exe --data-root 'D:\AuthorizedCT' --output .\out\validation\ct-audit.json
.\out\build\v1-release\medgl_vr_benchmark.exe --data-root 'D:\AuthorizedCT' --series series-1 --frames 12 --cycles 5 --width 512 --height 512 --preset ct-bone --output .\out\validation\vr-benchmark.json
```

诊断输出不得位于输入数据目录，原始影像只读；日志与报告不输出患者姓名、ID、UID 或原始文件名。审计仍共享 GDCM 解码，不代替外部医学查看器对照。基准的 `--compare-png`、`--compare-png-fast` 支持同 RGB 配对编码与解码核验；具体条件和测量限制见 [坐标与强度验证](../.spec/MedGL-v1-correctness.md)及 [性能基线](../.spec/MedGL-v1-performance.md)。

## 独立运行包

```powershell
.\scripts\package.ps1 -Build -CudaRoot 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2'
.\scripts\test-package.ps1 -Cycles 100
```

`-Build` 会重装前端依赖；需要先完成构建，再按上述步骤安装 Chromium，最后执行包测试。产物位于 `out/packages`，不包含患者数据；目标机仍需兼容的 GPU、驱动和 VC++ v14 x64 Runtime，不需开发工具。

解压后先运行包内的 `scripts/verify-package.ps1`，再启动服务。现有 ZIP 保留较早交付版本，不含后来新增的裁剪盒和玻璃切平面；最新功能请从源码构建。许可、依赖闭包、哈希清单与验收边界见 [交付说明](../.spec/MedGL-v1-delivery.md)。

## 代码结构

| 目录 | 职责 |
| --- | --- |
| `include/medgl`、`src/core` | 数据契约、LPS 几何与 CUDA 体渲染 |
| `src/io` | GDCM CT 读取、HU 转换、PNG/JPEG 编码 |
| `src/server` | HTTP/WebSocket、预设、场景生命周期与有界调度 |
| `src/cli`、`src/diagnostics` | 离屏出图、CUDA 探针、审计与性能基准 |
| `web` | React 工作站、Three.js 玻璃工具与浏览器测试 |
| `tests`、`scripts`、`deployment` | 原生回归、构建脚本及本机交付 |

技术设计与详细验收记录统一保留在 [.spec](../.spec/MedGL-v1-design.md)。患者影像、依赖和构建产物不纳入 Git；提交前仍应检查其他位置或无扩展名的数据文件。