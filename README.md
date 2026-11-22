# TeacherBox 工具箱

面向老师的 Windows 工具箱：一个 `main.exe` 负责升级与守护，`gui.exe` 用 WebView2 显示界面并常驻托盘，工具以「文件夹」形式放进 `tools/`，改清单即插即用。

- 只支持 Windows 10/11
- 三个进程都是 GUI 子系统（没有黑框控制台）
- 编译：CMake + MSVC（VS2022 x64），也支持 MinGW-w64 + GCC（`-Gcc`）

## 目录结构

```
TeacherBox/
├─ main.exe            调度器：先跑 update.exe，再拉起 gui.exe 并守护
├─ update.exe          升级器：自带进度窗口，检查/下载/覆盖
├─ gui.exe             主界面：WebView2 + 系统托盘
├─ WebView2Loader.dll  微软官方加载器（必须与 gui.exe 同目录）
├─ web/                界面资源（index.html / app.js / style.css）
├─ tools/              工具包目录，一个子目录 = 一个工具
├─ update.json         版本与更新源配置
├─ resources/          版本信息 + 应用程序清单（app.rc / app.manifest）
├─ release/            打包暂存（zip + manifest，上传到 GitHub Releases，不进 git）
├─ scripts/            WebView2 拉取、IID 生成、崩溃冒烟测试
└─ logs/               运行日志（实际写在 %LOCALAPPDATA%\TeacherBox\logs）
```

三个 exe 都嵌入了同一份版本信息与清单（`resources/app.rc`），在资源管理器里能看到「TeacherBox 教师工具箱 / 1.1.0.0」，清单声明 `asInvoker` + Per-Monitor V2 DPI。杀软（尤其 360）对「没有版本信息、无清单、还会拉起子进程」的裸 exe 判定很激进，带上这些信息能显著减少误报。

## 启动链路

```
main.exe（无窗口）
   ├─ 1. 启动 update.exe，等它退出（升级）
   ├─ 2. 启动 gui.exe（界面 + 托盘，常驻）
   └─ 3. 守护 gui.exe
         ├─ 用户点关闭按钮 → gui 发 EXIT_USER 正常退出 → main 一起退出（不重启）
         ├─ gui 异常退出   → 弹窗询问是否生成反馈 → 生成报告并打开目录 → 重启 gui
         └─ 连续 10 次异常 → 弹「致命性问题」→ main 退出
```

进程之间用命名管道通信（`\\.\pipe\teacherbox-<角色>-<pid>`），按行传输纯文本协议：

| 消息 | 方向 | 含义 |
| --- | --- | --- |
| `READY` | 子 → 主 | 子进程已就绪 |
| `STATUS xxx` | 子 → 主 | 进度文本 |
| `LOG xxx` | 子 → 主 | 日志 |
| `EXIT_USER` | 子 → 主 | 用户主动关闭窗口 |
| `SHUTDOWN` | 主 → 子 | 要求退出（预留） |

「运行时长 ≥ 30 秒后才崩」会重新计数，所以偶发崩溃不会累计到 10 次。

## 工具包格式

`tools/` 下每个子目录就是一个工具，**目录名 = 工具编号**，只允许字母、数字和 `. _ - +`，不能出现中文、空格、斜杠：

```
tools/
└─ python/            ← 编号 python
   ├─ tool.json       ← 清单
   └─ help.md         ← 帮助文档（界面里点“帮助”直接显示）
```

`tool.json`：

```json
{
  "name": "Python 运行时",
  "version": "3.12",
  "description": "一句话说明",
  "check":   { "command": "python --version", "timeout": 20 },
  "install": { "command": "winget install -e --id Python.Python.3.12", "timeout": 1200 },
  "actions": [
    { "label": "打开 IDLE", "command": "python -m idlelib" }
  ],
  "help": "help.md"
}
```

字段说明：

- `check`：检测指令。**退出码 0 且输出不为空** = `true`（已就绪）；否则 = `false`（未安装/未找到），此时才执行 `install`
- `install`：安装指令。`timeout` 单位是秒，`0` 表示用默认上限（30 分钟）
- `actions`：按钮列表，点了直接启动对应命令；`command` 若写工具目录内的文件名（如 `pack.exe`），会自动解析成绝对路径
- `entry`：等价于在 `actions` 头部加一个「打开程序」按钮
- 指令统一经 `cmd.exe /d /s /c` 执行，所以 `where`、`&&`、重定向都能用
- `check` / `install` 也可以直接写字符串，如 `"check": "git --version"`

