# Git 版本管理

## 检测

```powershell
git --version
```

输出类似 `git version 2.47.1.windows.1` 表示已安装。

## 安装

界面里的「安装」按钮会执行：

```powershell
winget install -e --id Git.Git --accept-source-agreements --accept-package-agreements
```

装完后如果本窗口检测仍然失败，重新打开 TeacherBox 即可（新的 PATH 需要新进程才会生效）。

## 适合老师的用法

- 用仓库管理一学期的讲义与试卷，改错了可以回退
- 把改动 push 到 Gitee / GitHub，多个老师协作备课
- 用 `.gitattributes` 避免 Word 文档被当成二进制乱合并

## 小抄

```bash
git init                 # 新仓库
git add . && git commit -m "第一次提交"
git log --oneline        # 看历史
git checkout -- 文件      # 撤销未提交的修改
```
