---
title: 实用工具
date: 2025/1/19
updated: 2025/1/19
---

收录了一些博主喜欢的实用工具。本页面不定期更新。

## 计算

### [Qalculate!](https://qalculate.github.io/index.html)

功能非常全面且设计合理的 PC 端计算器。（高级计算器和编程语言之间并没有明确的区隔；由于它很重视 fuzzy parsing，所以我还是将其算作计算器）

支持的特性非常多，比如大量非初等函数、简单的符号计算、不确定度传播、单位转换等等。详见[官方示例](https://qalculate.github.io/manual/qalculate-examples.html)。

![出自 [qalculate.github.io](https://qalculate.github.io/screenshots.html)](../assets/utilities/qalculate-history.png)

### [IPython](https://ipython.org/)

博主在科学计算方面基本只会 Python 生态，会用 MATLAB 或 Mathematica 的人可以忽略此节。

Python 自带 REPL 的上位。支持保存历史记录、语法高亮~~以及用·`exit` 而非 `exit()` 退出~~等。其中一部分功能也添加到了 Python 3.13 的[新 REPL](https://docs.python.org/3/whatsnew/3.13.html#a-better-interactive-interpreter)中。

IPython 还支持一些拓展语法。博主比较常用的有：

- 按 Ctrl+R 可以搜索历史记录
- 函数/类名后加 `?` 可以查看其帮助文档
- Tab 补全可以补全目录/文件名
- 全局变量 `_` 可以访问上一个返回值，`Out[i]` 可以返回第 i 个语句（块）的返回值

### [Plotly](https://plotly.com/python/)

比 Matplotlib 更现代的 Python 绘图库。这个库视 DataFrame 为一等公民，而且默认就能生成美观的交互式图表，适合 prototyping。

美中不足的是深度定制样式还是不如 Matplotlib 方便，毕竟后者资料太多了。而且现在有大模型。
