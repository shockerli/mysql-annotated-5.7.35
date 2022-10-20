# MySQL 源码注解
> 本项目提供基于 MySQL 官方源码的中文注释，方便 MySQL 源码研究者一起讨论。欢迎大家提 issue & commit。
>
> 基于 `MySQL 5.7.35` 版本
> 
> MySQL 官方源码下载: https://dev.mysql.com/get/Downloads/MySQL-5.7/mysql-5.7.35.tar.gz


## DEBUG 环境
作者基于 macOS + VSCode 进行编译调试，也可用 CLion 等其他 IDE。下面两篇文章可供参考：
- [MySQL 源码阅读 —— macOS VSCode 编译调试 MySQL 5.7](https://shockerli.net/post/mysql-source-macos-vscode-debug-5-7/)
- [MySQL 源码阅读 —— macOS CLion 编译调试 MySQL 5.7](https://shockerli.net/post/mysql-source-macos-clion-debug-5-7/)


## 注释说明
- 本项目不修改任何源码，仅添加注释及理解说明
- 文本注释无法描述清楚，将会以文章的形式发布，并将链接附到相关源码中
- 特殊原因对源码文件进行改动的将会在此处列出原因

### BREAKPOINT
对于关键断点，会在注释中以 `BREAKPOINT` 标记📌，可直接搜索该标记以帮助 DEBUG 代码。


### 源码变动日志
- 重命名 `VERSION` 文件及相关 `CMAKE`，原因及方案见 [MySQL 源码阅读 —— 问题 expanded from macro MYSQL_VERSION_MAJOR](https://shockerli.net/post/mysql-source-version-conflict-in-cpp-11/)


## 参考资料
- [MySQL 5.7 Reference Manual](https://dev.mysql.com/doc/refman/5.7/en/)
- [MySQL Internals Manual](https://dev.mysql.com/doc/internals/en/)


## 目录文件
MySQL 核心源码主要集中在 `sql` 和 `storage/innobase`，其他文件无需太过关注。


## Commit 规范
Git Commit 注释请以下方的要求为前缀、且必须清晰的说明本次提交修改的内容，内容较多建议分开提交。

- `(SRC)`: 修改源码
- `(COM)`: 完善注释
- `(OTH)`: 其他提交

