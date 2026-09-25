# drm-lab 实验任务

本清单是实验的执行与验收标准。实现时对照 [`spec.md`](spec.md) 的模块划分，编码遵守 [`code_review.md`](code_review.md)。

三版必须**按顺序**做：V1 打通 scanout，V2 在同一条管道上加时间轴，V3 把同一件事改成 property 事务。每一版都要能独立运行、独立验收，不能把 V2 的 page flip 提前塞进 V1。

运行前提：进程能成为 DRM master（通常无图形会话占用这块屏，用户在 `video` 组或具备相应权限）。默认设备 `/dev/dri/card0`。

---

## V1：纯色画面

目标：用 CPU 填一块 dumb buffer，经 legacy KMS 点亮屏幕。这是整条 lab 的地基。

### DRM 设备

- [ ] `open("/dev/dri/card0")`
- [ ] 获取 DRM resources
- [ ] 枚举 connector
- [ ] 找到 connected connector
- [ ] 读取 connector 支持的 mode
- [ ] 选择 preferred mode
- [ ] 获取 encoder
- [ ] 获取可用 CRTC

#### 思路

先把「显示管道」找齐，再碰缓冲。顺序不要反：没有 connector/mode/CRTC，dumb buffer 即使创建成功也无处可挂。
V1 整条路（心里先有图）

  Open device                    
  FindConnected pipeline         
  Create dumb + mmap
  Fill color / gradient
  Create fb
  SetCrtc(fb, mode)
  wait，然后 Restore + 析构

  对应硬件扫描链：

  进程 fd（card0）
      → dumb handle → fb_id
          → CRTC（按 mode 扫像素）
              → encoder
                  → connector（HDMI / eDP / DP）
                      → 屏

1. `open` 必须 `O_RDWR`（modeset 与 mmap 都需要写）。同时加 `O_CLOEXEC`。失败时打印 `errno`：`ENOENT` 是没这节点，`EACCES` 是权限，`EBUSY`/后续 ioctl 失败才更像被合成器占用。
2. `drmModeGetResources` 拿到的是 **id 列表快照**，不是硬件的实时镜像。立刻把 connector id 拷出来，用完 `drmModeFreeResources`，不要把 `drmModeResPtr` 存进长期对象。
3. 枚举 connector 时以 `connection == DRM_MODE_CONNECTED` 为准。`disconnected` 和 `unknown` 都跳过。多头输出时 V1 **只取第一个 connected**，避免过早处理多 CRTC。
4. mode 数组在 connector 对象上。preferred 看 `DRM_MODE_TYPE_PREFERRED`；没有则取 `modes[0]`。记下 `hdisplay`、`vdisplay`、`vrefresh`、`name`，后续 dumb 的宽高必须与此一致。
5. encoder：优先 `connector->encoder_id`；为 0 时遍历 `encoders[]`。CRTC 用 `encoder->crtc_id`（若当前已有绑定）或扫 `possible_crtcs` 位图，对照 `resources->crtcs[i]`。V1 选**一个**能驱动该 encoder 的 CRTC 即可。
6. 建议在封装里做成一次 `FindConnected()`，返回 `{connector_id, encoder_id, crtc_id, mode}`。`main`/lab 不要自己套四层 for 循环。

常见坑：拿了 HDMI connector 却用 LVDS 的 CRTC；mode 来自 A 头却 SetCrtc 到 B 头；resources 释放后再解引用 connector 里的指针。

### Dumb Buffer

- [ ] 创建 dumb buffer
- [ ] 获取 handle
- [ ] 获取 pitch
- [ ] 获取 size
- [ ] `mmap` buffer
- [ ] CPU 填充 RGB/XRGB 数据
- [ ] 创建 framebuffer
- [ ] 调用 `drmModeSetCrtc`

#### 思路

Dumb 是「让 CPU 也能写的 GEM 对象」。Framebuffer 才是 KMS 能扫描的「带格式的视图」。两者不是同一个 id。

