// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_RANDOM_H_
#define STORAGE_LEVELDB_UTIL_RANDOM_H_

#include <stdint.h>

namespace leveldb {

//C语言中伪随机数生成算法实际上是采用了"线性同余法"。具体的计算如下： 
//seed = (seed * A + C ) % M
//其中A,C,M都是常数（一般会取质数）。当C=0时，叫做乘同余法。

//公式1：等式(x<<31)%M == x成立。其中M等于2^31-1
//计算表达式左边(x << 31) % M，由于x<<31等于x*2^31,
//则(x << 31) % M=(x*2^31)%M=(x + x*(2^31-1))%M=(x + x*M)%M=x

//公式2：等式(product%M) == (product>>31)+(product&M)，其中M等于2^31-1

// A very simple random number generator.  Not especially good at
// generating truly random bits, but good enough for our needs in this
// package.
class Random {
 private:
  uint32_t seed_;

 public:
  // 0x7fffffffu == 2147483647L == 2^31-1 == 01111111 11111111 11111111 11111111
  // 表达式s & 0x7fffffffu，确保结果值在[0,2147483647]范围内
  explicit Random(uint32_t s) : seed_(s & 0x7fffffffu) {
    // Avoid bad seeds.
    if (seed_ == 0 || seed_ == 2147483647L) {
      seed_ = 1;
    }
  }
  uint32_t Next() {
    static const uint32_t M = 2147483647L;  // 2^31-1
    static const uint64_t A = 16807;        // bits 14, 8, 7, 5, 2, 1, 0
    // We are computing
    //       seed_ = (seed_ * A) % M,    where M = 2^31-1
    //
    // seed_ must not be zero or M, or else all subsequent computed values
    // will be zero or M respectively.  For all other values, seed_ will end
    // up cycling through every number in [1,M-1]
    uint64_t product = seed_ * A;

    // Compute (product % M) using the fact that ((x << 31) % M) == x.
    seed_ = static_cast<uint32_t>((product >> 31) + (product & M));
    // The first reduction may overflow by 1 bit, so we may need to
    // repeat.  mod == M is not possible; using > allows the faster
    // sign-bit-based test.
    if (seed_ > M) {
      seed_ -= M;
    }
    return seed_;
  }
  // Returns a uniformly distributed value in the range [0..n-1]
  // REQUIRES: n > 0
  uint32_t Uniform(int n) { return Next() % n; }

  // Randomly returns true ~"1/n" of the time, and false otherwise.
  // REQUIRES: n > 0
  bool OneIn(int n) { return (Next() % n) == 0; }

  // Skewed: pick "base" uniformly from range [0,max_log] and then
  // return "base" random bits.  The effect is to pick a number in the
  // range [0,2^max_log-1] with exponential bias towards smaller numbers.
  uint32_t Skewed(int max_log) { return Uniform(1 << Uniform(max_log + 1)); }
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_RANDOM_H_
