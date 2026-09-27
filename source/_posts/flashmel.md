---
title: FlashMel：GPU 上最快的 mel 频谱实现 
date: 2026/09/27
updated: 2026/09/27
mathjax: true
tags: [HPC]
---

## 前言

这项工作在大约三个月前完成，但我迟迟懒于公布，大概是因为它“食之无味，弃之可惜”吧。草草收个尾好了。 

Mel 频谱是绝大部分深度学习算法处理音频的第一步，使用广泛，但由于这一步在总体耗时中的占比过小，一直没有人进行优化。这正好是一个用来练习算子开发技能的机会。

FlashMel 的初版由我手工设计（这大概会是我手工设计的最后一个算子了），但 Claude Fable 5 发布之后，我发现仅仅加以几句点拨，agent 就能从头实现一个性能超过我 10%，逼近此方法的硬件上限的版本。我顿时失去了对这个项目的兴趣，连续几个月没再碰它。

我个人的叙述就到此为止；以下内容由 Claude Opus 5.5 撰写。 

## 介绍

[FlashMel](https://github.com/AlumKal/flashmel) 是一个 CUDA 实现的 mel 频谱图（mel spectrogram）算子，接口与 `torchaudio.transforms.MelSpectrogram` 相同，可以直接替换。它把分帧、补边、加窗、实数 FFT、取模平方和 mel 滤波器组投影合并到一次 kernel launch 里。在 RTX 4070 Laptop 上，它比 torchaudio 快 5 到 16 倍，fp32 输出与 torchaudio 的误差在 `atol = rtol = 1e-4` 以内。

名字借自 FlashAttention，两者的出发点相同：不把中间结果写回显存（HBM）。不同之处在于，attention 的中间矩阵在行方向上相互依赖，需要 online softmax 这类技巧才能分块计算；mel 频谱图的各帧彼此独立，融合本身没有算法上的障碍。真正的问题出现在融合之后：一个 kernel 里同时有访存、FFT 和稀疏归约，瓶颈落在哪一处，取决于参数。

本文先简单介绍 mel 频谱图和 FlashMel 的数据流，然后以 Whisper 的配置为例，看每一步优化分别解决了哪个瓶颈，最后看换到其他 FFT 尺寸时瓶颈如何变化。阅读本文只需要熟悉 CUDA 编程模型，不需要音频背景。

## mel 频谱图

音频是一维的采样序列。mel 频谱图的计算分四步：

1. 分帧：每隔 $H$ 个采样取一段长为 $N$ 的窗口。$N$ 即参数 `n_fft`，$H$ 即 `hop_length`。Whisper 使用 16 kHz 采样，$N = 400$（25 ms），$H = 160$（10 ms），相邻帧重叠 60%。`center=True` 时，信号两端先各补 $N/2$ 个采样，默认用反射（reflect）方式补齐。
2. 加窗并做实数 FFT：每帧乘以 Hann 窗后做 FFT，得到 $N/2+1$ 个频点。
3. 取功率：每个频点取 $|X|^2$。
4. mel 投影：用 $M$ 个三角形滤波器（Whisper 为 128 个）对功率谱加权求和。

写成公式：

$$
Y[m, t] = \sum_{k=0}^{N/2} F[m,k]\,\Bigl|\sum_{n=0}^{N-1} w[n]\,x[tH + n - N/2]\,e^{-2\pi i kn/N}\Bigr|^2
$$

从计算的角度，有三点值得注意：

- 帧与帧互相独立，天然适合并行。
- 滤波器矩阵 $F$ 的形状是 $M \times (N/2+1)$，但每一行只在一段连续区间上非零，所以投影实际上是一组很短的稀疏点积。
- 算术强度（arithmetic intensity）很低。以 Whisper 为例，每帧只需要读入 $H = 160$ 个新采样（640 B），写出 128 个 mel 值（512 B）；一次 400 点 FFT 按 $5N\log_2 N$ 估算约为 1.7 万次浮点运算，合约 15 FLOP/B。按这张卡的标称 FP32 峰值估算，roofline 的拐点（ridge point）在 100 FLOP/B 左右。因此，至少在理论上，这个算子应当受访存带宽限制。

## 一个 kernel 完成全部计算

torchaudio 的实现由 `torch.stft` 和一次矩阵乘组成，中间的复数频谱和功率谱都要完整写回 HBM，再由下一步读入。FlashMel 用 [cuFFTDx](https://docs.nvidia.com/cuda/cufftdx/) 把所有步骤合并进一个 kernel。cuFFTDx 是 NVIDIA 的设备端 FFT 库：FFT 由一个 block 内的线程协作完成，输入和输出都放在寄存器里，所以 FFT 前后可以直接接自定义代码。cuFFT 做不到这一点，它只能从主机端 launch，输入输出都必须在显存里。

cuFFTDx 的 FFT 尺寸、每个 block 处理的帧数（下文记作 FPB，frames per block）等参数都是编译期常量。因此 FlashMel 在运行时用 NVRTC 为每种参数组合单独编译 kernel，并把 cubin 缓存在磁盘上，首次使用某个配置约需 2 秒。

一个 block 负责同一条音频上连续的 FPB 帧，数据流如下：

```mermaid
flowchart LR
    A["HBM：音频采样"] -->|"分帧、补边、加窗"| B["寄存器"]
    B -->|"cuFFTDx block FFT"| C["寄存器：频谱"]
    C -->|"取模平方"| D["共享内存：功率谱 tile"]
    D -->|"稀疏 mel 投影"| E["HBM：mel 输出"]
```

有几个细节：

- 补边不会物化。`center` 引入的补边、`pad` 参数以及四种 `pad_mode`，都换算成读入时的下标运算。分帧后的信号从头到尾都不存在于显存中。
- STFT 的 `normalized` 选项只是给窗函数乘一个常数，所以在主机端预先乘进窗函数。
- `n_fft` 为 2 的幂时，使用 cuFFTDx 的 folded 模式：把 $N$ 个实数看作 $N/2$ 个复数，做 $N/2$ 点复数 FFT，共享内存和蝶形运算都减半。400 不是 2 的幂，cuFFTDx 的 folded 模式不支持这个尺寸，只能用 normal 模式，即把实数输入当作虚部为零的复数，做完整的 400 点复数 FFT。后文会看到，这是 Whisper 配置的特殊之处。
- FFT 执行完后，它在共享内存中的工作区不再有用，直接改作功率谱 tile，供投影阶段读取。

最后一点对应的代码如下（简化自 `kernel.cu`）：

```cpp
constexpr unsigned N_FREQS = N_FFT / 2 + 1;
// 所有支持的 n_fft 都是偶数，所以行宽 N_FREQS 必为奇数
constexpr unsigned PSTRIDE = N_FREQS;
static_assert(PSTRIDE % 2 == 1);

FFT().execute(thread_data, reinterpret_cast<complex_type*>(smem_raw));
__syncthreads();

// FFT 的共享内存工作区已经用完，原地改作功率谱 tile
float* prow = power + threadIdx.y * PSTRIDE;
for (unsigned i = 0; i < FFT::output_ept; ++i) {
    const unsigned idx = threadIdx.x + i * FFT::stride;
    if (idx < N_FREQS) prow[idx] = spec_value(thread_data[i]);
}
__syncthreads();
```

tile 的每一行存一帧的功率谱，行宽取 $N/2+1$。因为行宽是奇数，相邻帧的同一频点会落在不同的 bank 上，不需要额外补齐就能避免 bank conflict。奇数行宽还有一个副作用，讲到中等尺寸时会再提。

投影阶段，滤波器以稀疏格式存储：每个滤波器记录起点 `start[m]`、宽度 `width[m]` 和一段长为 `W_MAX` 的权重，`W_MAX` 是最宽滤波器的宽度。线程按（mel 序号，帧序号）分配输出，相邻线程计算同一 mel 行上的相邻帧，所以写回 HBM 时是合并访问（coalesced access）。

## Whisper：从延迟受限到带宽受限

先说明测量方式。所有数据都来自一台 RTX 4070 Laptop（sm_89），实测显存拷贝带宽约 198 GB/s，下文称为拷贝峰值。计时使用 CUDA event，取 50 次的中位数。笔记本的温度会让结果浮动约 15%，所以下面每一步优化的比值都来自同一进程内交替运行的 A/B 对比，不同时间测得的绝对耗时之间不做比较。瓶颈分析使用 Nsight Compute 的 SOL（speed of light）指标，即各硬件单元的吞吐量占其峰值的百分比。

Whisper 的标准配置是：`n_fft = 400`，`hop_length = 160`，128 个 mel，30 秒音频（480000 个采样），batch 为 32。FlashMel 最终耗时 0.54 ms，比 torchaudio 快 11.7 倍。按必要字节计算（输入和输出各计一次，共约 110 MB），有效带宽为 205 GB/s，达到拷贝峰值的 103%。有效带宽之所以能超过拷贝峰值，是因为相邻帧有重叠，每个采样平均被 2.5 帧读取，重复的读取大多命中了 L2，而必要字节只把输入计算一次。

下面看它是怎样走到这一步的。

### 第一步：共享内存限制了占用率

最初每个 block 处理 8 帧。normal 模式要做完整的 400 点复数 FFT，每帧的工作区是 400 × 8 B = 3.2 KB，8 帧共 25.6 KB，每个 SM 只能容纳 3 个 block。Nsight Compute 的结果是：DRAM 43%，SM 35%，占用率（occupancy）31%，限制因素为共享内存。

把 FPB 降到 4 后，每个 block 的工作区减半为 12.8 KB，每个 SM 能容纳 7 个 block，占用率升到约 42%，速度提升 1.14 倍。

但此时 DRAM 和 SM 的 SOL 仍然都只有 47% 到 48%，没有任何一个单元跑满。这是典型的延迟受限（latency-bound）：warp 大部分时间在等待，而能用来掩盖等待的 warp 又太少。占用率的上限由 normal 模式的工作区决定，而 400 点只能使用 normal 模式，所以从占用率入手已经走不下去了。剩下两个方向：减少每帧的计算量，或者减少每个 warp 的等待。

### 失败的尝试：两帧共用一次 FFT

normal 模式浪费了一半的计算量，因为输入的虚部全是零。一个经典的办法是把两帧打包进一次复数 FFT：帧 $a$ 作实部，帧 $b$ 作虚部，即 $z = a + ib$，然后利用共轭对称性把两帧的频谱分开：

$$
A[k] = \frac{Z[k] + \overline{Z[N-k]}}{2},\qquad B[k] = \frac{Z[k] - \overline{Z[N-k]}}{2i}
$$

这样蝶形运算和每帧的工作区都减半了。这个方案实现后通过了正确性测试，但在同一进程内交替运行的 A/B 对比中，它反而慢了约 1.5 倍。

原因有两个。第一，分离频谱时要同时用到 $Z[k]$ 和 $Z[N-k]$，它们通常不在同一个线程的寄存器里，所以完整的 $Z$ 必须先写进共享内存再读出来，多了一次共享内存往返。cuFFTDx 中一次 400 点 FFT 由 20 个线程协作完成，这些线程组会跨越 warp 的边界，所以也无法改用 warp shuffle 交换数据。第二，参与 FFT 的线程数减半，能用来掩盖延迟的并行度也随之减少。对一个延迟受限的 kernel 来说，减少浮点运算本来就帮不上忙，而这个方案还增加了访存、降低了并行度。

### 第二步：让绝大多数帧跳过边界处理

既然瓶颈是等待，就要看 warp 在等什么。原先的实现中，每个采样都要经过 `load_sample`：先处理反射，再减去 `pad` 偏移，再判断是否越界，然后才能算出地址、发出访存。每个采样都要执行这样一串相互依赖的比较和选择，而占用率只有约 41%，没有足够多的 warp 来掩盖这段延迟。

实际上，需要做边界处理的帧非常少。Whisper 配置下，一个帧只有伸出信号两端时才需要处理边界，每条音频的首尾各只有 2 帧，在 3001 帧中只占 4 帧。所以 kernel 改为按整帧判断：只要整帧都落在原始信号内部，就直接读取（简化自 `kernel.cu`，以 fp32 输入、reflect 模式为例）：

```cpp
// 通用路径：每个采样都要做一次边界处理
__device__ float load_sample(const float* x, int v, int L, int n, int pad) {
    v = v < 0 ? -v : v;              // 左端反射
    v = v >= L ? 2 * L - 2 - v : v;  // 右端反射
    v -= pad;
    if (v < 0 || v >= n) return 0.f; // pad 区域补零
    return x[v];
}

// 整帧位于信号内部：直接读取，没有任何逐采样的边界逻辑
if (valid && base >= pad && base + N_FFT <= pad + n_samples) {
    for (unsigned i = 0; i < FFT::input_ept; ++i) {
        const unsigned idx = threadIdx.x + i * FFT::stride;
        reg[i] = idx < FFT::input_length ? xf[idx] * window[idx] : 0.f;
    }
} else {
    for (unsigned i = 0; i < FFT::input_ept; ++i) {
        const unsigned idx = threadIdx.x + i * FFT::stride;
        reg[i] = (valid && idx < FFT::input_length)
                     ? load_sample(x, base + idx, L, n_samples, pad) * window[idx]
                     : 0.f;
    }
}
```

这个判断对每一帧只做一次，只有每条音频首尾的 block 里才会出现分支发散。改动后速度提升 2.0 倍。Nsight Compute 显示 DRAM 77%、L1 80%、SM 74%，其中 77% 的理论 DRAM 带宽大致就是实测的拷贝峰值。也就是说，Whisper 配置此时已经受 DRAM 带宽限制。占用率仍然是 41%，但它已经不再是瓶颈了。

## 换一个尺寸，瓶颈就变了

Whisper 最终停在 DRAM 带宽上，但这只是 FlashMel 所支持的尺寸中的一种情况。`n_fft` 增大时，每帧 FFT 的计算量按 $N\log N$ 增长，每帧读写的字节数大致按 $N$ 增长，所以算术强度随 $\log N$ 缓慢上升；与此同时，block 级 FFT 需要的共享内存和同步次数也在增长。下表是几种典型配置的最终状态：

| 配置 | `n_fft` / `hop` | mel 数 | 相对 torchaudio 的加速比 | 最终瓶颈 |
|---|---|---|---|---|
| small | 64 / 16 | 40 | 5.6× | DRAM |
| Whisper | 400 / 160 | 128 | 11.7× | DRAM |
| speech | 1024 / 256 | 80 | 16.2× | L1 与共享内存 |
| music | 4096 / 1024 | 256 | 12.9× | L1 与共享内存 |
| huge | 16384 / 4096 | 256 | 5.0× | 占用率与 barrier |

### 小尺寸（64 到 512）：DRAM

在 `hop = n_fft / 4` 的扫描中，64 到 512 的有效带宽达到拷贝峰值的 97% 到 120%。原因与 Whisper 相同：每个采样被 4 帧重复读取，重复的部分由 L2 承担。

不过，小尺寸并不是一开始就达到了带宽上限。帧很短时，每字节数据对应的访存指令很多，瓶颈在访存单元（LSU）的指令吞吐上，而不在 DRAM。folded 模式按（偶数位，奇数位）成对消费采样，而这两个采样在内存中正好相邻。因此，对于不涉及边界的帧，信号和窗函数都改用 `float2`（int16 输入则用 `short2`）一次读两个，访存指令数减半，64 点的耗时减少了 30%。这里有一个陷阱：对齐检查必须检查实际的指针，而不是下标的奇偶性。当每条音频的长度为奇数时，每隔一行的起始地址都会偏移 4 字节，只看下标会触发 misaligned address 错误。

### 中等尺寸（1024 到 4096）：L1 与共享内存

这个区间内 DRAM 的 SOL 只有 30% 到 40%，L1 与共享内存则达到 76% 到 81%。

第一次分析 1024 点配置时，L1 与共享内存的 SOL 高达 89%，其中很大一部分来自 mel 投影：每个点积都按 `W_MAX` 的长度读取补零后的整行权重。mel 刻度上，高频滤波器很宽，低频滤波器很窄，而 `W_MAX` 是由最宽的那个决定的，大多数滤波器的实际宽度远小于它。改为按每个滤波器的实际宽度 `width[m]` 循环后，1024 点快了 1.22 倍，4096 点快了 1.39 倍，这是所有优化中单项收益最大的一次。Whisper 的滤波器都很窄（`W_MAX` ≤ 16），所以保留了按 `W_MAX` 完全展开的循环。

剩下的 L1 流量主要来自 cuFFTDx 内部线程间的数据交换，FlashMel 无法控制。自己这边还能想到的优化，比如把投影阶段的共享内存读取向量化，会与前面的奇数行宽冲突：奇数行宽让各行的起始地址无法满足 8 字节或 16 字节对齐，所以避免 bank conflict 与向量化读取只能二选一。把输入先暂存到共享内存的方案同样不可行，因为共享内存和 L1 共用同一条 LSU 流水线，这样做只是把流量从一处挪到另一处，总量并没有减少。

### 大尺寸（8192 和 16384）：占用率与 barrier

这个区间的 FFT 工作区为 33 KB 到 64 KB，每个 SM 只能容纳 1 到 2 个 block。block 级 FFT 内部有多次 `__syncthreads`，每到一次 barrier，整个 SM 上的 warp 几乎都在等待，没有别的 block 可以调度。以 16384 点为例，按前面的方法估算，算术强度约为 30 FLOP/B，仍在拐点左侧，但它既没有跑满 DRAM（有效带宽 33 GB/s），也没有跑满计算单元。roofline 的两条上限都描述不了这种情况，真正限制它的是并行度和同步。

在这个前提下，能做的是不让线程闲着。16384 点的 block 有 1024 个线程，而每个 block 只处理 1 帧，只有 256 个 mel 输出，投影阶段有四分之三的线程无事可做。改为由多个线程（代码中的 `SPLIT`）合作计算一个点积，最后用 `__shfl_down_sync` 做 warp 内归约，16384 点又快了约 12%。外层循环对 warp 内的所有线程保持一致，这样全掩码的 shuffle 始终合法。

再往前走，要么把 FFT 拆成多个 kernel，频谱就必须写回 HBM，融合的前提也就不存在了；要么改用 fp16 旋转因子（twiddle factor），精度又无法满足 `1e-4` 的要求。

## 小结

各尺寸的最终状态如下：

- 64 到 512 以及 400（Whisper）受 DRAM 带宽限制，有效带宽达到或超过拷贝峰值。
- 1024 到 4096 受 L1 与共享内存吞吐限制，SOL 为 76% 到 81%，剩余流量来自 cuFFTDx 内部。
- 8192 和 16384 受占用率和 barrier 限制，继续优化需要放弃单 kernel 结构或 fp32 精度。

目前的限制：`n_fft` 必须是 400 或 64 到 16384 之间的 2 的幂，`n_mels` 不超过 256，窗函数固定为 Hann 窗，只支持推理，不支持反向传播。

{% note default no-icon 彩蛋 %}
让模型用一首押韵诗讲解自己的优化历程。

> Four hundred samples, windowed tight,<br>
> ride registers into the night;<br>
> no frame shall touch the HBM —<br>
> the L2 cache remembers them.
>
> The macro EPT broke the build<br>
> (a template name already filled);<br>
> renamed, recompiled, and then —<br>
> the spectra matched at one-e-minus-ten.
>
> We folded radix, halved the share,<br>
> of memory each block must bear;<br>
> we swept the FPB by twos<br>
> and read what Nsight said to choose.
>
> The paired-FFT, our cleverest scheme,<br>
> drowned in shared memory's stream —<br>
> half the math, yet slower still:<br>
> a tombstone on profiling hill.
>
> But width-bound loops cut traffic deep,<br>
> the mel bins now coalesce in sleep;<br>
> torchaudio takes six millis flat —<br>
> we're done in point-eight. Mind the gap.
>
> So here's to kernels fused as one,<br>
> to tests all green, to sweeps all run;<br>
> the bandwidth roofline, nearly kissed —<br>
> the rest is cuFFTDx's twist.
>
> *(Epilogue, round two)*<br>
> Then back we came with sharper knives:<br>
> let idle threads lead useful lives —<br>
> four lanes per mel, a shuffled sum,<br>
> and sixteen-k stopped looking glum.
>
> The folded pairs, loaded as two,<br>
> cut instructions clean in half — it's true;<br>
> but mind the rows of odd-length sound,<br>
> or misaligned your loads are found.
>
> Now small sizes drink the DRAM dry,<br>
> the middle pins the LSU high,<br>
> the giants wait at barrier walls —<br>
> and that is where the curtain falls.
{% endnote %}
