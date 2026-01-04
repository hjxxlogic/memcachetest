# memcachetest

## 目的

该工程用于对比不同缓存属性内存的访问性能：

- `WB` (write-back)
- `UC` (uncached)
- `WC` (write-combining)

内核态模块负责分配一块物理页内存，并通过 `mmap` 以不同的 page attribute 导出到用户态；用户态程序对每种映射进行读写带宽测试，并提供多种“写完成/排序”方式（`sfence`/UC-write fence）以及 `movnt`(non-temporal store) 写测试。

## 目录结构

- `kmod/`
  - `memcache_test.c`：内核模块，分配内存并创建设备节点 `/dev/memcache_wb|uc|wc`，支持 `mmap`。
  - `Makefile`：编译内核模块。
- `user/`
  - `cache_bench.c`：用户态 benchmark。
  - `Makefile`：编译 benchmark。

## 环境要求

- Linux 内核头文件（用于编译内核模块）
- `gcc`（用于编译用户态程序）
- 需要 root 权限加载模块/访问 `/dev/memcache_*`

## 编译与运行

### 1. 编译内核模块

```bash
make -C kmod
```

### 2. 加载模块

默认分配 16MB，可通过 `size_mb` 参数指定。

```bash
sudo insmod kmod/memcache_test.ko size_mb=16
```

可选参数：

- `numa_node`：指定页分配所在 NUMA node（默认 `-1` 表示内核默认策略）。例如：

```bash
sudo insmod kmod/memcache_test.ko size_mb=16 numa_node=0
```

设备节点：

- `/dev/memcache_wb`
- `/dev/memcache_uc`
- `/dev/memcache_wc`

查看内核日志（包含分配大小与 mmap 请求大小）：

```bash
dmesg | tail -n 100
```

卸载模块：

```bash
sudo rmmod memcache_test
```

### 3. 编译用户态程序

```bash
make -C user
```

### 4. 运行测试

```bash
sudo user/cache_bench
```

参数：

- `-s <size_mb>`：指定 mmap 大小（MB）。不指定则通过 ioctl 从模块获取。
- `-i <iters>`：迭代次数（默认 50）。
- `-c <cpu>`：绑定到指定 CPU（x86 上默认绑定 CPU0）。

## Benchmark 说明

对每种设备映射，测试项包括：

- `write`：普通 store 写入，每轮写入不同的值，并在每轮结束校验 sum 正确性。
- `write_nofence`：普通 store 写入，不使用任何 fence，每轮写入后校验。
- `write_ucfence`：不使用 `sfence`，每轮写入后对 UC 区域写入一个 fence word（并读回）作为排序/排空手段，然后校验。
- `ntwrite`：x86 上使用 `movntdq`（SSE2 16-byte streaming store）进行 non-temporal store 写入，每轮结束 `sfence`，然后校验。
- `ntwrite_nofence`：`movntdq` 写入，不使用任何 fence，每轮写入后校验。
- `ntwrite_nofence_deferred`：`movntdq` 写入，不使用任何 fence，并将校验延后到所有迭代写完后再做一次。
- `ntwrite_ucfence`：`movntdq` 写入后使用 UC-write fence，然后校验。
- `read`：顺序读取求和带宽。

说明：

- UC-write fence 使用 `/dev/memcache_uc` 的一页作为 fence word。由于驱动不支持带 offset 的 `mmap`，该页与 UC 被测区域可能存在物理重叠；为保证校验正确，UC 的 `*_ucfence` 测试会跳过该 fence word 对应的一个 64-bit 元素，不参与写入/求和/期望值。

## 示例运行结果

以下为一次运行输出示例：

