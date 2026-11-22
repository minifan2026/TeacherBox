# Python 运行时

## 这个工具做什么

- 用 `python --version` 检查本机是否已安装 Python
- 未安装时，调用 `winget` 安装 Python 3.12
- 安装完成后自动重新检测一次
- 提供两个快捷入口：IDLE、交互式解释器

## 检测规则

工具清单里的 `check` 指令执行完成后：

- **退出码 0 且标准输出非空** → 视为「环境就绪」，不会再执行安装
- **退出码非 0 或输出为空** → 视为「缺少环境」（例如 `python 不是内部或外部命令`），此时才会执行 `install`

## 手动验证

在 PowerShell 里执行：

```powershell
python --version
```

正常会输出类似 `Python 3.12.10`。如果提示「不是内部或外部命令」，说明没装或没进 PATH。

## 安装失败怎么办

1. 确认 `winget` 可用：`winget --version`
2. 或直接去 https://www.python.org/downloads/ 下载安装包
3. 安装时记得勾选 **Add python.exe to PATH**

## 需要更多版本

把目录复制一份，改名成 `python311` 之类的编号，再改 `tool.json` 里的检测/安装指令即可。目录名只能用字母、数字和 `. _ - +`。
