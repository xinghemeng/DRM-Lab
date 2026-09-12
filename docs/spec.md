# drm-lab 软件架构与系统设计

本文描述 mini DRM lab 的系统边界、对象模型、模块划分和 V1–V3 的控制流。实现必须能映射回这里的模块，而不是把所有 ioctl 堆进 `main.cpp`。

实验步骤与验收见 [`task.md`](task.md)。编码约定见 [`code_review.md`](code_review.md)。

---

## 1. 目标与非目标

### 1.1 目标

- 提供一个**可独立运行**的用户态程序，直接操作 DRM/KMS，验证 scanout 路径。
- 用同一套核心封装，分三阶段暴露能力：legacy modeset → page flip → atomic。
- 让「C++ 对象」和「内核 DRM 对象」一一对应，便于画图讲解。
- 资源（fd、dumb、fb、blob、atomic req）全部 RAII；失败路径不泄漏；退出尽量恢复显示。

### 1.2 非目标

- 不是 Wayland/X11 合成器，不合成多客户端。
- V1–V3 不引入 GBM、EGL、OpenGL、Vulkan、硬件解码。
- 不追求多显示器、热插拔、HDR、overlay 合成、plane 旋转缩放的完整覆盖（V3 只用 primary plane）。
- 不使用 C++ 异常；不做到桌面级的 VT 切换与 logind 会话完备性（可后续加，但不阻塞 V1）。

### 1.3 约束

| 项 | 选择 |
| --- | --- |
| 语言 | C++17 |
| 风格 | Google C++，4 空格，源文件 `.cpp` |
| 显示 API | libdrm（`xf86drm` / `xf86drmMode`） |
| 缓冲 | dumb buffer + CPU 绘制 |
| 进程模型 | 单进程、单线程事件循环 |
| 设备 | 默认 `/dev/dri/card0`，一块 connected connector |

---

## 2. 系统上下文

```text
┌─────────────────────────────────────────┐
│  drm-lab 进程（用户态）                  │
│  Lab V1 / V2 / V3  +  drm_lab 封装      │
└──────────────────┬──────────────────────┘
                   │ open / ioctl / mmap / poll
                   ▼
┌─────────────────────────────────────────┐
│  libdrm                                 │
│  drmModeGetResources / SetCrtc /        │
│  PageFlip / AtomicCommit / HandleEvent  │
└──────────────────┬──────────────────────┘
                   │ DRM ioctl
                   ▼
┌─────────────────────────────────────────┐
│  内核 DRM 核心 + 厂商 KMS 驱动           │
│  GEM dumb、CRTC、plane、connector       │
└──────────────────┬──────────────────────┘
                   │ scanout
                   ▼
            显示器 / 面板
```

`/dev/dri/card0` 是该 GPU/显示控制器的**主节点**：modeset、GEM、event 都走这一个 fd。同卡的 `renderD128` 不能设置模式，本项目不使用。

与桌面环境互斥：内核同一时刻通常只允许一个 DRM master 做 modeset。本程序需要成为 master。因此系统设计为 **独占显示管道的实验工具**，不是可与 GNOME 共存的 overlay 客户端。

---

## 3. DRM 对象模型（设计要对齐的现实）

硬件扫描链路（逻辑上）：

```text
Framebuffer (GEM + 格式 + pitch)
        ▲
        │ 绑定 FB_ID
     Plane (V1/V2 隐式 primary；V3 显式)
        ▲
        │ 扫描
      CRTC  ←── MODE（时序）
        ▲
        │ possible_crtcs
     Encoder
        ▲
     Connector ── 物理输出口 ── 显示器
```

用户态标识一律是 **uint32 id**（加上本进程持有的 fd）。封装类持有 id 与从 libdrm 取回的只读快照，不在对象里缓存过期的 `drmModeRes` 指针。

| 内核对象 | 本项目类型（规划） | 职责 |
| --- | --- | --- |
| device fd | `DrmDevice` | open/close、master、client cap、ioctl 入口、poll |
| resources / connector | `Connector` 快照含于 `DisplayPipeline` | 连接状态、mode、encoder id |
| encoder | 含于 `DisplayPipeline` | `possible_crtcs` |
| CRTC | 含于 `DisplayPipeline` | crtc id、mode；legacy SetCrtc |
| dumb GEM | `DumbBuffer` | create/map/unmap/destroy，提供映射与 pitch |
| framebuffer | `Framebuffer` | `AddFB2` / `RmFB`，持有 fb id |
| page flip | `FlipLoop` | 双缓冲下标、pending、handler、FPS |
| atomic | `AtomicUpdate` | 查找 property、blob、commit |

**所有权规则：**

- `DrmDevice` 拥有 fd，生命周期最长。
- `DumbBuffer` / `Framebuffer` 拥有内核对象，析构时释放；`Framebuffer` 不得长于其依赖的 `DumbBuffer`。
- `drmModeRes`、`drmModeConnector` 等 libdrm 堆对象在枚举函数内复制出需要的 id/mode 后立即 free，不把原始指针存进长期对象。
- page flip / atomic NONBLOCK 期间，被扫描的 `Framebuffer` 必须仍然存活。