```text
pinned to cpu 0
/dev/memcache_wb size: 16777216 bytes (16.00 MiB) source=ioctl
/dev/memcache_wb write verify: ok
/dev/memcache_wb write: 14605.71 MB/s (0.055 s)
/dev/memcache_wb write_nofence verify: ok
/dev/memcache_wb write_nofence: 12493.88 MB/s (0.064 s)
/dev/memcache_wb write_ucfence verify: ok
/dev/memcache_wb write_ucfence: 11718.75 MB/s (0.068 s)
/dev/memcache_wb ntwrite verify: ok
/dev/memcache_wb ntwrite: 20481.37 MB/s (0.039 s)
/dev/memcache_wb ntwrite_nofence verify: ok
/dev/memcache_wb ntwrite_nofence: 20584.17 MB/s (0.039 s)
/dev/memcache_wb ntwrite_nofence_deferred verify: ok
/dev/memcache_wb ntwrite_nofence_deferred: 20351.10 MB/s (0.039 s)
/dev/memcache_wb ntwrite_ucfence verify: ok
/dev/memcache_wb ntwrite_ucfence: 13888.57 MB/s (0.058 s)
/dev/memcache_wb read : 13198.05 MB/s (0.061 s) sum=0x660135300000
/dev/memcache_uc size: 16777216 bytes (16.00 MiB) source=ioctl
/dev/memcache_uc write verify: ok
/dev/memcache_uc write: 228.20 MB/s (3.506 s)
/dev/memcache_uc write_nofence verify: ok
/dev/memcache_uc write_nofence: 226.73 MB/s (3.528 s)
/dev/memcache_uc write_ucfence verify: ok
/dev/memcache_uc write_ucfence: 229.38 MB/s (3.488 s)
/dev/memcache_uc ntwrite verify: ok
/dev/memcache_uc ntwrite: 465.89 MB/s (1.717 s)
/dev/memcache_uc ntwrite_nofence verify: ok
/dev/memcache_uc ntwrite_nofence: 464.60 MB/s (1.722 s)
/dev/memcache_uc ntwrite_nofence_deferred verify: ok
/dev/memcache_uc ntwrite_nofence_deferred: 462.71 MB/s (1.729 s)
/dev/memcache_uc ntwrite_ucfence verify: ok
/dev/memcache_uc ntwrite_ucfence: 464.41 MB/s (1.723 s)
/dev/memcache_uc read : 95.72 MB/s (8.357 s) sum=0x6601352f9a02
/dev/memcache_wc size: 16777216 bytes (16.00 MiB) source=ioctl
/dev/memcache_wc write verify: ok
/dev/memcache_wc write: 25622.50 MB/s (0.031 s)
/dev/memcache_wc write_nofence verify: ok
/dev/memcache_wc write_nofence: 25378.19 MB/s (0.032 s)
/dev/memcache_wc write_ucfence verify: ok
/dev/memcache_wc write_ucfence: 14752.77 MB/s (0.054 s)
/dev/memcache_wc ntwrite verify: ok
/dev/memcache_wc ntwrite: 20700.59 MB/s (0.039 s)
/dev/memcache_wc ntwrite_nofence verify: ok
/dev/memcache_wc ntwrite_nofence: 21187.27 MB/s (0.038 s)
/dev/memcache_wc ntwrite_nofence_deferred verify: ok
/dev/memcache_wc ntwrite_nofence_deferred: 20781.79 MB/s (0.038 s)
/dev/memcache_wc ntwrite_ucfence verify: ok
/dev/memcache_wc ntwrite_ucfence: 15648.83 MB/s (0.051 s)
/dev/memcache_wc read : 104.03 MB/s (7.690 s) sum=0x660135300000
```

### 结果汇总表（MB/s）

| Memory type | write | write_nofence | write_ucfence | ntwrite | ntwrite_nofence | ntwrite_ucfence | read |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| WB | 14605.71 | 12493.88 | 11718.75 | 20481.37 | 20584.17 | 13888.57 | 13198.05 |
| UC | 228.20 | 226.73 | 229.38 | 465.89 | 464.60 | 464.41 | 95.72 |
| WC | 25622.50 | 25378.19 | 14752.77 | 20700.59 | 21187.27 | 15648.83 | 104.03 |

备注：

- `write`：普通 store + `sfence`（每轮结束）。
- `write_ucfence`：普通 store + UC-write fence（每轮结束，写 UC fence word 并读回）。
- `write_nofence`：普通 store（无 fence）。
- `ntwrite`：`movntdq` + `sfence`（每轮结束）。
- `ntwrite_nofence`：`movntdq`（无 fence）。
- `ntwrite_ucfence`：`movntdq` + UC-write fence（每轮结束）。

### 结果分析

#### 1) UC 读写最慢且最稳定

