# Watt Toolkit（原 Steam++）

给 GitHub、Gitee、Steam 这些站点做本地加速的工具，TeacherBox 的更新要靠它把 GitHub 的下载速度提上来。

## 检测

界面里的「检测」按钮执行：

```powershell
reg query HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam++ /v DisplayName
```

能查到 `DisplayName` 就说明装过了。

## 安装

「安装」按钮执行：

```powershell
winget install -e --id 9MTCFHS560NG --source msstore --accept-package-agreements --accept-source-agreements
```

这条走的是 Microsoft Store 的源，不经过 GitHub。第一次安装要下载几十兆，慢的时候五六分钟，日志里没动静不是卡死了，等它跑完。

装完之后按钮变绿，点「打开」启动。老版本装在 `C:\Program Files\Steam++\Steam++.exe`，新版本可能换了目录，从开始菜单搜「Watt Toolkit」最稳。

## 用法

- 启动后左侧列表里勾上 `GitHub`，点「一键加速」，保持它挂在托盘里
- 加速是本地反代，不开它 GitHub 就是直连，速度看运气
- 除了 GitHub，也可以勾 Gitee、Steam 商店、Discord，一次配好长期用

## 注意

- 个别杀软会把它当成加速器误报，加白名单即可
- 学校网络如果本身封了 GitHub，加速也只能改善速度，不能绕开封锁