目录名不合规、缺 `tool.json`、JSON 解析失败的工具会被跳过，并在界面顶部列出原因。

## 构建

```powershell
cd C:\Users\Administrator\Desktop\项目\TeacherBox
.\build.ps1
```

脚本会：

1. 没有 `_deps/webview2` 时自动下载 NuGet 上的 WebView2 SDK（`scripts/fetch_webview2.ps1`）
2. `cmake -G "Visual Studio 17 2022" -A x64` 配置并编译
3. 装配 `dist/`：三个 exe + WebView2Loader.dll + web/ + tools/ + update.json

其它用法：

```powershell
.\build.ps1 -Config Debug      # 调试版
.\build.ps1 -Gcc              # 用 GCC（需要 gcc/ninja 在 PATH）
.\build.ps1 -SkipAssemble     # 只编译
```

### 关于 GCC

WebView2 官方头文件里的接口没有 `IID_XXX` 导出符号，MSVC 可以靠 `__uuidof`，GCC 不行。本工程的做法是：`scripts/gen_iids.ps1` 从 `WebView2.h` 里抽出所有 `MIDL_INTERFACE("guid")`，生成 `src/gui/webview2_iids.h`，代码统一用 `kIID_XXX` 常量。因此 GCC 也能编。

### 图标与签名

- 图标资源：`resources/app.ico`，由 `scripts/make_icon.ps1` 生成（朱红圆角底 + 米白衬线 T，9 帧 16~256）。`app.rc` 以 `101 ICON` 嵌进三个 exe，`gui.exe` 建窗口时用 `LoadImageW(101)` 取同一枚图标，所以资源管理器、任务栏、托盘看到的是同一个，用户在文件夹里不会认不出 `main.exe`
- 改图标后要重新跑 `pwsh -File scripts\make_icon.ps1`，再 `.\build.ps1`

签名：

```powershell
.\build.ps1
pwsh -File .\scripts\sign.ps1        # 没证书会先自签一张，然后签 dist\*.exe
```

