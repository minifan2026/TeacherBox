# C 盘清理

清的都是能再生的临时文件，不动文档、桌面、下载这些地方。

## 三个按钮

- **扫描**：只统计，不删。弹一个框列出各项占用和合计
- **清理**：删掉 1 天以前的临时文件、Windows 更新下载缓存、缩略图缓存，并清空 C 盘回收站，最后报告释放了多少
- **每周日**：建一个计划任务，每周日 20:00 自动跑一次「清理」。第一次点会弹一次 UAC，同意后建好；任务挂在当前用户名下，只在登录状态时运行

## 清理范围

| 目录 | 说明 |
| --- | --- |
| `%TEMP%` | 当前用户的临时文件，只删 1 天以前的 |
| `C:\Windows\Temp` | 系统临时文件，同上 |
| `C:\Windows\SoftwareDistribution\Download` | Windows 更新下载缓存，不动正在用的更新 |
| `%LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db` | 缩略图缓存，删了会重新生成 |
| C 盘回收站 | 清空 |

被占用或没权限的文件会跳过，不会因为删不掉就中断。

## 其他

- 每次扫描/清理都会在 `%LOCALAPPDATA%\TeacherBox\clean.log` 记一行
- 取消定时：`schtasks /delete /tn "TeacherBox 清理 C 盘" /f`
- 想立刻看任务状态：`schtasks /query /tn "TeacherBox 清理 C 盘"`
- 更新缓存那项需要管理员权限才删得动，普通权限下会被跳过，不影响其他项
