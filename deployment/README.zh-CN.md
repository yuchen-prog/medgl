# MedGL 本机研发包

Author: yuchen.sun

这是 Windows x64、localhost、单用户普通 VR 研发包，不是临床软件或远程服务。包内不含患者数据，不需要 Node.js、Visual Studio 或 vcpkg 即可运行；构建仍需要它们。目标机必须具备兼容的 NVIDIA GPU/驱动及 Microsoft Visual C++ v14 x64 Runtime。当前 CUDA 构建目标为 sm_89，在其他GPU/驱动上必须重新验收。

## 启动

在包目录打开 Windows x64 PowerShell 5.1 或更新版本，先校验完整性，再指定外部 DICOM 目录：

```powershell
.\scripts\verify-package.ps1
.\scripts\start-medgl.ps1 -DataRoot 'D:\authorized-ct' -CheckOnly
.\scripts\start-medgl.ps1 -DataRoot 'D:\authorized-ct'
```

配置位于 `deployment/medgl.config.json`，相对 dataRoot 始终相对包根目录，不随当前工作目录变化。默认端口8081、PNG default、严格UID检查。参数可覆盖配置；启动后访问 http://127.0.0.1:8081/，Ctrl+C停止。页面先加载合成体，真实CT需手动选序列加载。不自动打开防火墙或提权。

快速无损PNG和已知数据的只读UID兼容模式必须显式开启：

```powershell
.\scripts\start-medgl.ps1 -DataRoot 'D:\authorized-ct' -Port 8091 -PngCompression fast -AllowInstanceUidMismatch
```

UID兼容不是修复或脱敏，不放宽其他输入校验。不要将DICOM放入web目录。默认data目录仅含说明文件，无患者数据，可直接启动查看合成体；真实CT可指定外部目录。端口占用时换端口，不终止其他程序。

系统若阻止脚本运行，应按组织策略审阅并授权本地脚本，不必全局关闭执行策略。下载包可能有来源标记；此包未代码签名。

## 内容与边界

`bin` 包含服务、CLI、CUDA探针、HU审计、性能工具及解析到的非系统DLL；`web` 为生产静态资源，IBM Plex字体本地打包，不请求外部字体服务。`metadata` 记录版本、依赖锁定输入和源码输入哈希；`licenses` 附原始第三方许可和字体许可。系统依赖名单另列，系统DLL/驱动不随包复制；运行库缺失时需自行从官方渠道安装，不由脚本提权安装。

哈希清单检测传输损坏或误改，不证明发布者身份。修改配置会使原包完整性校验失败，先校验原包再保存本地配置；也可用参数覆盖。构建可追溯不等于ZIP逐字节可复现，没有承诺时间戳、编译器产物一致。

许可文件收集不等于法律合规审核。完整第三方/专有运行库再分发审查、签名、多小时资源验证、OOM恢复和外部医学查看器对照尚未完成。只用于获授权的本机研发，不应直接对外分发或开放局域网。源码、安装步骤和验证范围见项目仓库文档。