# drm-lab 文档

这是 **mini DRM lab** 的文档目录：从规范、任务到系统设计。代码尚未按实验实现，先把约定读完再写 V1。

## 读什么

| 文件 | 内容 |
| --- | --- |
| [task.md](task.md) | V1 / V2 / V3 实验清单、每项思路、验收标准 |
| [spec.md](spec.md) | 软件架构与系统设计（分层、对象、控制流、恢复） |
| [code_review.md](code_review.md) | 开发规范与代码审查（Google C++，C++17，4 空格，`.cpp`） |

建议顺序：`code_review.md`（怎么写）→ `spec.md`（怎么拆）→ `task.md`（做什么）。实现某一版时，把 `task.md` 里对应条目勾上。

## 实验在做什么

用户态程序经 **libdrm** 操作 **KMS**：把 CPU 可写的 dumb buffer 送给 CRTC 扫描到显示器。不依赖桌面环境，也不走 GBM/GL（V1–V3 范围外）。

```text
V1  纯色 / 渐变     legacy SetCrtc，一块缓冲
V2  动画不撕裂      双缓冲 + PageFlip + poll
V3  属性事务        Atomic commit，含故意失败
```

三版共用同一套封装（`DrmDevice`、管道枚举、dumb、fb），后一版只加能力，不推翻前一版命令行。

## 运行环境（实现后）

- Linux + 可用的 `/dev/dri/card0`（或 `--device=` 指定）。
- 用户能成为 DRM master：加入 `video` 组，且尽量不要让 GNOME/KWin 等占着同一块屏。典型做法是切到空闲 VT，或 SSH 到机器上跑。
- 构建：CMake preset（见仓库根目录 `CMakePresets.json`），C++17，链接 libdrm。

规划中的入口：

```text
drm-lab --lab=v1 [--color=red|green|blue|gradient]
drm-lab --lab=v2
drm-lab --lab=v3 [--fail=plane-size|bad-fb]
```

进程退出应恢复原显示模式。若实验后黑屏，用其它 VT 或 SSH 结束进程并检查 Restore 是否生效。

## 仓库布局（规划）

与 `spec.md` 一致，落地 V1 时创建源码目录：

```text
include/drm_lab/   封装头文件
src/               封装 + lab_v1/v2/v3 + main.cpp
docs/              本目录
```

当前仓库仍是 CMake 骨架加占位 `main.cpp`，以本文档为准开始第一版实现。
