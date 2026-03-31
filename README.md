# 台区识别工频频率差算法说明

## 可用性说明

- 2026年3月30日，辽宁台体送检测试通过

## 功能概述
该算法用于馈线台区识别中的辅助归属判定。核心思路不是直接比较单个 `NTB` 时间戳，而是先把 CCO 和 STA 的过零 `NTB` 序列还原为工频周期，再换算成频率，最后比较两端频率是否足够接近。

可以把它理解为下面这条链路：

`过零 NTB 序列 -> 周期差值序列 -> 代表频率 -> CCO/STA 频差 -> 按 CCO MAC 累积 -> 选出最可能归属的 CCO`

算法假设是：同一台区内，CCO 与 STA 感知到的工频频率更接近；不同台区之间，这个频差通常更大。

## 文件结构说明
仓库当前的主要文件结构如下：

```text
.
├── LICENSE
├── README.md
├── app_area_frequency_calc.c
├── app_area_frequency_calc.h
└── tests
    ├── Serial-COM22_03_11_15_16-025021800033成功.log
    ├── Serial-COM7_03_11_15_16-025021800021错误.log
    ├── bin
    │   ├── app_area_frequency_calc_single
    │   └── period_cmp_records.txt
    ├── extract_period_cmp_records.py
    ├── test.sh
    └── 编码规范.md
```

各文件/目录职责如下：

| 路径 | 说明 |
| --- | --- |
| `README.md` | 项目说明文档，介绍算法原理、关键公式和测试方法。 |
| `LICENSE` | 仓库许可证文件。 |
| `app_area_frequency_calc.h` | 频率差判定算法对外声明的头文件，包含宏、数据结构和接口声明。 |
| `app_area_frequency_calc.c` | 核心算法实现文件，包含频率换算、频差记录和归属 CCO 选择逻辑。 |
| `tests/` | 测试与样例数据目录，包含日志样例、抽取脚本和测试入口。 |
| `tests/test.sh` | 一键测试脚本，负责串联“日志抽取 -> 编译测试程序 -> 回放验证”。 |
| `tests/extract_period_cmp_records.py` | 从串口日志中提取 `period_cmp` 记录并转换成测试输入格式的脚本。 |
| `tests/Serial-*.log` | 测试使用的串口日志样例，分别用于成功/异常场景验证。 |
| `tests/bin/` | 测试过程生成或使用的中间产物目录，例如抽取结果和测试可执行文件。 |
| `tests/编码规范.md` | 测试目录下的补充说明文档，不参与算法执行。 |

## 数据来源
生产流程里，频率差算法的输入不是原始波形，而是已经对齐过相位、并从过零点 `NTB` 数据中提取出来的周期差值数组。

主要处理链路如下：

1. `periodinfocollecttimerCB()` 从过零数据缓冲区中抽取指定起点之后的 `NTB` 序列。
2. `area_ind_get_ntb_data_by_start_ntb()` 把相邻过零 `NTB` 的差值转换成 `collect_info` 中的周期差值数组。
3. `deal_period_pma()` 对齐 CCO 和 STA 的同相位数据后，调用 `area_record_frequency_diff()` 记录一次频差。
4. `DealDistinguishResultQuery()` 在需要输出最终归属结果时，调用 `area_get_belonging_cco_mac()` 选出最优的 CCO MAC。

## 周期差值是怎么得到的
`collect_info` 里存放的不是原始周期 Tick，而是相对于理论 `50Hz` 周期的“压缩差值”。

理论上：

- `NTB` 时钟频率为 `25 MHz`
- `1 Tick = 1 / 25,000,000 s = 40 ns`
- `50 Hz` 的工频周期是 `20 ms`
- 所以理论周期对应的 Tick 数为：

```text
20 ms * 25,000,000 Hz = 500,000 Tick
```

代码里把这个值定义为：

```text
AREA_FREQ_THEORETICAL_50HZ_PERIOD_TICKS = 500000
```

对任意两个相邻同类过零点 `NTB[i]` 和 `NTB[i+1]`，先得到真实周期 Tick：

```text
NtbDelta = NTB[i+1] - NTB[i]
```

再编码成周期差值：

