# TeacherBox 工具包写作规范

给写代码的 Agent：照这份规范生成一个目录，放进 `tools/` 就能被工具箱认出来，不需要改主程序。

## 目录

```
tools/<id>/
  tool.json      必需，清单
  help.md        可选，界面「帮助」按钮读它
  *.ps1 / *.bat  可选，脚本随意
```

`<id>` 就是目录名，只允许 `A-Z a-z 0-9 . _ - +`，不能有中文、空格、斜杠，长度不超过 64。

## tool.json 字段

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `name` | 字符串 | 界面上显示的名字，中文可以 |
| `version` | 字符串 | 填 `1.0`、`3.2.1` 这种，可省略 |
| `description` | 字符串 | 一句话说清干什么，超过一行会被截断 |
| `check` | 字符串或对象 | 检测命令，见下 |
| `install` | 字符串或对象 | 安装命令，见下 |
| `entry` | 字符串 | 工具目录里的可执行文件名，界面自动多一个「打开程序」按钮 |
| `actions` | 数组 | 自定义按钮，元素为 `"标签"` 或 `{"label":"标签","command":"命令"}` |
| `help` | 字符串 | 帮助文件名，默认 `help.md` |

`check` / `install` 也可以写成对象，加超时（秒）：

```json
{ "name": "某工具", "version": "1.0",
  "description": "一句话",
  "check":   { "command": "where code", "timeout": 20 },
  "install": { "command": "winget install --id X -e --silent", "timeout": 1800 },
  "actions": [ { "label": "打开", "command": "start \"\" code.exe" } ] }
```

## 命令的执行环境

每条命令都由 `cmd.exe /d /s /c "<命令>"` 执行，**工作目录就是该工具目录**，所以脚本可以直接写相对路径：

```json
"actions": [ {"label": "扫描", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File scan.ps1 -Mode scan"} ]
```

三条硬规矩：

1. 命令里别再套双引号，外层已经被 cmd 包了一层。需要长脚本就单独写 `.ps1`。
2. 需要复杂表达式、括号、分号，一律放进 `.ps1`，不要塞进 `-Command`。
3. `.ps1` 里如果写中文，文件存成 UTF-8 with BOM，否则 Windows PowerShell 5.1 会乱码。

## 检测约定

`check` 的退出码为 0 **且** stdout 有非空输出，界面判为「就绪」，两点缺一不可。

```json
"check": "if exist \"%ProgramFiles%\\7-Zip\\7z.exe\" (echo ok)"
```

## 需要管理员时

不要默认假设有管理员权限。真需要就自己提权，宁可弹一次 UAC：

```
powershell -NoProfile -ExecutionPolicy Bypass -File task.ps1 -Mode schedule-elevated
```

脚本里用 `Start-Process -Verb RunAs -FilePath powershell -ArgumentList ...` 重新拉起自己。

## 装不进工具目录就装系统

`install` 适合 `winget`、注册表、下载解压这类动作，装完记得让 `check` 能验证出来。超时务必给足，`winget` 冷启动三五分钟很正常。

## 提交到工具市场

1. 把整个 `tools/<id>` 目录打成 zip，`tool.json` 放在 zip 根目录（套一层同名目录也行，工具箱会自己往下找一层）。
2. 在仓库根目录 `market.json` 的 `items` 里加一项：

```json
{ "id": "img-resize", "name": "图片批量缩放", "version": "1.0",
  "description": "一句话", "url": "https://raw.githubusercontent.com/minifan2026/TeacherBox/main/market/img-resize.zip" }
```

3. 用户点「下载」后：下 zip → 展开 → 落到 `tools/<id>` → 界面立刻出现这个工具。

## 反面清单

- 不要弹交互式黑框（`pause`、`Read-Host`），界面会一直等。
- 不要把路径写死成某台机器，用 `%LOCALAPPDATA%`、`$env:USERPROFILE`。
- 不要在 `tool.json` 里塞几百行命令，拆成脚本。
- 不要依赖 `gcc`、`make` 这类没装的构建工具。
- 工具目录名不要带中文，界面会直接忽略并给出警告。