- UC 的写带宽约 `~220-233 MB/s`，读带宽约 `~105 MB/s`，四种写方式差异很小。
- 原因：UC 映射不使用 CPU cache，加上强顺序/不可合并等属性限制，性能主要受总线/内存系统与访问语义约束。

#### 2) WC 写入明显快于 WB（在本测试口径下）

- WC 的 `write/ntwrite` 写带宽达到 `~23-26 GB/s`，高于 WB 的 `~14 GB/s`。
- 原因：WC 允许 write-combining，连续写可在 CPU/uncore 侧合并成更大粒度事务，提升写吞吐。
- 注意：这里测到的是“每轮写完后做 fence 的写完成口径”，但 WC 的 write combining 行为仍可能使其看起来非常快；不同平台/内存类型下数字可能变化。

#### 3) UC-write fence 会显著拉低 WB/WC 的写带宽

- WB：`write_ucfence` (9995 MB/s) < `write` (14382 MB/s)
- WC：`write_ucfence` (14466 MB/s) < `write` (25918 MB/s)
- 原因：UC fence 本身是一笔非常慢的 UC 访问，每轮一次会成为强制的序列化点/额外开销，限制整体吞吐。

#### 3.1) UC 写作为 fence（UC-write fence）机制分析

UC-write fence 的做法是：在每轮写入结束后，额外对一段 UC 映射地址写入一个 fence word，并读回一次。

工程中对应测试项：

- `write_ucfence`
- `ntwrite_ucfence`

为什么它“像 fence”：

- UC 访问通常具有更强的顺序性要求，且不会被 CPU cache 隐藏。
- 当执行到一次 UC store/load 时，CPU 往往需要先把之前的写缓冲（包括 WC 的 posted/合并写）推进到一个更“全局可见”的点，才能继续后续的 UC 访问（具体依赖架构与实现）。
- 因此 UC-write fence 常被用作一种“通过慢路径访问迫使前序写收敛/排序”的手段。

为什么它会显著降低吞吐：

- UC 写入/读回本身非常慢（在本例 UC 写约 200MB/s 级别）。
- 把这类慢访问放在每轮（每 16MB）一次，会成为固定开销 + 强制序列化点，从而明显拉低 WB/WC 的可持续写带宽。

与 `sfence` 的差异（重要）：

- `sfence` 是指令级的内存序列化/可见性保证（针对 store 屏障），开销相对可控。
- UC-write fence 是“利用 UC 访问语义”间接达成排序，属于经验性/平台相关手段：
  - 并不保证在所有平台上等价于 `sfence`。
  - 其效果可能更强或更弱，且性能开销通常更大。

实现注意事项（本工程的坑）：

- 我们的驱动不支持带 offset 的 `mmap`，因此如果从 `/dev/memcache_uc` 额外 `mmap` 一页作为 fence word，它可能与 UC 的被测映射物理重叠，导致 fence 写污染被测数据。
- 为避免这个问题，工程对 UC 的 `*_ucfence` 测试会跳过 fence word 对应的一个 64-bit 元素：
  - 写入时跳过
  - 求和校验时跳过
  - 期望值计算中扣除该项

#### 4) movnt（non-temporal store）并不总是更快

- 在 WB：`ntwrite` 与 `write` 接近。
- 在 WC：`ntwrite` 略低于 `write`。
- 原因：`movnt` 的优势通常体现在“流式写入且避免污染 cache”的场景；在 WC 映射上写合并已经很强，`movnt` 不一定带来收益。
- 备注：当前实现使用 `movntdq`（SSE2 16-byte streaming store）。在 UC 映射上，`ntwrite` 往往会明显快于普通 `write`，这是因为写入粒度更大、更适合总线事务。

#### 5) UC 的 read sum 与 WB/WC 不一致属于预期

示例输出里 UC 的 `read` sum 为：

- UC：`sum=0x6601352f9a02`
- WB/WC：`sum=0x660135300000`

原因：UC 的 `*_ucfence` 测试为了避免与 UC fence word 的物理重叠污染校验，会跳过 fence word 对应的一个 64-bit 元素不写入/不求和/期望值也扣除该项。因此最终 buffer 中该元素可能保留了旧值，导致最终 `read` 的全量求和与 WB/WC 不同。
