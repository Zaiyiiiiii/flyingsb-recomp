# FlyingSB（幻想西游记）静态重编译版

把 1998 年的 Win32 游戏 FlyingSB / 幻想西游记（繁体中文版）静态重编译成 C，不改原版 exe 和资源，在现代平台上运行。

## 特性

- 支持 Windows、浏览器、Android。
- 支持 16:9 宽屏，默认匹配屏幕比例；主菜单新增“遊戲設定”可切换。
- 支持手柄。
- Android 有触屏虚拟按键，方向盘出现在左半屏手指按下的位置。
- 浏览器版的游戏数据按需读取，不用整体下载。

仓库不含原版的图片、声音、视频、文字和生成的 C 代码。构建和运行都需要你自己的原版游戏目录。

## 运行

### Windows

```
flyingsb.exe --game <游戏目录>
```

| 参数 | 作用 |
|---|---|
| `--save <目录>` | 存档/配置目录 |
| `--skip-video` | 跳过视频 |
| `--cheat` | 启用原版秘籍输入 |
| `--trace-api` | 打印 API 调用 |

### Web

```
flyingsb-server --game <游戏目录> --web <web 文件目录> --port 8080
```

浏览器打开 `http://127.0.0.1:8080/`。从其他设备访问需要 HTTPS（`--tls-cert`、`--tls-key`），例如 `https://[IPv6 地址]:8080/`。启用 TLS 后，同端口的 IPv4 HTTP 访问会跳转到证书里的地址。

| 参数 | 作用 |
|---|---|
| `--game` | 游戏目录（只读） |
| `--web` | 网页客户端目录，默认 `../build/web` |
| `--port` | 端口，默认 8080 |
| `--bind` | 绑定地址，默认监听 IPv4 和 IPv6 全部地址 |
| `--font` | 提供给浏览器的中文字体，不填则自动查找 |
| `--tls-cert` / `--tls-key` | HTTPS 证书与私钥 |

网址参数：`?skip-video`、`?trace`、`?cheat`，用 `&` 连接。游戏数据按需读取；存档在浏览器 IndexedDB 里，清除站点数据会丢失。

### Android

