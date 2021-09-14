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


## 参考资料
- [MySQL 5.7 Reference Manual](https://dev.mysql.com/doc/refman/5.7/en/)
- [MySQL Internals Manual](https://dev.mysql.com/doc/internals/en/)


## 目录文件
MySQL 核心源码主要集中在 `sql` 和 `storage/innobase`，其他文件无需太过关注。