```text
PeriodValue = (NtbDelta - 500000) / 8
```

这里除以 `8` 的作用有两个：

1. 把原始 Tick 偏差压缩后存进 `int16_t`
2. 降低微小抖动带来的数值噪声

因此，`PeriodValue` 的 1 个单位表示 `8 Tick`，也就是：

```text
8 * 40 ns = 320 ns
```

在 `50Hz` 附近，`PeriodValue` 每变化 `1`，频率大约变化 `0.0008 Hz`。

## 怎么从 NTB 的差值转换成频率
这部分是整个算法最关键的换算。

### 1. 先从周期差值恢复真实周期 Tick
代码中的恢复公式是：

```text
NtbTicks = PeriodValue * 8 + 500000
```

也就是把前面压缩掉的 `8 Tick` 和理论周期 `500000 Tick` 加回来。

### 2. 再由周期换算频率
频率公式是：

```text
Frequency = 25000000 / NtbTicks
```

其中 `25000000` 就是 `NTB` 时钟频率，单位是 `Hz`。

### 3. 正负号的物理意义
- `PeriodValue = 0`：周期正好是理论 `50Hz`
- `PeriodValue > 0`：真实周期比 `20ms` 更长，频率低于 `50Hz`
- `PeriodValue < 0`：真实周期比 `20ms` 更短，频率高于 `50Hz`

## 直接从 NTB 差值换算频率的例子
### 例 1：理想 `50Hz`
假设两个相邻过零点的差值正好是：

```text
NtbDelta = 500000 Tick
```

那么：

```text
PeriodValue = (500000 - 500000) / 8 = 0
Frequency = 25000000 / (0 * 8 + 500000) = 50.000000 Hz
```

### 例 2：周期稍长，频率低于 `50Hz`
假设：

```text
NtbDelta = 501000 Tick
```

那么：

```text
PeriodValue = (501000 - 500000) / 8 = 125
Frequency = 25000000 / (125 * 8 + 500000)
          = 25000000 / 501000
          ≈ 49.900200 Hz
```

### 例 3：周期稍短，频率高于 `50Hz`
假设：

```text
NtbDelta = 499000 Tick
```

那么：

```text
PeriodValue = (499000 - 500000) / 8 = -125
Frequency = 25000000 / (-125 * 8 + 500000)
          = 25000000 / 499000
          ≈ 50.100200 Hz
```

### 例 4：使用当前算法中的一个实际差值
如果周期差值数组里有一个值是：

```text
PeriodValue = 73
```

先恢复周期 Tick：

```text
NtbTicks = 73 * 8 + 500000 = 500584
```

再换算频率：

```text
Frequency = 25000000 / 500584 ≈ 49.941668 Hz
```

反过来看，这也等价于原始过零 `NTB` 差值约为 `500584 Tick`。

## 单次比较是如何得到“代表频率”的
`area_record_frequency_diff()` 不会直接拿数组平均值，而是分别从 CCO 数组和 STA 数组中提取一个“代表频率”。

处理过程如下：

1. 遍历整个周期差值数组。
2. 先做合法性检查：
   - 任一 `PeriodValue` 的绝对值超过 `2800`，整组数据直接判无效。
   - 恢复后的 `NtbTicks` 不在 `(250000, 2500000)` 之间，当前点无效。
3. 对每个有效点执行 `PeriodValue -> NtbTicks -> Frequency` 换算。
4. 取本组中的最大频率，作为该组的代表频率。

也就是说，当前实现的代表频率是：

```text
RepresentativeFrequency = max(Frequency[i])
```

这一步是实现里的真实行为，不是均值，也不是中位数。

## 单次频差怎么记录
拿到两端代表频率后，算法记录的是绝对频差：

```text
FrequencyDiff = abs(CcoFrequency - StaFrequency)
```

然后按 `CCO MAC` 分桶保存：

1. 每个 CCO MAC 单独维护一个记录项。
2. 每次比较都向该记录项追加一个 `FrequencyDiff`。
3. 每个 MAC 最多保留 `50` 个频差样本。
4. 如果样本已满，则丢弃最旧值，保留最新的 `50` 个。

## 多次样本后如何选出归属 CCO
最终选择不是看某一次最小，而是看某个 CCO MAC 在多次观测下是否持续稳定地更接近 STA。

