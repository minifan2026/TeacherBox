# Windows 系统小工具

这个工具没有检测/安装指令，只有一组 `actions`：界面上的按钮点下去，TeacherBox 就按清单里的 `command` 启动进程。

## 清单写法

```json
{
  "name": "Windows 系统小工具",
  "actions": [
    { "label": "记事本", "command": "notepad.exe" },
    { "label": "某个自带 exe", "command": "my-tool.exe" }
  ]
}
```

规则：

- `command` 可以是 PATH 里的程序名（`notepad.exe`）、协议（`ms-settings:`）、完整路径
- 如果写的是工具目录里的文件名（例如 `pack.exe`），TeacherBox 会自动补成该目录下的绝对路径再启动
- `label` 是按钮文字，可以写中文

## 为什么这样设计

工具目录本身就是「一个文件夹 = 一个工具」，把 exe、脚本、说明文档一起丢进文件夹，界面只负责按清单去调用。想加新能力不用改程序，改清单就行。
