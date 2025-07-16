---
title: Vibe Coding 小记
date: 2025/07/16
updated: 2025/07/16
mathjax: true
tags: [AI]
---

## 概述

> 其后，京兆尹将饰官署，余往过焉。委群材，会众工。或执斧斤，或执刀锯，皆环立向之。梓人左持引，右执杖，而中处焉。量栋宇之任，视木之能举，挥其杖曰：“斧！”彼执斧者奔而右；顾而指曰：“锯！”彼执锯者趋而左。俄而斤者斫，刀者削，皆视其色，俟其言，莫敢自断者。其不胜任者，怒而退之，亦莫敢愠焉。画宫于堵，盈尺而曲尽其制，计其毫厘而构大厦，无进退焉。既成，书于上栋，曰“某年某月某日某建”，则其姓字也。凡执用之工不在列。余圜视大骇，然后知其术之工大矣。<br>——《梓人传》柳宗元

我花了一周的时间，用 [Claude Code](https://www.anthropic.com/claude-code) 从头搓了一个[前端 app](https://github.com/alumkal/cob-planner)。花在项目上的时间大约有 40h。代码总计 14k 行，去除单元测试、注释和空行后约 6k 行。项目的所有代码都是 Claude Code 生成的，我只提供了约 2000 字的初始项目描述和后续开发过程中的 prompt。

总体来说，Claude Code 的表现相当不错，我最担心的界面美观性对它而言其实不是问题。我观察到的比较明显的缺点有：

- Token 用量过大：通过 API 高强度使用 Claude Code 的话花费能达到 $5/h 级别，这已经和人类实习生的工资在同一个数量级了。
- 重构能力较弱：对于较大的项目，Claude Code 很难在重构时追踪到所有需要修改的文件，导致重构很难一遍过，需要反复修正。或许换个 prompt 能好点。
- 处理复杂功能时表现不佳：我本来想让 Claude Code 实现一个 drag & drop，但它死活写不对，只好先放弃了。

瑕不掩瑜，强烈建议需要大量写代码（尤其是前后端、CLI等典型场景）的人试一试 Claude Code。Claude Pro $20/mo 的订阅价格和它的功能相比非常划算。目前 Claude Pro 提供的额度还是很慷慨的，大概能支撑你每 5h 高强度使用（上一条恢复后立刻开始下一条）2h。低强度使用的话根本不需要担心额度问题。

## 小技巧

以下是我使用 Claude Code 的过程中摸索出来的一些小技巧。

### 通知

Claude Code 执行一个任务可能需要几分钟，这段时间一直盯着终端有点浪费时间。可以加个 hook，让它在请求权限或任务完成时发送通知。把以下内容写到 `~/.claude/settings.json` 中即可。（非 Linux 系统请自行修改命令）

```json
{
  "hooks": {
    "Notification": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "jq -r \"\\\"notify-send -a 'Claude Code' 'Notification' '\\(.message)'\\\"\" | bash"
          }
        ]
      }
    ],
    "Stop": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "notify-send -a 'Claude Code' 'Task Finished' 'Claude is waiting for your input'"
          }
        ]
      }
    ]
  }
}
```

### Playwright MCP

[Playwright MCP](https://github.com/microsoft/playwright-mcp) 可以让 Claude Code 以 a11y tree 的形式访问网页并操作，这样它就可以自动测试网页了。执行以下命令以添加：

```bash
claude mcp add playwright -- npx @playwright/mcp@latest --executable-path /usr/bin/chromium --isolated --headless
```

浏览复杂网页时 token 用量不小，按量计费时需要注意。

### Think

在 Claude Code 中，思维链模式需要通过特定关键词触发。

> These specific phrases are mapped directly to increasing levels of thinking budget in the system: "think" < "think hard" < "think harder" < "ultrathink." Each level allocates progressively more thinking budget for Claude to use.

在实现复杂功能之前，开启 plan mode 并加入 `ultrathink` 关键词可以让 Claude Code 先思考出一个详细的计划。

### 中途输入

你可以在任何时刻向 Claude Code 提供输入，它会在完成下一次工具交互后读入这些内容。比如你发现它犯错后，你可以及时纠正，而不需停止整个任务。

### 继续对话

`claude --resume` 可以选择一个之前的对话继续进行。`claude --continue` 会继续最近的对话。

过长的对话会增大 token 用量。适时清空上下文或者使用 `/compact` 命令可以减少花费。

## 软件工程

TBD