### 1. 先做样本门限
只有频差样本数不少于 `5` 的 CCO MAC 才有资格参与最终选择。

### 2. 计算该 CCO 的平均频差

```text
AverageDiff = sum(DiffValues) / DiffCount
```

### 3. 计算所有样本的全局平均频差

```text
GlobalAverageDiff = 所有 CCO 的全部 FrequencyDiff 的平均值
```

### 4. 计算选择分数
当前代码使用了一个带先验平滑的分数：

```text
SelectionScore =
    (AverageDiff * DiffCount + GlobalAverageDiff * 5) /
    (DiffCount + 5)
```

这个公式的含义是：

1. 样本数少时，不完全相信该 CCO 自己的平均频差。
2. 先用全局平均值做一个权重为 `5` 的平滑。
3. 样本数越多，该 CCO 自己的 `AverageDiff` 权重越大。

最终规则是：`SelectionScore` 越小，越可能是正确归属的 CCO。

### 5. 并列时的判定顺序
如果两个 CCO 的分数非常接近，代码按下面顺序继续比较：

1. 样本数更多的优先
2. 平均频差更小的优先
3. MAC 字节序更小的优先

## 关键阈值
当前实现里用到的关键常量如下：

| 常量 | 数值 | 作用 |
| --- | ---: | --- |
| `AREA_FREQ_THEORETICAL_50HZ_PERIOD_TICKS` | `500000` | 理论 `50Hz` 周期 Tick |
| `AREA_FREQ_NTB_CLOCK_HZ` | `25000000` | `NTB` 时钟频率 |
| `AREA_FREQ_MAX_ABS_DIFF` | `2800` | 单个周期差值允许范围 |
| `AREA_FREQ_MIN_VALID_TICKS` | `250000` | 最小有效周期 Tick |
| `AREA_FREQ_MAX_VALID_TICKS` | `2500000` | 最大有效周期 Tick |
| `AREA_FREQ_MAX_CCO_MAC_COUNT` | `16` | 最多记录的 CCO 数量 |
| `AREA_FREQ_MAX_DIFF_COUNT_PER_MAC` | `50` | 每个 CCO 保留的最大样本数 |
| `AREA_FREQ_MIN_SELECT_DIFF_COUNT` | `5` | 参与最终判决的最小样本数 |
| `AREA_FREQ_SELECT_PRIOR_WEIGHT` | `5.0` | 全局平均值的平滑权重 |

## 一个完整的理解例子
假设某次对比后得到：

- CCO 周期差值数组中的代表值对应 `PeriodValue = 1036`
- STA 周期差值数组中的代表值对应 `PeriodValue = 61`

那么：

```text
CCO:
NtbTicks = 1036 * 8 + 500000 = 508288
Frequency = 25000000 / 508288 ≈ 49.184714 Hz

STA:
NtbTicks = 61 * 8 + 500000 = 500488
Frequency = 25000000 / 500488 ≈ 49.951248 Hz

FrequencyDiff = |49.184714 - 49.951248|
              ≈ 0.766533 Hz
```

这 `0.766533 Hz` 就会被记到该 `CCO MAC` 的频差历史里。后续如果这个 CCO 多次比较都持续得到更小的频差，那么它的平均频差和最终选择分数就会更低，更容易被选为 STA 的归属 CCO。

## 总结
这个算法本质上做了三件事：

1. 用过零 `NTB` 差值恢复工频周期。
2. 用周期换算出 CCO 和 STA 的代表频率，并记录绝对频差。
3. 按 `CCO MAC` 对多次频差做平滑统计，选择长期最稳定、频差最小的那个 CCO 作为最终归属结果。

因此，`NTB` 的差值之所以能转换成频率，是因为它本质上就是工频周期的数字化表示；而频率又天然等于时钟频率除以周期 Tick 数。

## tests 目录测试说明

`tests/` 目录里的内容主要用于把日志中的 `area_record_frequency_diff params:` 参数块抽取出来，再喂给 `app_area_frequency_calc.c` 中 `#ifdef TEST_CODE` 下的单文件测试入口做回放验证。

### 快速运行