```
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

游戏数据不在 APK 里，首次启动时在启动页选择你的游戏目录导入。横屏全屏，存档在应用私有目录，卸载即删除。

### 游戏版本

加载时只校验基址和镜像大小。运行用的 `FlyingSB.exe` 可以是原版，也可以是免光盘补丁版；两者只差 4 个字节（`0x43B278` 一个字节、`0x559E88` 三个字节，光盘盘符检测），原版会在内存里自动补齐。

### 中文字体

按顺序查找：环境变量 `FLYINGSB_FONT` → 存档目录 `font.txt`（内容为字体路径）→ `fonts/simsun.ttc` → 系统字体。仓库不带字体，`simsun.ttc` 自行从 Windows 复制。没有它时改用系统中文字体，没有宋体的点阵字形。详见 `fonts/README.txt`。

### 画面比例

默认匹配屏幕：接近 4:3 用原版画面，更宽的屏幕按比例加宽（高度仍是 600）。

手动指定：主菜单“遊戲設定”（在“結束”之前）里选 4:3 或 16:9（1066x600）。写入 `FlyingSB.INI` 的 `[VideoOption] ScreenRatio`，重启后生效；删除这一项恢复匹配屏幕。

## 操作

**键盘**

| 按键 | 功能 |
|---|---|
| 方向键 | 移动 |
| Enter / X | 确认 |
| Esc / Z | 取消 |
| Space | 空格键功能 |
| C / D | 切换队伍 |
| F11 / Alt+Enter | 全屏 |

**手柄**

| 手柄 | 功能 |
|---|---|
| 十字键、左摇杆 | 方向 |
| A、Select | Enter（确认） |
| B、Start | Esc（取消/菜单） |
| X | Space |
| Y | M |
| LB / RB | C / D |
| R3 | 全屏 |

原生版可在存档目录用 `gamepad.ini` 改映射。网页版里浏览器不把手柄按键当作用户操作，R3 可能进不了全屏，请用页面上的“全屏”按钮或 F11。

**Android 虚拟按键**（没有手柄时显示，接入手柄后隐藏）

| 位置 | 功能 |
|---|---|
| 左半屏任意空白处按下 | 方向盘出现在落点，松手消失；可斜向 |
| 右下 A / B / X / Y | Enter / Esc / Space / M |
| 右上 C / D | 切换队伍 |

## 构建

### 依赖

- Python 3：`pip install capstone pefile`
- CMake、Ninja、clang（或 MSVC）；FreeType、SDL3 由 CMake 自动拉取
- Web 客户端：Emscripten
- Android：Android SDK（platform 35）、NDK 25.2.9519653、JDK 17+
- 服务器：Rust（cargo）

### Windows

```
cmake -S . -B build/win -G Ninja -DCMAKE_C_COMPILER=clang -DFLYINGSB_ORIGINAL_EXE=<FlyingSB.exe>
cmake --build build/win
```

产物 `build/win/flyingsb.exe`，构建后会把 `fonts/` 复制到它旁边。

### Web 客户端

先载入 emsdk 环境：

```
cmake -S . -B build/web -G Ninja -DCMAKE_TOOLCHAIN_FILE=$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake -DFLYINGSB_ORIGINAL_EXE=<FlyingSB.exe>
cmake --build build/web
```

产物 `build/web/flyingsb.{html,js,wasm}`。

- 使用 pthreads（`-pthread -sPROXY_TO_PTHREAD`），页面必须跨源隔离（COOP/COEP），因此需要 HTTPS 或 localhost。
- 来宾主循环不让出线程，OffscreenCanvas 不会提交；画面写入共享缓冲，主线程在 `requestAnimationFrame` 里 `putImageData`。
- 键盘和手柄由 `web/shell.html` 直接读取，经共享内存环形队列送入；SDL 的键盘/手柄事件在 Web 下被忽略。
- 游戏文件由 `runtime/vfs_web.c` 通过同步 XHR Range 读取，块缓存 256KB。存档目录 `/save` 挂载 IDBFS。

### Android

先构建一次 Windows 版（拉取 SDL、FreeType 并生成 C 代码，Android 复用）。打包前把 `simsun.ttc` 放进 `fonts/`，会被打进 APK；不放则只用系统自带的 Noto CJK，部分机型可能缺字。

在 `android/` 下创建 `local.properties`：`sdk.dir=<Android SDK 路径>`，然后：

```
cd android
./gradlew assembleDebug
```

产物 `android/app/build/outputs/apk/debug/app-debug.apk`。可选参数：`-PsdlDir=<SDL 源码>`、`-PftDir=<FreeType 源码>`、`-PgenDir=<已生成的 C 代码目录>`。

- Gradle 只负责打包；原生库按 ABI（arm64-v8a、x86_64）由 NDK 的 CMake 构建。
- 游戏数据由启动页导入到 `Android/data/<包名>/files/game`（文件管理器可见，也可以自己直接放进去）；`runtime/vfs_android.c` 启动时扫描该目录建清单，与 Web 版共用清单机制（`HOST_REMOTE_GAME`）。
- 启动图标和 Windows exe 图标都是重编译时从原版 exe 的资源里提取的（`recomp/icon.py`），不进仓库。
- 虚拟按键在 `platform/touch_pad.c`，触摸不合成鼠标事件。

### 服务器

```
cd server
cargo build --release
```

axum 0.8 + axum-server（rustls，ring）。响应头带 COOP / COEP / CORP；提供清单、游戏文件（支持 Range）、字体（`@font`、`@font2`、`@font3`，按真实路径去重）。默认 `--bind 0.0.0.0` 时同时监听 `0.0.0.0` 和 `[::]`（Windows 的 `[::]` 不是双栈）。配置证书后 TLS 只在 `[::]` 上，`0.0.0.0` 上是明文 HTTP，只做到证书 SAN 地址的跳转。

## 开发

| 目录 | 内容 |
|---|---|
| `recomp/` | 重编译器：Capstone 反汇编 → C |
| `runtime/` | 来宾内存、API 软件实现、VFS |
| `platform/` | SDL3 窗口、渲染、音频、键盘、手柄、触屏 |
| `web/` | 网页外壳 `shell.html` |
| `android/` | Android 工程 |
| `server/` | Rust 服务器 |
| `fonts/` | 本地字体（不入库） |

- **钩子**：`recomp/hooks.txt` 每行 `地址 函数名 [跳转目标]`，在该地址的指令前调用 `runtime/hooks.c` 里的 C 函数；带跳转目标时函数返回 `int`，非零则跳过原逻辑去目标地址。主菜单的“遊戲設定”就是用它实现的。
- **未知间接调用**：运行时遇到没有对应重编译函数的间接调用地址，会记一条 `[警告]` 日志，按空函数（返回 0，只弹返回地址）跳过，不退出。要补上这个入口，把地址（十六进制）加进 `recomp/extra_roots.txt` 后重新构建。
- **调试**：`--trace-api` / `?trace` 打印 API 调用；`--skip-video` / `?skip-video` 跳过视频。视频由 `runtime/avi_codec.c` 手写解码 Cinepak/MSVC1。
- 不要提交 `game/`、`build/`、`server/target/` 和 `fonts/` 下的字体文件（已在 `.gitignore`）。