1. `DRM_IOCTL_MODE_CREATE_DUMB`：宽高用 mode 的 `hdisplay`/`vdisplay`，`bpp = 32`。成功后记下 `handle`、`pitch`、`size`。`pitch` **不一定等于** `width * 4`，填充时必须按 pitch 走。
2. `DRM_IOCTL_MODE_MAP_DUMB` 得到的是相对 drm fd 的 **offset**，再 `mmap(fd, size, offset)`。改的是这块 GEM 的 CPU 可见映射，不是「一块匿名堆内存碰巧叫 buffer」。
3. 像素格式与后续 `drmModeAddFB2` 一致：本项目统一 **XRGB8888**。按行写入：`row = map + y * pitch`，再写 `uint32_t` 像素。切勿假设整块是连续 `width*height` 个像素而无 padding。
4. `drmModeAddFB2`（或 `AddFB`）把 `handle` + 宽高 + pitch + 四cc 登记成 `fb_id`。CRTC/plane 认的是 fb_id，不认 dumb handle。
5. `drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &connector_id, 1, &mode)` 是 V1 的「点亮」：绑 fb、设时序、接通 connector。这是 **modeset**，可能熄灭重亮一次，不要当每帧 API 用。
6. 析构顺序：先 `RmFB`，再 unmap，再 `DESTROY_DUMB`，最后 `close(fd)`。错误返回路径同样走这套，见 `code_review.md` 专项。

### 显示验证

- [ ] 显示纯红色
- [ ] 显示纯绿色
- [ ] 显示纯蓝色
- [ ] 显示简单渐变

#### 思路

验证的是「像素公式 + scanout」而不是动画。同一块 fb 上改 mmap 内容即可换色（CRTC 持续扫描这块 fb），不必每次 `SetCrtc`。建议命令行 `--color=red|green|blue|gradient`，默认红，停留到 Ctrl+C 或按回车，便于肉眼确认。

- 红 `(0xFF, 0, 0)`、绿、蓝各做一次，排除通道填反（BGR vs RGB）。
- 渐变用水平或垂直 `r = x * 255 / (w-1)` 一类公式，确认 pitch 正确：若按 `width*4` 而实际 pitch 更大，画面会错位/斜纹。
- 若三种纯色都不对但渐变「有图像」，优先查四cc 与字节序；若纯色对、渐变花，优先查 pitch。

### 需要真正理解

- [ ] `/dev/dri/card0` 是什么
- [ ] connector 是什么
- [ ] encoder 是什么
- [ ] CRTC 是什么
- [ ] framebuffer 是什么
- [ ] pitch/stride 是什么
- [ ] 为什么 `mmap` 后修改内存能影响显示
- [ ] DRM ioctl 的基本工作方式

#### 思路

不要只背定义，用 V1 跑通后的 id 对照硬件：

| 概念 | 用实验回答 |
| --- | --- |
| card0 | 打开后所有 ioctl 都走这个 fd；`renderD*` 做不了 SetCrtc。它是显示控制器的**主节点**，不是「一张显卡的帧缓冲文件」。 |
| connector | 物理口（HDMI/eDP/DP）。`connected` 表示后面有 sink。没有它就没有 mode。 |
| encoder | connector 与 CRTC 之间的信号转换。用户态很少直接编程它，但选 CRTC 必须看 `possible_crtcs`。 |
| CRTC | 扫描器：按 mode 时序从 fb 读像素送到 encoder。一块 CRTC 同时只扫一个（主）fb。 |
| framebuffer | KMS 眼里的「可扫描图像」：GEM handle + 格式 + pitch + 宽高。 |
| pitch | 一行占用的字节数，含对齐。显存扫描按 pitch 跨行，不是按 `width*bpp`。 |
| mmap 为何能上屏 | dumb 的 CPU 映射与 scanout 读的是**同一块 GEM**。SetCrtc 之后硬件周期性 DMA/扫描这块内存。 |
| ioctl | 用户态填结构体，经 drm 核心进驱动。libdrm 只是 ioctl 包装。失败看返回值与 `errno`，不是异常。 |

