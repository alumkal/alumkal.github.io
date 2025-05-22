---
title: 黑科技：如何向 Transformers 中的模型传递注意力掩码矩阵
date: 2025/4/9
updated: 2025/4/9
mathjax: true
tags: [AI, 深度学习, LLM]
---

由于 Transformer 本质上是位置无关的，因此通过正确设置 positional embedding 和 attention mask，我们可以把多个“序列”合并到一个序列里进行推理。
当这些序列具有很长的公共前缀时，这种方法会比 batched inference 更高效。这种方法的具体用途不是本文的重点，此处不做展开。

![图片出自 [SpecInfer: Accelerating Generative Large Language Model Serving with Tree-based Speculative Inference and Verification](https://arxiv.org/abs/2305.09781)](../assets/modify-attn-mask/tree-based-parallel-decoding.png)

但是，Transformers 库的自带 [API](https://huggingface.co/docs/transformers/v4.51.1/en/model_doc/llama#transformers.LlamaForCausalLM.forward.attention_mask) 并不支持传递 attention mask 矩阵，而只支持选择是否 mask 掉整个 token（这种 mask 是用来处理 padding 的）。
一种常见的解决方法是直接修改模型的代码，比如 [REST 的实现](https://github.com/FasterDecoding/REST/blob/50a5fc197382ed8df5b3e946dad2f8337511b541/rest/model/modeling_llama_kv.py#L917)。
显然，这种方法在需要测试多种模型时会很麻烦。

有没有更通用一点的方法呢？有的！通过阅读[源码](https://github.com/huggingface/transformers/blob/10baffb599cd32099f1a9780b6569f0e02a0ad80/src/transformers/models/llama/modeling_llama.py#L671C1-L706C41)可以发现，当模型把 2d 的 attention mask（这个参数是从 `forward` 一路传过来的）转化为 4d 时，如果输入已经是 4d 的话，这个函数会直接返回输入。
因此通过这个不在文档里的 API，就可以传递 attention mask 矩阵了。

代码大概是这样的：（注意传入的 mask 值应为 -inf/0 而非 0/1）

```python
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

# Load model
model_name = "Qwen/Qwen2.5-0.5B-Instruct"
model = AutoModelForCausalLM.from_pretrained(model_name, device_map="auto")
tokenizer = AutoTokenizer.from_pretrained(model_name)

# Create test input
message = "You are a are not a"
message_tokenized = tokenizer.encode(message, return_tensors="pt").to(model.device)
assert message_tokenized.shape[-1] == 6

# Create position_ids and attention_mask according to the tree structure
position_ids = torch.Tensor([[0, 1, 2, 1, 2, 2]]).long().to(model.device)
attn_mask_bool = torch.Tensor([[[
    [1, 0, 0, 0, 0, 0],
    [1, 1, 0, 0, 0, 0],
    [1, 1, 1, 0, 0, 0],
    [1, 0, 0, 1, 0, 0],
    [1, 0, 0, 1, 1, 0],
    [1, 0, 0, 1, 0, 1],
]]]).bool()
attn_mask_float = torch.where(attn_mask_bool, 0.0, -torch.inf).to(model.device)
print("position_ids:", position_ids.shape)
print("attn_mask_float:", attn_mask_float.shape)

r'''
树的结构：
        - are ----- a
       /
You ---           - not
       \         /
        - are ---
                 \
                  - a

如果实现正确，两个 "are" 和 "a" 的 logits 应该相同
'''

# Run model
with torch.no_grad():
    outputs = model(
        message_tokenized,
        position_ids=position_ids,
        attention_mask=attn_mask_float
    )
logits = outputs.logits
print("logits:", logits.shape)
print(f"{torch.allclose(logits[0, 1, :], logits[0, 3, :])=}")
print(f"{torch.allclose(logits[0, 2, :], logits[0, 5, :])=}")
```

输出为：

```plain
position_ids: torch.Size([1, 6])
attn_mask_float: torch.Size([1, 1, 6, 6])
logits: torch.Size([1, 6, 151936])
torch.allclose(logits[0, 1, :], logits[0, 3, :])=True
torch.allclose(logits[0, 2, :], logits[0, 5, :])=True
```

这种方法的优点：

- 对大部分 Transformer 模型即插即用
- 不需要深入模型的实现细节

这种方法的缺点：

- 不是官方支持的 API，随时可能失效
- 不支持 Flash Attention 2
