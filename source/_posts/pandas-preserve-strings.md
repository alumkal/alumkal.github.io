---
title: Pandas 读取 CSV 时保留字符串原值
date: 2026/1/13
updated: 2026/1/13
tags: [Python]
---

TLDR: 使用 Pandas 读取 CSV 文件时，最好指定 `keep_default_na=False, na_values=''`。

## 问题

Pandas 的 `read_csv` 默认会把**长得像**无效值的字符串（如 `NA`、`null`、`None` 等）解析为 `NaN`，而这些字符串有时确实是合法内容~~（比如某些不知好歹的人的用户名）~~。这会导致读入 CSV 后重新导出的过程中丢失数据。

Pandas 默认的 NA 值列表可以在[文档](https://pandas.pydata.org/docs/reference/api/pandas.read_csv.html)中找到：

> By default the following values are interpreted as NaN: ` `, `#N/A`, `#N/A N/A`, `#NA`, `-1.#IND`, `-1.#QNAN`, `-NaN`, `-nan`, `1.#IND`, `1.#QNAN`, `<NA>`, `N/A`, `NA`, `NULL`, `NaN`, `None`, `n/a`, `nan`, `null `.

## 解决方案

在 `read_csv` 时指定以下参数：

```python
df = pd.read_csv('data.csv', keep_default_na=False, na_values='')
df.to_csv(index=False)
```

`keep_default_na=False` 的作用是禁用默认的 NA 值列表；`na_values=''` 的作用是把空字符串视为缺失值。如果去掉后者，则所有包含空单元格的列都会被视作字符串类型，对数值类数据不合理。

美中不足的是，并不存在一个参数可以直接将所有字符串列的空值视作 `''`，同时保留数值列的 `NaN`。不过可以通过后续处理来实现：

```python
cols = df.select_dtypes(include=['object']).columns
df.fillna(pd.Series('', index=cols), inplace=True)
```

## 示例

```python
>>> df = pd.read_csv(io.StringIO('str,num\nnull,\n,1')); df
   str  num
0  NaN  NaN
1  NaN  1.0

>>> df = pd.read_csv(io.StringIO('str,num\nnull,\n,1'), keep_default_na=False, na_values=''); df
    str  num
0  null  NaN
1   NaN  1.0

>>> df.fillna(pd.Series('', index=df.select_dtypes(include=['object']).columns))
    str  num
0  null  NaN
1        1.0
```