---

## 4. 软件分层

```text
┌──────────────────────────────────────────────┐
│  app：main + lab 选择                         │
│  lab_v1 / lab_v2 / lab_v3                    │
│  （场景：填色、动画、非法 commit）            │
├──────────────────────────────────────────────┤
│  drm_lab：C++ 封装（无异常，错误码返回）      │
│  device / pipeline / dumb_buffer / fb        │
│  flip_loop / atomic_update / draw            │
├──────────────────────────────────────────────┤
│  libdrm + libc（open, mmap, poll, clock）    │
└──────────────────────────────────────────────┘
```

- **Lab 层**：只描述「这一版实验要发生什么」（选红、画移动矩形、构造非法 SRC）。不直接散落 ioctl。
- **drm_lab 层**：唯一允许调用 `drmMode*` 与 DRM ioctl 的地方。头文件放 `include/drm_lab/`，实现放 `src/`。
- Lab 层不得 `#include <xf86drmMode.h>`（防止绕过封装）。`main.cpp` 也不包含 DRM 头。

错误模型：封装函数返回 `bool` 或小型 `Status`（`ok` + `errno` + 静态原因字符串）。日志打到 `stderr`，带上 fd / id / errno。不使用异常。

---

## 5. 目录与可执行文件

规划（随 V1 落地时创建，不必一次写齐 V3）：

```text
include/drm_lab/
  device.h
  pipeline.h          # 选 connector/encoder/crtc/mode
  dumb_buffer.h
  framebuffer.h
  flip_loop.h         # V2
  atomic_update.h     # V3
src/
  device.cpp
  pipeline.cpp
  dumb_buffer.cpp
  framebuffer.cpp
  draw.cpp
  flip_loop.cpp
  atomic_update.cpp
  lab_v1.cpp
  lab_v2.cpp
  lab_v3.cpp
  main.cpp
```

**一个二进制 `drm-lab`**，用命令行选实验，避免三套重复的打开设备逻辑：

```text
drm-lab --lab=v1 [--device=/dev/dri/card0] [--color=red|green|blue|gradient]
drm-lab --lab=v2 [--fps=60]
drm-lab --lab=v3 [--atomic-flip] [--fail=plane-size|bad-fb]
```

`--lab` 缺省为 `v1`。未知参数报错退出，返回码非 0。

CMake：`add_executable(drm-lab ...)` 链接 `libdrm`；`CMAKE_CXX_STANDARD 17`；警告 `-Wall -Wextra -Werror`。

---

## 6. 核心模块设计

### 6.1 `DrmDevice`

- `Open(const char* path)`：`O_RDWR | O_CLOEXEC`。
- `int fd() const`：借用，不移交。
- 析构 `close`。禁止拷贝；可移动。
- `EnableAtomic()`：V3 设置 client cap。
- `WaitReadable(int timeout_ms)`：内部 `poll`。
- `DispatchEvents(...)`：封装 `drmHandleEvent`。

### 6.2 `DisplayPipeline`

一次枚举的结果（值类型快照）：

- `connector_id`、`encoder_id`、`crtc_id`
- `drmModeModeInfo mode`
- 进入前的 CRTC 状态（原 fb id、mode、x/y），供 `Restore()` 使用

`FindConnected(const DrmDevice&)`：实现 `task.md` V1「DRM 设备」整段。找不到 connected 输出则失败，由 lab 打印「未接显示器」。

### 6.3 `DumbBuffer` / `Framebuffer`

`DumbBuffer::Create(device, width, height, bpp=32)` → handle / pitch / size / mapped pointer。

绘制只通过：

- `void* data()`
- `uint32_t pitch() const`、`width()`、`height()`

不在 `DumbBuffer` 里写死 XRGB 填充，避免封装层假定像素格式。填色放在 `draw` 模块，由 lab 调用。

`Framebuffer::CreateFromDumb(device, dumb, format=XRGB8888)` → `fb_id()`。

销毁顺序由 C++ 成员顺序保证：lab 里 `Framebuffer` 字段放在对应 `DumbBuffer` **之后**声明（先析构 fb 再 destroy dumb），或使用明确的 `Reset()`。

### 6.4 Legacy modeset（V1/V2 共用）

`SetCrtc(device, pipeline, fb)` → `drmModeSetCrtc`。

仅在「需要改时序或第一次点亮」时调用。V2/V3 的每帧路径禁止走这里。

### 6.5 `FlipLoop`（V2）

状态：

- 两块 fb 的下标（缓冲由 lab 持有）
- `front`、`back`、`flip_pending`
- 帧计数与上一帧时间戳

循环：

```text
draw(back)
PageFlip(back, EVENT)
flip_pending = true
while pending: poll → HandleEvent
swap front/back
```

handler 只置标志与时间戳，不在回调里绘制。停止时先清 `running`，等 pending flip 结束再析构。

### 6.6 `AtomicUpdate`（V3）