画一张图：`进程 fd → dumb handle → fb_id → CRTC → encoder → connector → 屏`，把程序里打印的数字标上去。这张图就是 V1 验收的「对应关系」。

### V1 验收

- [ ] 程序可以独立运行
- [ ] 不依赖桌面环境
- [ ] 能成功改变屏幕内容
- [ ] 能画出代码与 DRM 对象对应关系

#### 思路

- 「独立运行」：一个二进制，SSH 或 VT 启动即可，不链 GTK/Qt、不起 Wayland。
- 「不依赖桌面」：在无合成器、或切到未占用该 CRTC 的 VT 上验证。桌面仍占 master 时失败是预期，应打清晰错误，而不是视为 V1 未完成。
- 「改变屏幕内容」：肉眼看到纯色/渐变，且与 `--color` 一致。
- 「对应关系」：stderr 打印 connector/encoder/crtc/fb/handle/pitch；口头或笔记能画出上一节的链。缺日志则验收不通过。
- 退出应尽量恢复原 CRTC。黑屏可 SSH 杀进程，但 Restore 是设计要求，不是可选项。

---

## V2：Double Buffer + Page Flip

目标：前台扫描、后台绘制，用 page flip 在 VBlank 换 fb，做出不撕裂的动画。

### 功能

- [ ] 创建两个 framebuffer
- [ ] 当前 buffer 用于 scanout
- [ ] 后台 buffer 由 CPU 绘制
- [ ] 调用 `drmModePageFlip`
- [ ] 使用 `poll()` 等待事件
- [ ] 注册 page flip event handler
- [ ] 每次 VBlank 后交换 buffer

#### 思路

V1 的「改同一块内存」在动画里会撕裂：扫描可能读到半帧新半帧旧。V2 用**两块完整 fb**，CRTC 一次只扫其中一块。

1. 两块 dumb + 两块 fb，尺寸/格式与 V1 相同。先 `SetCrtc(fb[0])` 点亮（仍用一次 legacy modeset），之后**禁止每帧 SetCrtc**。
2. 约定 `front` = 正在 scanout，`back` = 正在 CPU 画。画完 `back` 后 `drmModePageFlip(fd, crtc_id, back_fb, DRM_MODE_PAGE_FLIP_EVENT, user_data)`。
3. Page flip **不拷贝像素**。它只改 CRTC 的扫描目标到另一个 fb_id。内核等到 VBlank 再切，避免行中途换缓冲。
4. `PAGE_FLIP_EVENT` 会在 drm fd 上变成可读。`poll(fd, POLLIN)` 等到后 `drmHandleEvent`。handler 里只标记 `flip_pending = false` 并记时间戳，不要在回调里画下一帧（保持单线程、逻辑清晰）。
5. 一次 flip 未完成时不要再提交第二次（`EBUSY`）。状态机：`idle → submit → pending → event → swap index → idle`。
6. 退出时等 pending flip 结束再 RmFB，否则可能 `EBUSY` 或内核仍引用该 fb。

`user_data` 传指向 `FlipLoop` 的指针时，必须保证 handler 触发时对象仍在。

### 动态效果

- [ ] 实现颜色变化动画
- [ ] 实现移动矩形
- [ ] 控制固定 FPS
- [ ] 统计实际帧率

#### 思路

先做全屏颜色周期（HSV 或 RGB 相位），确认 flip 路径稳定；再在背景上画移动矩形，确认没有「两条残影」（那是画到了 front 或 pitch 错误）。

- 每帧**整块**重画 back（清背景再画矩形），不要在未清的 back 上叠画。双缓冲下 back 是「上一帧的上一帧」，不是空白。
- FPS：以 flip 完成时间为节拍，目标不超过 `mode.vrefresh`。不要 `sleep(16ms)` 代替 VBlank，那会和扫描相位错开，统计失真。
- 实际帧率：`N` 帧平均 `N / Δt_monotonic`。stderr 每秒打一次。若稳定等于 panel 刷新率，说明被 VBlank 卡住了，这是好事。若远低于刷新率，先查绘制是否过重、是否在 pending 时忙等。

