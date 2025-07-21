---
title: Lean 4 初体验
date: 2025/07/21
updated: 2025/07/21
mathjax: true
tags: [数学, Lean]
---

第一次不依赖大模型用 Lean 证明了一个没那么显然的定理，在此记录一下。写得很 naive，希望将来自己的水平能有所长进吧。

题目：设 $d(n)$ 表示自然数 $n$ 在十进制下的各位数字之和。证明：对于任意自然数 $n,\,k$，若 $1 \le n \le 10^k$，则有 $d\left(\left(10^k - 1\right) \cdot n\right) = 9k$.

思路：把 $\left(10^k - 1\right) \cdot n$ 拆分为高 $k$ 位和低 $k$ 位两部分。高 $k$ 位的值为 $n - 1$，低 $k$ 位的值为 $10^k - n$。注意到 $(n - 1) + (10^k - n) = 10^k - 1$，因此这两部分的各位数字互补，每一位的数字之和都为 $9$。因此总和为 $9k$。

代码如下：

```lean
import Mathlib

def digitSum (n : ℕ) := (Nat.digits 10 n).sum

theorem digitSum_divmod_10 (n : ℕ) :
  digitSum n = digitSum (n / 10) + (n % 10) := by
  by_cases hn : n = 0
  · simp [hn]
  · simp [digitSum]
    rw [Nat.digits_eq_cons_digits_div (by decide) hn]
    simp [add_comm]

theorem digitSum_eq_self_iff (n : ℕ)
  : n < 10 ↔ digitSum n = n := by
  constructor
  · intro hn
    by_cases hn2 : n = 0
    · simp [digitSum, hn2]
    · simp [digitSum, Nat.digits_of_lt _ _ hn2 hn]
  · contrapose!
    intro hn
    apply Nat.ne_of_lt
    rw [digitSum_divmod_10]
    nth_rw 3 [← Nat.div_add_mod n 10]
    calc digitSum (n / 10) + n % 10
      _ ≤ n / 10 + n % 10 := by simp [digitSum, Nat.digit_sum_le]
      _ < 10 * (n / 10) + n % 10 := by
        have : n / 10 > 0 := Nat.div_pos hn (by decide)
        linarith

theorem digitSum_divmod_power_of_10 (n k : ℕ)
  : digitSum n = digitSum (n / 10^k) + digitSum (n % 10^k) := by
  induction k generalizing n with
  | zero => norm_num; simp [Nat.mod_one, digitSum]
  | succ k ih =>
    -- LHS: digitSum n
    -- digitSum n = digitSum n/10^k + digitSum n%10^k
    -- digitSum n/10^k = digitSum n/10^(k+1) + n/10^k % 10
    rw [ih, digitSum_divmod_10]
    rw [Nat.div_div_eq_div_mul, ← Nat.pow_add_one, add_assoc]
    -- RHS: digitSum n/10^(k+1) + digitSum n%10^(k+1)
    -- digitSum n%10^(k+1) = digitSum n%10^(k+1) / 10^k + digitSum n%10^k
    -- n%10^(k+1) / 10^k = n/10^k % 10
    nth_rw 4 [ih]
    have : 10 ^ k ∣ 10 ^ (k + 1) := Nat.pow_dvd_pow _ (Nat.le_add_right _ _)
    rw [Nat.mod_mod_of_dvd _ this]
    rw [pow_succ, Nat.mod_mul_right_div_self]
    nth_rw 4 [(digitSum_eq_self_iff _).mp]
    exact Nat.mod_lt _ (by decide)

lemma complement_divmod_10 {a b k : ℕ} :
  a + b = 10 ^ (k + 1) - 1 ↔ a / 10 + b / 10 = 10 ^ k - 1 ∧ a % 10 + b % 10 = 9 := by
  constructor
  · intro h
    have h2: (a % 10 + b % 10) % 10 = 9 := by
      rw [← Nat.add_mod, h]
      induction k with
      | zero => simp
      | succ k ih =>
        rw [Nat.pow_succ, Nat.mod_eq_sub_iff (c := 1) (by decide) (by decide)]
        rw [← Nat.pow_succ, Nat.sub_add_cancel (Nat.one_le_pow _ _ (by decide))]
        apply Nat.dvd_mul_left
    omega
  · rintro ⟨ha, hb⟩
    rw [← Nat.div_add_mod' a 10, ← Nat.div_add_mod' b 10]
    have : (a / 10 * 10 + a % 10) + (b / 10 * 10 + b % 10)
      = (a / 10 + b / 10) * 10 + (a % 10 + b % 10) := by ring
    rw [this, ha, hb]
    rw [Nat.sub_mul, ← Nat.pow_succ]
    have : 10 ≤ 10 ^ (k + 1) := by apply Nat.le_pow; simp
    rw [← Nat.sub_add_comm this, Nat.add_sub_add_right]

lemma digitSum_eq_9k_of_complement {a b k : ℕ}
  (h : a + b = 10 ^ k - 1) :
  digitSum a + digitSum b = 9 * k := by
  induction k generalizing a b with
  | zero => norm_num at *; simp [digitSum, h]
  | succ k ih =>
    rw [complement_divmod_10] at h
    rw [digitSum_divmod_10 a, digitSum_divmod_10 b]
    rw [mul_add_one, ← ih (h.left), ← h.right]
    ac_rfl

-- For all natural number n, k such that 1 ≤ n ≤ 10^k, show that the digit sum of (10^k - 1)n is 9k.
example (k : ℕ) (n : ℕ) (hn : 1 ≤ n ∧ n <= 10 ^ k) :
  digitSum ((10 ^ k - 1) * n) = 9 * k := by
  -- (10^k - 1) * n = 10^k * n - n = (n - 1) * 10^k + (10^k - n)
  -- divide the digits into two parts：
  -- 1. digitSum ((n - 1) * 10^k) = digitSum (n - 1)
  -- 2. digitSum (10^k - n) = digitSum ((10^k - 1) - (n - 1)) = 9 * k - digitSum (n - 1)
  let x := (10^k - 1) * n
  have hSplit : x / 10^k = n - 1 ∧ x % 10^k = 10^k - n := by
    rw [Nat.div_mod_unique (Nat.pow_pos (by decide))]
    constructor
    · dsimp [x]
      zify [hn.left, hn.right, show 1 ≤ 10^k from Nat.one_le_pow _ _ (by decide)]
      ring
    · exact Nat.sub_lt (Nat.pow_pos (by decide)) hn.left
  rw [digitSum_divmod_power_of_10 _ k, hSplit.left, hSplit.right]
  apply digitSum_eq_9k_of_complement
  rw [← Nat.sub_add_comm hn.left, Nat.add_sub_of_le hn.right]
```
