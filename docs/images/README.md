# 产品截图说明

Author: yuchen.sun

README 截图选自 2026-10-19 的实际 CUDA 浏览器回归，不使用患者影像，也不对渲染结果做合成或美化。两图均为 1440 × 960 的浏览器原始截图。

| 图片 | 输入与来源 |
| --- | --- |
| [工作站总览](workstation.png) | 程序生成的 64 层 CT DICOM；来源为 `web/tests/integration/real-backend.spec.ts` 的 `real-cuda-desktop.png` |
| [玻璃切平面](glass-plane.png) | 程序生成的 Synthetic phantom；来源为 `web/tests/integration/clip-plane.spec.ts` 的 `clip-plane-desktop.png`，图中渲染尺寸为 50% |

原始测试图片位于已忽略的 `web/test-results-real/`。运行 `npm --prefix web run test:real` 可重新生成；更新文档图片前应确认测试仅使用合成输入，并等待精细帧完成。

这些图展示实际界面、裁切工具和渲染链路，不代表真实解剖细节、临床图像质量或性能承诺。请勿用包含患者影像或标识的截图替换。