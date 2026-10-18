# BlueArchive JP FPS Unlock

给  BlueArchive JP 手游解锁帧率与渲染分辨率：注入进游戏进程的一个小 `.so`，接管
`Application.targetFrameRate`、`QualitySettings.vSyncCount`、`Screen.SetResolution`。

> **本仓库只包含注入进游戏进程的那部分代码（`fps_unlocker.cpp`）。**
> 注入器（ptrace 远端 `mmap` + `dlopen`）与 状态 悬浮窗**不公开**。
> 想用起来需要你自己的注入方式（Zygisk 模块 / LSPosed / 任意 ptrace 注入器如JS HOOK），

---

## ⚠️ 先读这一段

- **这是改客户端运行状态**，违反绝大多数游戏的用户协议。**可能被检测、可能封号。**
  本项目**没有任何反检测**，不保证安全。**别用在你在意的主号上。**
- 只用于**你自己的设备、你自己的账号**。因使用产生的后果，需自己负责。
- 高帧率会让 GPU 长时间高温 + 高负载，可能对设备有损耗。
- 不修改游戏文件、不联网、不上传任何数据——全部逻辑在进程内存里，
  游戏重启即失效。

---

## 实测效果

一加 Pad 2 Pro（OPD2413）/ Android 15 / KernelSU / 3392x2400 @144Hz：

| 渲染分辨率 | 像素占比 | 实测帧率 |
|---|---|---|
| 3392x2400（原生） | 100% | 70-120（波动大） |
| 2560x1811（2K） | 57% | 137-144 |
| 2240x1584（1.5K） | 44% | 144 |
| **1920x1358（1080p）** | **32%** | **143-144** |

**结论**：官方只开放 30 / 60 帧两档。原生 3392x2400 强解到高帧率会明显卡顿掉帧；
在我这台设备上，未解锁时也能跑 72 帧（用 Scene 工具观测到）。

---

## 环境要求

| 项 | 要求 |
|---|---|
| 设备 | arm64 Android，**已 root**（KernelSU / Magisk 均可） |
| Android | 12 以上实测通过（本机 Android 15 / SDK 35） |
| 编译 | NDK r29（只需 clang/clang++） |
| 游戏 | 默认目标包名 `com.YostarJP.BlueArchive` |

---

## 构建

```bash
git clone https://github.com/<you>/BlueArchive-FPS-Unlock.git
cd BlueArchive-FPS-Unlock

$HOME/android-ndk/android-ndk-r29/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++ \
    --target=aarch64-linux-android21 -O2 -shared -fPIC \
    -static-libstdc++ \
    -DPKG_NAME='"com.YostarJP.BlueArchive"' \
    -o libfps_unlocker.so \
    "src/fps_unlocker.cpp" -llog
```

产物：`libfps_unlocker.so`（约 21 KB）。

---

**配置**（读，可选；不写就用默认值）：

```
/data/user/0/<游戏包名>/files/.bauni.conf
fps=144          # 目标帧率，有效范围 40-185
res=1920x1358    # 渲染分辨率：0 或 original 表示保持原始；也支持只写宽度（高度按比例算）
```

**状态输出**（写，供外部读取，可以自己写一个悬浮窗）：

| 文件 | 内容 |
|---|---|
| `.bauni_status` | `INJECTED\|FPS=144\|PID=20934` |
| `.bauni_now` | 实测帧率（每秒更新） |
| `.bauni_res` | 实际渲染分辨率 + 像素占比，如 `1920x1358\|32%` |
| `.bauni_log` | 完整执行轨迹（**唯一可靠日志**，见下） |

---

## 原理

```
注入到游戏进程后（IL2CPP）：
  il2cpp_resolve_icall("UnityEngine.Application::set_targetFrameRate")
  → set(144) → patch_to_ret()             ← 不 patch，游戏会改回 30
  → set_vSyncCount(0) → patch_to_ret()    ← vSync 不为 0 时 targetFrameRate 被忽略
  → Screen.SetResolution(w, h, 1, &fps) → patch_to_ret()   ← 降渲染分辨率
```

---

## 已知限制

| 说明 |

| IL2CPP 兜底偏移与版本相关 | `il2cpp_resolve_icall` 走动态符号表最稳；代码里的兜底偏移 `base+0xc330dc` 只在被测版本可用，**游戏更新后必须重新校准**（正常路径不需要它） |

---

## 免责声明

本项目仅供**学习 Android 逆向、动态注入、Unity 引擎机制**使用，**仅限自有设备与自有
账号**。作者不对任何账号封禁、数据丢失、设备损坏或违反第三方服务条款的后果负责。
如果你在游戏用户协议禁止的范围内使用，风险由你自行承担。

## 许可

[MIT](LICENSE)。第三方组件声明见。