### 学习内容

- [ ] `poll`
- [ ] DRM event
- [ ] VBlank
- [ ] Page Flip
- [ ] 双缓冲
- [ ] tearing
- [ ] frame timing

#### 思路

| 概念 | 用实验回答 |
| --- | --- |
| poll | drm fd 在 event 到来前会阻塞；不要空转 `drmHandleEvent`。它等的是「内核有事件」，不是「画完了」。 |
| DRM event | 内核往该 fd 写控制块；`drmHandleEvent` 解出 page flip/vblank 回调。与普通 read 文件不同。 |
| VBlank | 一帧扫描结束、下一帧开始前的间隙。此时切换扫描地址才不会在一帧里混两块缓冲。 |
| Page Flip | 预约「下一个 VBlank 把 CRTC 指向另一个 fb」。不是 blit，不是 memcpy。 |
| 双缓冲 | 扫描与绘制分离。少一块就会要么撕裂要么等扫描（单缓冲）。 |
| tearing | 同一帧画面里出现新旧两截。V1 动画式改内存最容易看到；对比 V2 可演示。 |
| frame timing | 一帧 = 绘制 + 等待 VBlank。CPU 再快也快不过刷新率，除非 missed vsync（绘制超时，会掉帧）。 |

可选对照实验（不必进主程序）：故意在 front 上画移动矩形，观察撕裂，再改回 back。理解会比只看文档深。

### V2 验收

- [ ] 连续运行无闪烁
- [ ] 能解释 page flip 为什么不是简单 memcpy
- [ ] 能说明 VBlank 与 tearing 的关系

#### 思路

- 「无闪烁」：长时间跑矩形来回，无黑帧、无撕裂、无每帧闪一下（后者常是误用 SetCrtc）。
- 「不是 memcpy」：两块 GEM 地址不同，flip 只改扫描指针；可用 `fb_id` 在日志里交替打印验证，显存占用约两块 dumb size，而不是每帧复制 size 字节的 CPU 峰值。
- 「VBlank 与 tearing」：能说明「扫描中途换地址或改正在扫的内存 → 撕裂」；「等回扫再切 fb → 整帧一致」。
- Ctrl+C 干净退出、恢复显示，同 V1。

---

## V3：Atomic KMS

目标：用一次 atomic commit 提交整条管道的 property，完成 modeset 与 page flip，并主动制造失败来读内核态度。

### 基础

- [ ] 枚举 DRM property
- [ ] 找到 `CRTC_ID`
- [ ] 找到 `FB_ID`
- [ ] 找到 `SRC_X/Y/W/H`
- [ ] 找到 `CRTC_X/Y/W/H`
- [ ] 创建 mode blob
- [ ] 设置 `MODE_ID`
- [ ] 设置 `ACTIVE`

#### 思路

Atomic 把「一堆分散 ioctl」换成「一张属性表 + 一次提交」。先开能力再查表：

1. `drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)` 以及 `UNIVERSAL_PLANES`。失败则本机驱动过旧，V3 无法做，应明确报错而不是回退到静默 legacy。
2. 对象有三类常用：connector、CRTC、plane。`drmModeObjectGetProperties` + `drmModeGetProperty`，按 **名字** 找 id，不要写死 property id（每台机器不同）。
3. 点亮最小集合：
   - connector：`CRTC_ID = crtc_id`
   - CRTC：`MODE_ID = blob`，`ACTIVE = 1`
   - primary plane：`FB_ID`、`CRTC_ID`、`SRC_*`、`CRTC_*`
4. `SRC_*` 的单位是 **16.16 定点数**（像素 `<< 16`）。`CRTC_*` 是像素。这是 V3 最常见的「合法看起来却 EINVAL」来源。封装应对 lab 提供「按像素设 SRC」的接口，非法实验再走 raw。
5. mode 不能当整数写进 property，要 `drmModeCreatePropertyBlob(&mode, sizeof(mode))`，把 blob id 赋给 `MODE_ID`。blob 在不用时 destroy。
6. 找 primary plane：枚举 plane，`possible_crtcs` 含本 CRTC，且 type 为 `Primary`。V3 不做 overlay。