`sign.ps1` 用 `CN=minifan` 的自签证书签三个 exe，pfx/cer 导到 `certs\`（不进 git）。签名后 `Get-AuthenticodeSignature` 能看到签名者，但状态是 `UnknownError`——自签证书没进系统信任链，装的人照样会看到"未知发布者"。

真正的解法是买 OV/EV 代码签名证书（一年几百到几千元，EV 还能立刻过 SmartScreen 积累的信誉）。用自签证书时，给别人装可以：

1. 把 `certs\TeacherBox.cer` 一起发过去
2. 双击 → 安装证书 → 当前用户 → 放入「受信任的发布者」
3. 之后这批 exe 就是"已验证的发布者"，不再弹未知发布者

注意每次 `build.ps1` 都会重新生成 exe，签名要在装配之后再跑一遍，顺序不能反。

### CI

`.github/workflows/cmake-multi-platform.yml`：push/PR 到 `main` 或手动触发，在 `windows-2022` 镜像上用 MSVC 构建，产物是 `TeacherBox-<版本>-win64.zip` + `dist/`，挂在 Actions 的 Artifacts 里。

镜像固定在 `windows-2022` 是因为 `windows-latest` 已经换成 `windows-2025-vs2026`（只有 VS2026），`-G "Visual Studio 17 2022"` 在那上面找不到实例。

## 运行

直接双击 `dist\main.exe`：

1. 会先出现更新窗口（默认 `update.json` 没配置更新源，显示「离线模式，已是最新版本」后自动关闭）
2. 主界面出现，右上角有版本号
3. 把窗口关掉 = 退出程序（同时 main.exe 退出，不重启）
4. 点托盘图标 = 重新显示界面；右键托盘 = 显示/隐藏/打开工具目录/退出

调试时可以 `.\dist\main.exe --skip-update` 跳过升级步骤。

## 升级配置

`update.json`（跟着 exe 走，本地配置）：

```json
{
  "version": "1.1.0",
  "remote": "https://github.com/minifan2026/TeacherBox/releases/latest/download/update.json",
  "package": ""
}
```

- `version`：当前安装的版本，用于和远端比较
- `remote`：远端 manifest 的 URL。这里用 Releases 的 `latest/download/` 固定路径，发新版本不用改地址
- `package`：本地更新包文件名（相对 exe 目录），用于离线分发/测试；`remote` 为空时只走它

远端 manifest 长这样：

```json
{
  "version": "1.1.0",
  "package": "https://github.com/minifan2026/TeacherBox/releases/latest/download/TeacherBox-1.1.0-win64.zip"
}
```

升级流程：拉取 manifest → 比较版本 → 下载 zip → 解压到临时目录 → 逐个覆盖。正在运行的 `main.exe` 会先被改名成 `*.old` 再写入新文件，下次启动生效；`*.old` 在下次升级时清理。

拉不到时还会换域名重试：`raw.githubusercontent.com` ↔ `cdn.jsdelivr.net/gh/...`。GitHub 站点整体不通时这套没用，得先开 Watt Toolkit。

### 发一个版本

1. 改 `resources/app.rc` 里的 `FILEVERSION` / `PRODUCTVERSION`
2. 改 `update.json` 的 `version`，然后 `.\build.ps1` 装配 `dist/`
3. 打包到本地 `release/`（这个目录不进 git）：
   `Compress-Archive -Path "dist\*" -DestinationPath "release\TeacherBox-<版本>-win64.zip" -Force`
4. 写 `release/update.json`，`package` 指向下一步会传上去的 zip 下载地址
5. 发 Release（tag 用 `v<版本>`），把 `update.json` 和 zip 两个文件当附件传上去：
   ```
   gh release create v1.1.0 release\update.json release\TeacherBox-1.1.0-win64.zip -t "TeacherBox 1.1.0"
   ```
   没装 gh 就在网页上 New release，拖文件进去
6. 提交推送，老的安装下次启动就会自己升上来

包里的 `update.json` 必须是新版本号，否则每次启动都会重复下载同一个包。附件名不要带版本号以外的变化（`latest/download/` 是按文件名找的）。


## 日志与反馈

- 日志：`%LOCALAPPDATA%\TeacherBox\logs\`（main / gui / update 各自按天一个文件）
- 崩溃反馈：`%LOCALAPPDATA%\TeacherBox\feedback\feedback-YYYYMMDD-HHMMSS.txt`

反馈文件里包含退出码、运行时长、连续失败次数、系统信息、gui 管道输出和两边日志尾部——直接发给开发者即可。

## 自测

`scripts/smoke-crash.ps1` 会自动强杀 gui.exe 来验证守护逻辑（需要先用 pwsh 跑，UTF-8 中文才不乱码）：

```powershell
# 先启动 dist\main.exe，然后：
pwsh -File .\scripts\smoke-crash.ps1 -Rounds 1     # 异常退出 -> 弹窗 -> 点“否” -> gui 重启
pwsh -File .\scripts\smoke-crash.ps1 -Rounds 10    # 连崩 10 次 -> 弹「致命错误」-> main 退出
```

已实测通过：`update.exe` 退出码 0（约 1.4 秒，离线模式）、gui 窗口可见 958×646、异常退出后点「是」生成 `feedback-*.txt`（3.9 KB，含系统信息与日志尾部）、点「否」自动重启、第 10 次弹出「TeacherBox 致命错误」后 main 退出、发送 WM_CLOSE 则 main 与 gui 一起正常退出不重启。

## 排查

| 现象 | 处理 |
| --- | --- |
| 界面白屏 / 提示 WebView2 初始化失败 | 安装「Microsoft Edge WebView2 Runtime」（Win11 一般自带） |
| 提示缺少 WebView2Loader.dll | 把 `_deps\webview2\build\native\x64\WebView2Loader.dll` 拷到 exe 旁 |
| 工具一直「缺少环境」 | 看界面底部运行输出，确认指令本身能否在 cmd 里跑通 |
| 改了 PATH 仍然检测失败 | 装完新软件需要重启 TeacherBox，子进程才会拿到新环境变量 |
| `main.exe` 一运行就「拒绝访问」，随后文件消失 | 杀软（实测 360 安全卫士）把启动器当木马清掉了。把 `dist` 目录加进 360 的「信任区 / 白名单」，或重新编译（本工程已带版本信息+清单，能避开该误报） |
| 首次启动偶尔「拒绝访问」，几秒后正常 | 刚编译出来的 exe 被杀软实时扫描占用，稍等 2–3 秒再启动即可 |
| 窗口只出现在任务栏、看不到界面 | 父进程若给子进程设了 `STARTF_USESHOWWINDOW`，子进程首次 `ShowWindow` 会被忽略；`main.cpp` 的 `SpawnChild` 已刻意不设该标志，`gui.cpp` 里也连调两次 `ShowWindow` |

## 1.1.3 新增

- **工具市场**：页签「工具市场」从 `market.json`（raw.githubusercontent.com）拉清单，点「下载」直接把 zip 装进 `tools/`，已装的显示「装好了」。
- **拖拽 / 上传安装**：把工具包 zip 拖进窗口就装；也可以点「导入」选本地 zip。装完立刻出现在「本机工具」。
- **中文桌面快捷方式**：首次启动如果没有，就在桌面建一个 `老师工具箱.lnk`，指向 `main.exe`，图标取 exe 里的资源。
- **内置工具 `agent-spec`**：一份写给 Agent 看的工具写作规范，`agent-spec.md` 与 `agent-spec.txt` 内容一致；两个按钮「打开 TXT」「复制 MD 到桌面」。
- 界面上所有「插件」字样统一改成「工具」。

## 签名（发布者 minifan）

三个 exe 都用 `scripts\sign.ps1` 自签，Subject 固定 `CN=minifan, O=minifan`，资源里 `CompanyName / LegalCopyright / Publisher` 也都是 minifan。

自签证书没被系统信任时，双击 exe 会提示「未知发布者」。包里的 `安装证书.bat` 会把 `TeacherBox.cer` 装进本机的「受信任的根」和「受信任的发布者」，**装一次就够**：

```
双击 安装证书.bat  →  是  →  重新打开 main.exe
```

想彻底免掉这个提示，只有买 OV/EV 代码签名证书（每年几百到几千），自签做不到。

## 启动系统工具被拒

「系统自带小程序」里的任务管理器、服务、设置这类工具，普通权限下 `ShellExecute` 偶尔返回「拒绝访问 / 需要提升」。1.1.3 起 `LaunchDetached` 会再试一次带 `runas`（UAC 提升），用户点「是」就能起来；用户点「否」不会反复弹。

如果整个 exe 都被「拒绝访问」并随后被删，那是 360 之类的杀软误杀，把 `dist` 目录加进白名单即可（见下方排查表）。

## GitHub Pages（*.github.io）

1. 新建仓库，名字必须是 `<用户名>.github.io`（例如 `minifan2026.github.io`），公开
2. 仓库根放 `index.html`，推到 `main`
3. 仓库 Settings → Pages → Source 选 `Deploy from a branch`，分支 `main`、目录 `/ (root)`，Save
4. 等 1–2 分钟，访问 `https://<用户名>.github.io`