- 按对象类型缓存 `name → property_id` 表（crtc / connector / plane）。
- `CreateModeBlob(mode)`，析构时 destroy blob。
- `FindPrimaryPlane(crtc_id)`。
- Modeset 与 Flip 分成两个函数，避免一个万能 `Commit` 塞全部 flags：
  - `CommitModeset(...)`：`ALLOW_MODESET`，可阻塞。
  - `CommitFlip(fb_id, ...)`：`NONBLOCK` + page flip event。
- `SetSrc(x, y, w, h)` 内部按 16.16 左移，lab 传像素值。另提供 `SetSrcRaw` 给非法实验。

### 6.7 `draw`

共享像素寻址：`FillSolid` / `FillGradient` / `BlitRect`。这是唯一集中「按 pitch 写 XRGB8888」的模块，不属于 ioctl 层。

---

## 7. 三版控制流

### 7.1 V1

```text
Open device
FindConnected pipeline
Create dumb + mmap
Fill color / gradient    // 按 pitch
Create fb
SetCrtc(fb, mode)
wait stdin or signal
Restore + destruct
```

无 poll，无第二块 buffer。第一次必须 `SetCrtc`；之后改 mmap 内容就会出现在屏上（CRTC 持续扫描同一 fb）。换色不必每次 `SetCrtc`。这是「为什么 mmap 能影响显示」的现场。

### 7.2 V2

```text
V1 的点亮（fb[0]）
Create 第二块 dumb+fb
loop:
    DrawScene(back, frame)
    PageFlip(back)
    WaitEvent
    swap
    UpdateFps
until signal
Restore
```

SIGINT / SIGTERM：只写 `sig_atomic_t` 退出标志，不在信号处理里调 DRM。

### 7.3 V3

```text
Open + EnableAtomic + UniversalPlanes
FindConnected + FindPrimaryPlane
Create 两块 dumb+fb + mode blob
CommitModeset(fb0)           // 等价 V1
loop (若 atomic-flip):
    Draw(back)
    CommitFlip(back)         // 等价 V2
    WaitEvent
    swap
可选：FailExperiment()       // TEST_ONLY + 记录 errno
Restore: 恢复或 ACTIVE=0；destroy blob/fb/dumb
```

`--fail=` 走单独分支，不破坏主验收。

---

## 8. 绘制与像素格式

V1–V3 统一 **XRGB8888**。小端内存布局为 `B, G, R, X`，像素字：

```text
pixel = (r << 16) | (g << 8) | b;
row = data + y * pitch;
((uint32_t*)row)[x] = pixel;   // pitch 按 4 字节对齐时
```

---

## 9. 并发、信号与时序

- **单线程**：绘制、ioctl、poll 同线程。不把 fd 分给第二个线程。
- **禁止**在 scanout 的 front buffer 上绘制。
- 时钟：`clock_gettime(CLOCK_MONOTONIC, ...)` 统计 FPS。
- 目标 FPS 不超过 `mode.vrefresh`。用 flip 完成作为节拍，不用 `sleep` 模拟 VBlank。
- 信号处理只写 `volatile sig_atomic_t`。

---

## 10. 恢复与安全

进程结束必须尽量：

1. 等待 in-flight flip/commit。
2. 若保存了原 CRTC 状态，legacy 用 `SetCrtc` 设回；atomic 用一次 modeset commit 设回或 `ACTIVE=0`。
3. RmFB、destroy dumb、munmap、destroy blob、close fd。

实验机若只接一块屏，恢复失败会导致 TTY 黑屏。**Restore 从 V1 起就是设计要求**，不是 V3 才补的补丁。

非法 atomic 实验只提交清单中的两类错误，避免把显示器控制器置于需重启的状态。

---

## 11. 日志与可观测性

统一前缀，便于和 kernel log 对照：

```text
[drm-lab] open /dev/dri/card0 fd=3
[drm-lab] connector=41 connected modes=12 preferred=1920x1080
[drm-lab] encoder=40 crtc=38
[drm-lab] dumb handle=1 pitch=7680 size=8294400
[drm-lab] fb=32
[drm-lab] SetCrtc ok
[drm-lab] fps=59.8
[drm-lab] atomic commit failed errno=22 (Invalid argument)
```

V3 失败实验的 kernel log 由实验者手动保存（`dmesg` / `journalctl -k`），程序不依赖 root 读内核日志。

---

## 12. 演进规则

- V1 合入标准：能点亮，且已有 `DrmDevice` / `DisplayPipeline` / `DumbBuffer` / `Framebuffer`；lab 代码不再直接 ioctl。
- V2 只新增 `FlipLoop` 与第二块缓冲，不改 dumb 的语义。
- V3 只新增 cap、plane、property、atomic commit；保留 `--lab=v1|v2` 作为回归。
- 下一阶段若引入 GBM/EGL，新建模块，不把 GL 头文件塞进现有 `DumbBuffer`。

模块边界以「能否单独讲清对应的内核对象」为准：讲不清的函数说明切分还不够。