建议在 `tests/` 目录下执行：

```bash
cd tests
bash test.sh
```

当前脚本依赖：

- `python3`
- `gcc`

`test.sh` 的执行流程如下：

1. 用 `extract_period_cmp_records.py` 从默认日志 `DATA0000.TXT` 中抽取 `area_record_frequency_diff params:` 参数块。
2. 把抽取结果写入 `tests/bin/period_cmp_records.txt`。
3. 用 `gcc` 编译 `../app_area_frequency_calc.c`，并打开 `-DTEST_CODE` 与 `-DFEEDER_ZONE_IDENTIFICATION` 宏，生成测试程序 `tests/bin/app_area_frequency_calc_single`。
4. 将可执行文件复制到 `/tmp/app_area_frequency_calc_single` 后运行，避免在 WSL 下直接执行 `/mnt/c` 上的 ELF 文件。
5. 测试程序读取抽取结果，输出每条记录的处理状态、汇总统计以及最终选中的 `CCO MAC`。

以当前默认日志为例，脚本运行后可以看到这类关键信息：

- 抽取阶段：`72` 条记录被成功导出，`0` 条在抽取阶段被跳过。
- 计算阶段：汇总结果为 `summary files=1 records=72 ok=70 invalid_param=1 invalid_data=1 mac_table_full=0`。
- 最终选择结果：`selected_cco_mac=45:53:20:25:00:01`。

### 脚本与样例文件说明

| 文件 | 说明 |
| --- | --- |
| `tests/test.sh` | 一键测试入口脚本。默认使用 `DATA0000.TXT` 作为输入，完成“日志抽取 -> 编译测试程序 -> 回放计算”整条链路。 |
| `tests/extract_period_cmp_records.py` | 日志抽取脚本。负责从日志中识别 `area_record_frequency_diff params:` 参数块，提取 `CcoMac`、`CcoData`、`StaData`，并转换成测试程序可直接读取的 `AREA_FREQ_RECORD_V1` 文本格式。 |
| `tests/DATA0000.TXT` | 当前默认回归样例。文件中的目标日志格式是一个参数块，典型内容为 `area_record_frequency_diff params:`、`CcoMac=...`、`CcoData=[...] count=N`、`StaData=[...] count=N`。 |
| `tests/编码规范.md` | 测试目录中的附加说明文档，不参与测试执行。 |

### `extract_period_cmp_records.py` 用法

脚本支持输入单个日志文件、目录或 glob 模式，输出到文件或标准输出。目录输入会扫描 `*.log`、`*.txt` 和 `*.TXT` 文件。

当前脚本只解析下面这类日志块：

```text
area_record_frequency_diff params:
CcoMac=45:53:20:25:00:02
CcoData=[...] count=36
StaData=[...] count=36
```

示例：

```bash
python3 tests/extract_period_cmp_records.py \
  tests/DATA0000.TXT \
  -o tests/bin/period_cmp_records.txt
```

```bash
python3 tests/extract_period_cmp_records.py "tests/*.TXT" -o tests/bin/all_records.txt
```

常用参数说明：

- `-o/--output`：指定输出文件，传 `-` 时输出到标准输出。
- `--strict`：遇到数量不匹配、数值解析失败或超出 `int16` 范围的数据时立即失败；不加该参数时，脚本会打印 `WARN` 并跳过异常记录。

抽取脚本输出的每条记录格式如下，供测试程序逐条回放：

```text
AREA_FREQ_RECORD_V1
source_log=...
source_line=...
cco_mac=455320250002
cco_data_count=36
cco_data=...
sta_data_count=36
sta_data=...
```

### 手动抽取并回放

如果想把抽取和回放拆开执行，最简单的方式是先手动抽取，再把输出文件传给测试程序：

```bash
python3 tests/extract_period_cmp_records.py \
  tests/DATA0000.TXT \
  -o /tmp/period_cmp_data0000.txt

/tmp/app_area_frequency_calc_single /tmp/period_cmp_data0000.txt
```

这种方式适合对比不同日志下的：

- 记录抽取数量
- `invalid_data` 计数
- 每个 `CCO MAC` 的 `diff_count` 和 `avg_diff`
- 最终 `selected_cco_mac`