也可以给别的仓库开 Pages（Settings → Pages 里任选分支/目录），只是没有那个好记的域名。`<用户名>.github.io` 一个账号只能有一个。

## 1.1.4 工具市场改成分页

清单不再是一个大 `market.json`，而是 GitHub Pages 上的静态站 `https://minifan2026.github.io/TeacherBoxWeb`：

```
api/page-0.json     前 50 条
api/page-50.json    第 51-100 条
api/meta.json       total / page_size / updated
tools/<id>.zip      每个工具一个包，点下载才拉
```

客户端（`src/gui/gui.cpp`）按 `offset` 取页，点「加载更多」再取下一页，取过的页留在内存里不重复请求；列表只渲染名称 / 版本 / 作者 / 体积，卡片里才显示简介。条目带 `dev: true` 时挂「开发者」角标。

GitHub Pages 是纯静态，`?offset=50` 这种查询串服务端拿不到，所以落成 `page-<offset>.json`；以后有真后端就换成 `GET /api/list?offset=<n>&limit=50`，客户端只改一行拼地址。

页拉不动会回落到仓库里的 `market.json`（老地址），所以没网 / 没 Pages 也不会空着。

站点仓库：`https://github.com/minifan2026/TeacherBoxWeb`（`tools-index.json` 是唯一真相，`scripts\build-api.ps1` 生成分页文件）。