先把 property 名/id 打表到 stderr，验收时对着这张表讲。

### Atomic Commit

- [ ] 创建 atomic request
- [ ] 添加 connector property
- [ ] 添加 CRTC property
- [ ] 添加 plane property
- [ ] 执行 `drmModeAtomicCommit`
- [ ] 实现 non-blocking commit

#### 思路

`drmModeAtomicAlloc` → 多次 `AddProperty` → `Commit` → `Free`。一次 request 用完即弃，不要复用已 commit 的 req。

- **Modeset**：flags 带 `DRM_MODE_ATOMIC_ALLOW_MODESET`，可阻塞，对应 V1 的 SetCrtc。缺这个 flag 时改 mode/ACTIVE 会失败。
- **Flip**：只改 plane 的 `FB_ID`（及其它已保持的几何），flags 用 `DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT`（或等价 page flip 标志）。对应 V2。仍要 poll + handler。
- 先 `TEST_ONLY` 再真提交：尤其是实验非法值时。TEST_ONLY 不改变扫描状态，适合探路。
- Non-blocking 失败常见：`EBUSY`（上一次还在途）、`EACCES`（非 master）、`EINVAL`（属性组合非法）。把 flags、fb_id、plane_id 打进日志。
- 不要混用：V3 路径点亮后，不要再调用 `drmModeSetCrtc` / `drmModePageFlip` 做同一 CRTC 的每帧更新。

### 错误实验

- [ ] 故意设置非法 plane size
- [ ] 故意设置非法 framebuffer
- [ ] 观察 atomic commit 返回值
- [ ] 保存 kernel log
- [ ] 分析一次失败原因

#### 思路

目的是读**驱动的拒绝**，不是把机器搞到必须重启。约束：

1. 非法 plane size：例如 `SRC_W/H` 与 fb 不一致、`CRTC_W/H` 为 0、SRC 超出 fb。先 `TEST_ONLY`，记录 errno。
2. 非法 fb：未 AddFB 的 id、已 RmFB 的 id、宽高与 SRC 矛盾。同样先 TEST_ONLY。
3. 用户态日志与 `dmesg` / `journalctl -k` 一起存（例如 `docs/notes/v3-fail.log`，需要时再加）。分析写清：提交了哪些 property、哪一个不合法、内核说了什么、errno 是什么。
4. 真 commit 非法值若意外成功，说明理解有误，记下实际行为，不要为了「失败」去写更危险的组合（随机 property、关掉不相关 CRTC）。
5. 实验结束后必须回到合法 modeset，保证屏幕仍可用。

### V3 验收

- [ ] 能完成 atomic modeset
- [ ] 能完成 atomic page flip
- [ ] 能解释 legacy KMS 与 atomic KMS 的差别
- [ ] 能看懂基本 atomic property

#### 思路

差别用「同一条管道的两种写法」讲，而不是背术语：

| | Legacy (V1/V2) | Atomic (V3) |
| --- | --- | --- |
| 点亮 | `SetCrtc` 一次塞 fb+mode+connector | 多对象 property 一次 commit |
| 换帧 | `PageFlip(fb)` | 改 `FB_ID` 再 NONBLOCK commit |
| 失败 | 可能已经改了一半 | 校验失败则整单不生效（TEST_ONLY / 原子性） |
| 平面 | 主 plane 隐式 | 必须显式设 plane |

「看懂 property」：能指着日志说 `CRTC_ID` 把 connector 接到哪、`MODE_ID` 为何是 blob、`SRC_*` 为何左移 16 位、`ACTIVE=0` 意味着熄灭扫描。

V1/V2 命令行路径在 V3 合入后仍须能跑，作为回归。

---

## 进度约定

完成某一版时，把本节对应 `- [ ]` 改成 `- [x]`。思路段落可以保留，供下一轮复习。新的实验（GBM/EGL 等）另开 V4，不塞进本文件已有三版的验收里。
