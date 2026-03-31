/**
 * @file app_area_frequency_calc.c
 * @brief 区域频率计算测试代码
 * @note 当前目录下无同名头文件，本文件暂保留所需标准头文件依赖。
 */

#ifdef FEEDER_ZONE_IDENTIFICATION

#include "app_area_frequency_calc.h"

/* 50Hz 理论周期对应的 Tick 数。 */
#define AREA_FREQ_THEORETICAL_50HZ_PERIOD_TICKS 500000
/* 单个周期差值允许的最大绝对偏差。 */
#define AREA_FREQ_MAX_ABS_DIFF 2800
/* 有效周期允许的最小 Tick 数。 */
#define AREA_FREQ_MIN_VALID_TICKS 250000
/* 有效周期允许的最大 Tick 数。 */
#define AREA_FREQ_MAX_VALID_TICKS 2500000
/* NTB 时钟频率，单位 Hz。 */
#define AREA_FREQ_NTB_CLOCK_HZ 25000000.0f

/* 最多缓存的 CCO MAC 数量。 */
#define AREA_FREQ_MAX_CCO_MAC_COUNT 16
/* 每个 MAC 最多保存的频差样本数。 */
#define AREA_FREQ_MAX_DIFF_COUNT_PER_MAC 50
/* MAC 地址长度。 */
#define AREA_FREQ_MAC_LEN 6
/* 参与归属选择所需的最少样本数。 */
#define AREA_FREQ_MIN_SELECT_DIFF_COUNT 5
/* 全局先验权重。 */
#define AREA_FREQ_SELECT_PRIOR_WEIGHT 5.0f
/* 浮点比较容差。 */
#define AREA_FREQ_SELECT_EPSILON 0.0001f

typedef struct
{
    uint8_t Used;
    uint8_t CcoMac[AREA_FREQ_MAC_LEN];
    uint16_t DiffCount;
    float DiffValues[AREA_FREQ_MAX_DIFF_COUNT_PER_MAC];
} AreaFreqRecord_t;

typedef enum
{
    AREA_FREQ_STATUS_OK = 0,             /* 处理成功。 */
    AREA_FREQ_STATUS_INVALID_PARAM = -1, /* 输入参数非法。 */
    AREA_FREQ_STATUS_INVALID_DATA = -2,  /* 输入数据非法。 */
    AREA_FREQ_STATUS_MAC_TABLE_FULL = -3 /* MAC 记录表已满。 */
} AreaFreqStatus_e;

static AreaFreqRecord_t S_AreaFreqRecords[AREA_FREQ_MAX_CCO_MAC_COUNT];

/**
 * @brief 打印 MAC 地址。
 *
 * @param [in] Mac 待打印的 MAC 地址
 * @return void 无
 */
static void area_print_mac(const uint8_t *Mac)
{
    printf_s("%02X:%02X:%02X:%02X:%02X:%02X",
           Mac[0],
           Mac[1],
           Mac[2],
           Mac[3],
           Mac[4],
           Mac[5]);
}

/**
 * @brief 打印归属选择结果。
 *
 * @return void 无
 */
void area_print_belonging_cco_mac(void)
{
    AreaFreqSelectResult_t Result;

    if (false == area_get_belonging_cco_mac(&Result))
    {
        printf_s("selected_cco_mac=NONE\n");
        return;
    }

    printf_s("selected_cco_mac=");
    area_print_mac(Result.CcoMac);
    printf_s(" diff_count=%u avg_diff=%.4f score=%.4f\n",
           (unsigned int)Result.DiffCount,
           Result.AverageDiff,
           Result.SelectionScore);
}

/**
 * @brief 打印周期差值数组。
 *
 * @param [in] Label 数组标签
 * @param [in] Data 周期差值数组
 * @param [in] Count 数组元素个数
 * @return void 无
 */
static void area_debug_print_period_values(const char *Label, const int16_t *Data, uint16_t Count)
{
    uint16_t Index = 0U;

    printf_s("%s=", Label);
    if (NULL == Data)
    {
        printf_s("NULL count=%u\n", (unsigned int)Count);
        return;
    }

    printf_s("[");
    for (Index = 0U; Count > Index; Index++)
    {
        printf_s((0U == Index) ? "%d" : ", %d", Data[Index]);
    }
    printf_s("] count=%u\n", (unsigned int)Count);
}

/**
 * @brief 打印 area_record_frequency_diff 的全部入参。
 *
 * @param [in] CcoMac CCO MAC 地址
 * @param [in] CcoData CCO 周期差值数组
 * @param [in] CcoDataCount CCO 周期差值数组长度
 * @param [in] StaData STA 周期差值数组
 * @param [in] StaDataCount STA 周期差值数组长度
 * @return void 无
 */
static void area_debug_print_record_frequency_diff_params(const uint8_t *CcoMac,
                                                          const int16_t *CcoData,
                                                          uint16_t CcoDataCount,
                                                          const int16_t *StaData,
                                                          uint16_t StaDataCount)
{
    printf_s("area_record_frequency_diff params:\n");
    printf_s("CcoMac=");
    if (NULL == CcoMac)
    {
        printf_s("NULL\n");
    }
    else
    {
        area_print_mac(CcoMac);
        printf_s("\n");
    }

    area_debug_print_period_values("CcoData", CcoData, CcoDataCount);
    area_debug_print_period_values("StaData", StaData, StaDataCount);
}

/**
 * @brief 打印当前缓存的频差记录。
 *
 * @return void 无
 */
void area_dump_frequency_records(void)
{
    uint16_t RecordIndex = 0U;

    for (RecordIndex = 0U; AREA_FREQ_MAX_CCO_MAC_COUNT > RecordIndex; RecordIndex++)
    {
        uint16_t DiffIndex = 0U;
        const AreaFreqRecord_t *Record = &S_AreaFreqRecords[RecordIndex];

        if (0U == Record->Used)
        {
            continue;
        }

        printf_s("record[%u] mac=", (unsigned int)RecordIndex);
        area_print_mac(Record->CcoMac);
        printf_s(" diff_count=%u diff_values=[", (unsigned int)Record->DiffCount);

        for (DiffIndex = 0U; Record->DiffCount > DiffIndex; DiffIndex++)
        {
            printf_s((0U == DiffIndex) ? "%f" : ", %f", Record->DiffValues[DiffIndex]);
        }

        printf_s("]\n");
    }
}

/**
 * @brief 将周期差值恢复为理论 Tick 数。
 *
 * @param [in] PeriodValue 周期差值
 * @return int32_t 恢复后的 Tick 数
 */
static inline int32_t area_restore_ntb_diff_to_ticks(int16_t PeriodValue)
{
    return ((int32_t)PeriodValue * 8) + AREA_FREQ_THEORETICAL_50HZ_PERIOD_TICKS;
}

/**
 * @brief Preserve raw frequency precision for later statistics.
 *
 * @param [in] Frequency Input frequency
 * @return float Unmodified frequency
 */
static inline float area_preserve_frequency_precision(float Frequency)
{
    return Frequency;
}

/**
 * @brief 根据周期差值计算频率。
 *
 * @param [in] PeriodValue 周期差值
 * @return float 计算得到的频率，失败时返回 0
 */
static inline float area_calc_frequency_from_period_value(int16_t PeriodValue)
{
    int32_t NtbTicks = area_restore_ntb_diff_to_ticks(PeriodValue);

    if ((AREA_FREQ_MIN_VALID_TICKS >= NtbTicks) || (AREA_FREQ_MAX_VALID_TICKS <= NtbTicks))
    {
        return 0.0f;
    }

    return area_preserve_frequency_precision(AREA_FREQ_NTB_CLOCK_HZ / (float)NtbTicks);
}

/**
 * @brief 根据周期差值数组获取频率结果。
 *
 * @param [in] Diff 周期差值数组
 * @param [in] Count 数组元素个数
 * @return float 计算得到的最大频率，失败时返回 0
 */
static float area_get_frequency_by_ntb_diff(const int16_t *Diff, uint16_t Count)
{
    float FrequencyMax = 0.0f;
    uint16_t Index = 0U;

    if ((NULL == Diff) || (0U == Count))
    {
        return 0.0f;
    }

    for (Index = 0U; Count > Index; Index++)
    {
        float Frequency = 0.0f;

        if ((AREA_FREQ_MAX_ABS_DIFF < Diff[Index]) || ((-AREA_FREQ_MAX_ABS_DIFF) > Diff[Index]))
        {
            return 0.0f;
        }

        Frequency = area_calc_frequency_from_period_value(Diff[Index]);
        if (Frequency > FrequencyMax)
        {
            FrequencyMax = Frequency;
        }
    }

    return FrequencyMax;
}

/**
 * @brief 计算两个频率的绝对差值。
 *
 * @param [in] CcoFrequency CCO 频率
 * @param [in] StaFrequency STA 频率
 * @return float 频率绝对差值
 */
static float area_abs_frequency_diff(float CcoFrequency, float StaFrequency)
{
    float Diff = CcoFrequency - StaFrequency;

    return (0.0f <= Diff) ? Diff : -Diff;
}

/**
 * @brief 计算浮点数的绝对值。
 *
 * @param [in] Value 输入值
 * @return float 绝对值结果
 */
static float area_float_abs(float Value)
{
    return (0.0f <= Value) ? Value : -Value;
}

/**
 * @brief 比较两个 MAC 地址是否相同。
 *
 * @param [in] Lhs 左侧 MAC
 * @param [in] Rhs 右侧 MAC
 * @return int 相同返回 1，不同返回 0
 */
static int area_mac_equal(const uint8_t *Lhs, const uint8_t *Rhs)
{
    return (0 == memcmp(Lhs, Rhs, AREA_FREQ_MAC_LEN)) ? 1 : 0;
}

/**
 * @brief 复制 MAC 地址。
 *
 * @param [out] Dst 目标 MAC
 * @param [in] Src 源 MAC
 * @return void 无
 */
static void area_mac_copy(uint8_t *Dst, const uint8_t *Src)
{
    memcpy(Dst, Src, AREA_FREQ_MAC_LEN);
}

/**
 * @brief 查找已存在的 MAC 记录。
 *
 * @param [in] CcoMac 待查找的 MAC
 * @return AreaFreqRecord_t* 找到则返回记录指针，否则返回 NULL
 */
static AreaFreqRecord_t *area_find_record(const uint8_t *CcoMac)
{
    uint16_t Index = 0U;

    for (Index = 0U; AREA_FREQ_MAX_CCO_MAC_COUNT > Index; Index++)
    {
        if ((0U != S_AreaFreqRecords[Index].Used) &&
            (0 != area_mac_equal(S_AreaFreqRecords[Index].CcoMac, CcoMac)))
        {
            return &S_AreaFreqRecords[Index];
        }
    }

    return NULL;
}

/**
 * @brief 为新的 MAC 分配记录槽位。
 *
 * @param [in] CcoMac 待分配的 MAC
 * @return AreaFreqRecord_t* 分配成功返回记录指针，失败返回 NULL
 */
static AreaFreqRecord_t *area_alloc_record(const uint8_t *CcoMac)
{
    uint16_t Index = 0U;

    for (Index = 0U; AREA_FREQ_MAX_CCO_MAC_COUNT > Index; Index++)
    {
        if (0U == S_AreaFreqRecords[Index].Used)
        {
            S_AreaFreqRecords[Index].Used = 1U;
            area_mac_copy(S_AreaFreqRecords[Index].CcoMac, CcoMac);
            S_AreaFreqRecords[Index].DiffCount = 0U;
            return &S_AreaFreqRecords[Index];
        }
    }

    return NULL;
}

/**
 * @brief 获取 MAC 对应的记录槽位。
 *
 * @param [in] CcoMac 待获取的 MAC
 * @return AreaFreqRecord_t* 已存在或新分配的记录槽位，失败返回 NULL
 */
static AreaFreqRecord_t *area_get_record_slot(const uint8_t *CcoMac)
{
    AreaFreqRecord_t *Record = area_find_record(CcoMac);

    if (NULL != Record)
    {
        return Record;
    }

    return area_alloc_record(CcoMac);
}

/**
 * @brief 向记录中追加一个频差样本。
 *
 * @param [in,out] Record 目标记录
 * @param [in] FrequencyDiff 频差样本
 * @return void 无
 */
static void area_append_frequency_diff(AreaFreqRecord_t *Record, float FrequencyDiff)
{
    if (AREA_FREQ_MAX_DIFF_COUNT_PER_MAC > Record->DiffCount)
    {
        Record->DiffValues[Record->DiffCount] = FrequencyDiff;
        Record->DiffCount++;
        return;
    }

    memmove(&Record->DiffValues[0],
            &Record->DiffValues[1],
            sizeof(Record->DiffValues[0]) * (AREA_FREQ_MAX_DIFF_COUNT_PER_MAC - 1));
    Record->DiffValues[AREA_FREQ_MAX_DIFF_COUNT_PER_MAC - 1] = FrequencyDiff;
}

/**
 * @brief 记录一组 CCO 与 STA 的频率差值。
 *
 * @param [in] CcoMac CCO MAC 地址
 * @param [in] CcoData CCO 周期差值数组
 * @param [in] CcoDataCount CCO 周期差值数量
 * @param [in] StaData STA 周期差值数组
 * @param [in] StaDataCount STA 周期差值数量
 * @return int 处理状态码
 */
int area_record_frequency_diff(const uint8_t *CcoMac,
                               const int16_t *CcoData,
                               uint16_t CcoDataCount,
                               const int16_t *StaData,
                               uint16_t StaDataCount)
{
    float CcoFrequency = 0.0f;
    float StaFrequency = 0.0f;
    float FrequencyDiff = 0.0f;
    AreaFreqRecord_t *Record = NULL;

    area_debug_print_record_frequency_diff_params(CcoMac, CcoData, CcoDataCount, StaData, StaDataCount);

    if ((NULL == CcoMac) ||
        (NULL == CcoData) ||
        (0U == CcoDataCount) ||
        (NULL == StaData) ||
        (0U == StaDataCount))
    {
        return AREA_FREQ_STATUS_INVALID_PARAM;
    }

    CcoFrequency = area_get_frequency_by_ntb_diff(CcoData, CcoDataCount);
    StaFrequency = area_get_frequency_by_ntb_diff(StaData, StaDataCount);
    printf_s("CcoFrequency = %f, StaFrequency = %f\n", CcoFrequency, StaFrequency);

    if ((0.0f >= CcoFrequency) || (0.0f >= StaFrequency))
    {
        return AREA_FREQ_STATUS_INVALID_DATA;
    }

    Record = area_get_record_slot(CcoMac);
    if (NULL == Record)
    {
        return AREA_FREQ_STATUS_MAC_TABLE_FULL;
    }

    FrequencyDiff = area_abs_frequency_diff(CcoFrequency, StaFrequency);
    area_append_frequency_diff(Record, FrequencyDiff);

    return AREA_FREQ_STATUS_OK;
}

/**
 * @brief 计算单条记录的平均频差。
 *
 * @param [in] Record 目标记录
 * @return float 平均频差，失败时返回 0
 */
static float area_calc_record_average_diff(const AreaFreqRecord_t *Record)
{
    float DiffSum = 0.0f;
    uint16_t Index = 0U;

    if ((NULL == Record) || (0U == Record->DiffCount))
    {
        return 0.0f;
    }

    for (Index = 0U; Record->DiffCount > Index; Index++)
    {
        DiffSum += Record->DiffValues[Index];
    }

    return DiffSum / (float)Record->DiffCount;
}

/**
 * @brief 计算全局平均频差。
 *
 * @param [out] SampleCount 全局样本总数
 * @return float 全局平均频差，失败时返回 0
 */
static float area_calc_global_average_diff(uint32_t *SampleCount)
{
    float DiffSum = 0.0f;
    uint32_t DiffSamples = 0U;
    uint16_t RecordIndex = 0U;

    for (RecordIndex = 0U; AREA_FREQ_MAX_CCO_MAC_COUNT > RecordIndex; RecordIndex++)
    {
        uint16_t DiffIndex = 0U;
        const AreaFreqRecord_t *Record = &S_AreaFreqRecords[RecordIndex];

        if (0U == Record->Used)
        {
            continue;
        }

        for (DiffIndex = 0U; Record->DiffCount > DiffIndex; DiffIndex++)
        {
            DiffSum += Record->DiffValues[DiffIndex];
            DiffSamples++;
        }
    }

    if (NULL != SampleCount)
    {
        *SampleCount = DiffSamples;
    }

    if (0U == DiffSamples)
    {
        return 0.0f;
    }

    return DiffSum / (float)DiffSamples;
}

/**
 * @brief 判断当前结果是否优于已选结果。
 *
 * @param [in] Record 当前记录
 * @param [in] AverageDiff 当前平均频差
 * @param [in] SelectionScore 当前评分
 * @param [in] BestResult 已选最优结果
 * @param [in] Found 是否已有最优结果
 * @return bool true 表示当前结果更优，false 表示不是更优结果
 */
static bool area_is_better_select_result(const AreaFreqRecord_t *Record,
                                         float AverageDiff,
                                         float SelectionScore,
                                         const AreaFreqSelectResult_t *BestResult,
                                         bool Found)
{
    bool ScoreTied = false;

    if ((NULL == Record) || (NULL == BestResult) || (false == Found))
    {
        return true;
    }

    if (SelectionScore < (BestResult->SelectionScore - AREA_FREQ_SELECT_EPSILON))
    {
        return true;
    }

    ScoreTied = area_float_abs(SelectionScore - BestResult->SelectionScore) <= AREA_FREQ_SELECT_EPSILON;
    if ((true == ScoreTied) && (Record->DiffCount > BestResult->DiffCount))
    {
        return true;
    }

    if ((true == ScoreTied) &&
        (Record->DiffCount == BestResult->DiffCount) &&
        (AverageDiff < (BestResult->AverageDiff - AREA_FREQ_SELECT_EPSILON)))
    {
        return true;
    }

    if ((true == ScoreTied) &&
        (Record->DiffCount == BestResult->DiffCount) &&
        (area_float_abs(AverageDiff - BestResult->AverageDiff) <= AREA_FREQ_SELECT_EPSILON) &&
        (0 > memcmp(Record->CcoMac, BestResult->CcoMac, AREA_FREQ_MAC_LEN)))
    {
        return true;
    }

    return false;
}

/**
 * @brief 获取归属关系最优的 CCO MAC。
 *
 * @param [out] Result 选择结果
 * @return bool true 表示成功获取，false 表示未获取到结果
 */
bool area_get_belonging_cco_mac(AreaFreqSelectResult_t *Result)
{
    float GlobalAverageDiff = 0.0f;
    uint32_t GlobalSampleCount = 0U;
    uint16_t Index = 0U;
    bool Found = false;

    if (NULL == Result)
    {
        return false;
    }

    memset(Result, 0, sizeof(*Result));

    GlobalAverageDiff = area_calc_global_average_diff(&GlobalSampleCount);
    if (0U == GlobalSampleCount)
    {
        return false;
    }

    for (Index = 0U; AREA_FREQ_MAX_CCO_MAC_COUNT > Index; Index++)
    {
        float AverageDiff = 0.0f;
        float SelectionScore = 0.0f;
        const AreaFreqRecord_t *Record = &S_AreaFreqRecords[Index];

        if ((0U == Record->Used) || (AREA_FREQ_MIN_SELECT_DIFF_COUNT > Record->DiffCount))
        {
            continue;
        }

        AverageDiff = area_calc_record_average_diff(Record);
        SelectionScore =
            ((AverageDiff * (float)Record->DiffCount) +
             (GlobalAverageDiff * AREA_FREQ_SELECT_PRIOR_WEIGHT)) /
            ((float)Record->DiffCount + AREA_FREQ_SELECT_PRIOR_WEIGHT);

        if (true == area_is_better_select_result(Record,
                                                 AverageDiff,
                                                 SelectionScore,
                                                 Result,
                                                 Found))
        {
            area_mac_copy(Result->CcoMac, Record->CcoMac);
            Result->DiffCount = Record->DiffCount;
            Result->AverageDiff = AverageDiff;
            Result->SelectionScore = SelectionScore;
            Found = true;
        }
    }

    return Found;
}

/**
 * @brief 清空所有频差记录。
 *
 * @return void 无
 */
void area_reset_frequency_records(void)
{
    printf_s("area_reset_frequency_records\n");
    memset(S_AreaFreqRecords, 0, sizeof(S_AreaFreqRecords));
}

#ifdef TEST_CODE

/* 单次输入记录允许的最大数据点个数。 */
#define AREA_FREQ_MAX_INPUT_DATA_COUNT 256
/* 单行输入允许的最大长度。 */
#define AREA_FREQ_MAX_INPUT_LINE_LEN 4096
/* 原始日志路径字符串最大长度。 */
#define AREA_FREQ_MAX_SOURCE_LOG_LEN 1024
/* MAC 文本串长度。 */
#define AREA_FREQ_MAC_TEXT_LEN 12

typedef struct
{
    uint8_t HasMarker;
    uint8_t HasSourceLog;
    uint8_t HasSourceLine;
    uint8_t HasCcoMac;
    uint8_t HasCcoDataCount;
    uint8_t HasCcoData;
    uint8_t HasStaDataCount;
    uint8_t HasStaData;
    char SourceLog[AREA_FREQ_MAX_SOURCE_LOG_LEN];
    uint32_t SourceLine;
    char CcoMacText[AREA_FREQ_MAC_TEXT_LEN + 1];
    uint16_t CcoDataCountExpected;
    uint16_t CcoDataCountActual;
    int16_t CcoData[AREA_FREQ_MAX_INPUT_DATA_COUNT];
    uint16_t StaDataCountExpected;
    uint16_t StaDataCountActual;
    int16_t StaData[AREA_FREQ_MAX_INPUT_DATA_COUNT];
} AreaFreqInputRecord_t;

typedef struct
{
    uint32_t Files;
    uint32_t Records;
    uint32_t StatusOk;
    uint32_t InvalidParam;
    uint32_t InvalidData;
    uint32_t MacTableFull;
} AreaFreqRunStats_t;

typedef struct
{
    const char *InputPath;
    uint32_t InputLineNo;
} AreaFreqParseContext_t;

/**
 * @brief 重置输入记录缓存。
 *
 * @param [out] Record 待重置的记录
 * @return void 无
 */
static void area_reset_input_record(AreaFreqInputRecord_t *Record)
{
    memset(Record, 0, sizeof(*Record));
}

/**
 * @brief 去除字符串尾部的换行符。
 *
 * @param [in,out] Text 待处理字符串
 * @return void 无
 */
static void area_trim_line_end(char *Text)
{
    size_t Len = strlen(Text);

    while ((0U < Len) && (('\n' == Text[Len - 1U]) || ('\r' == Text[Len - 1U])))
    {
        Text[Len - 1U] = '\0';
        Len--;
    }
}

/**
 * @brief 跳过字符串前导空白。
 *
 * @param [in] Text 输入字符串
 * @return const char* 跳过空白后的指针
 */
static const char *area_skip_spaces(const char *Text)
{
    while ((' ' == *Text) || ('\t' == *Text))
    {
        Text++;
    }

    return Text;
}

/**
 * @brief 判断一行是否为空白行。
 *
 * @param [in] Text 待判断的字符串
 * @return int 空白返回 1，否则返回 0
 */
static int area_line_is_blank(const char *Text)
{
    return ('\0' == *area_skip_spaces(Text)) ? 1 : 0;
}

/**
 * @brief 判断输入记录是否已有待处理内容。
 *
 * @param [in] Record 输入记录
 * @return int 有待处理内容返回 1，否则返回 0
 */
static int area_record_pending(const AreaFreqInputRecord_t *Record)
{
    if (NULL == Record)
    {
        return 0;
    }

    return (0U != Record->HasMarker) ||
           (0U != Record->HasSourceLog) ||
           (0U != Record->HasSourceLine) ||
           (0U != Record->HasCcoMac) ||
           (0U != Record->HasCcoDataCount) ||
           (0U != Record->HasCcoData) ||
           (0U != Record->HasStaDataCount) ||
           (0U != Record->HasStaData);
}

/**
 * @brief 解析无符号 32 位整数。
 *
 * @param [in] Text 输入文本
 * @param [out] Value 解析结果
 * @return int 成功返回 0，失败返回 -1
 */
static int area_parse_uint32(const char *Text, uint32_t *Value)
{
    unsigned long Parsed = 0UL;
    char *End = NULL;

    if ((NULL == Text) || (NULL == Value) || ('\0' == *Text))
    {
        return -1;
    }

    errno = 0;
    Parsed = strtoul(Text, &End, 10);
    if ((0 != errno) || (End == Text) || ('\0' != *End) || (0xFFFFFFFFUL < Parsed))
    {
        return -1;
    }

    *Value = (uint32_t)Parsed;
    return 0;
}

/**
 * @brief 解析逗号分隔的 int16 数组。
 *
 * @param [in] Text 输入文本
 * @param [out] Values 解析后的数值数组
 * @param [out] Count 实际解析出的数量
 * @return int 成功返回 0，失败返回 -1
 */
static int area_parse_int16_list(const char *Text, int16_t *Values, uint16_t *Count)
{
    const char *Cursor = Text;
    uint16_t ParsedCount = 0U;

    if ((NULL == Text) || (NULL == Values) || (NULL == Count))
    {
        return -1;
    }

    while ('\0' != *Cursor)
    {
        char *End = NULL;
        long Parsed = 0L;

        Cursor = area_skip_spaces(Cursor);
        if ('\0' == *Cursor)
        {
            break;
        }

        errno = 0;
        Parsed = strtol(Cursor, &End, 10);
        if ((0 != errno) || (End == Cursor) || (-32768L > Parsed) || (32767L < Parsed))
        {
            return -1;
        }

        if (AREA_FREQ_MAX_INPUT_DATA_COUNT <= ParsedCount)
        {
            return -1;
        }

        Values[ParsedCount] = (int16_t)Parsed;
        ParsedCount++;

        Cursor = area_skip_spaces(End);
        if (',' == *Cursor)
        {
            Cursor++;
            continue;
        }

        if ('\0' != *Cursor)
        {
            return -1;
        }
    }

    *Count = ParsedCount;
    return 0;
}

/**
 * @brief 将 MAC 文本转换为字节数组。
 *
 * @param [in] Text MAC 文本
 * @param [out] Mac 解析后的 MAC 地址
 * @return int 成功返回 0，失败返回 -1
 */
static int area_parse_mac_text(const char *Text, uint8_t *Mac)
{
    char Pair[3] = {0};
    uint16_t Index = 0U;

    if ((NULL == Text) || (NULL == Mac) || ((size_t)AREA_FREQ_MAC_TEXT_LEN != strlen(Text)))
    {
        return -1;
    }

    for (Index = 0U; AREA_FREQ_MAC_LEN > Index; Index++)
    {
        Pair[0] = Text[Index * 2U];
        Pair[1] = Text[Index * 2U + 1U];

        if ((0 == isxdigit((unsigned char)Pair[0])) || (0 == isxdigit((unsigned char)Pair[1])))
        {
            return -1;
        }

        Mac[Index] = (uint8_t)strtoul(Pair, NULL, 16);
    }

    return 0;
}

/**
 * @brief 将状态码转换为文本。
 *
 * @param [in] Status 状态码
 * @return const char* 状态文本
 */
static const char *area_status_to_string(int Status)
{
    switch (Status)
    {
        case AREA_FREQ_STATUS_OK:
            return "OK";
        case AREA_FREQ_STATUS_INVALID_PARAM:
            return "INVALID_PARAM";
        case AREA_FREQ_STATUS_INVALID_DATA:
            return "INVALID_DATA";
        case AREA_FREQ_STATUS_MAC_TABLE_FULL:
            return "MAC_TABLE_FULL";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief 更新运行统计信息。
 *
 * @param [in,out] Stats 统计对象
 * @param [in] Status 当前状态码
 * @return void 无
 */
static void area_update_run_stats(AreaFreqRunStats_t *Stats, int Status)
{
    if (NULL == Stats)
    {
        return;
    }

    Stats->Records++;

    switch (Status)
    {
        case AREA_FREQ_STATUS_OK:
            Stats->StatusOk++;
            break;
        case AREA_FREQ_STATUS_INVALID_PARAM:
            Stats->InvalidParam++;
            break;
        case AREA_FREQ_STATUS_INVALID_DATA:
            Stats->InvalidData++;
            break;
        case AREA_FREQ_STATUS_MAC_TABLE_FULL:
            Stats->MacTableFull++;
            break;
        default:
            break;
    }
}

/**
 * @brief 判断输入记录字段是否完整。
 *
 * @param [in] Record 输入记录
 * @return int 完整返回 1，不完整返回 0
 */
static int area_is_input_record_complete(const AreaFreqInputRecord_t *Record)
{
    if (NULL == Record)
    {
        return 0;
    }

    return (0U != Record->HasSourceLog) &&
           (0U != Record->HasSourceLine) &&
           (0U != Record->HasCcoMac) &&
           (0U != Record->HasCcoDataCount) &&
           (0U != Record->HasCcoData) &&
           (0U != Record->HasStaDataCount) &&
           (0U != Record->HasStaData);
}

/**
 * @brief 处理一条完整的输入记录。
 *
 * @param [in] Record 输入记录
 * @param [in,out] Stats 运行统计信息
 * @return int 成功返回 0，失败返回 -1
 */
static int area_process_input_record(const AreaFreqInputRecord_t *Record, AreaFreqRunStats_t *Stats)
{
    uint8_t CcoMac[AREA_FREQ_MAC_LEN] = {0};
    int Status = 0;

    if (0 == area_is_input_record_complete(Record))
    {
        fprintf(stderr, "input record is incomplete\n");
        return -1;
    }

    if (Record->CcoDataCountExpected != Record->CcoDataCountActual)
    {
        fprintf(stderr,
                "cco_data_count mismatch at %s:%u expected=%u actual=%u\n",
                Record->SourceLog,
                (unsigned int)Record->SourceLine,
                (unsigned int)Record->CcoDataCountExpected,
                (unsigned int)Record->CcoDataCountActual);
        return -1;
    }

    if (Record->StaDataCountExpected != Record->StaDataCountActual)
    {
        fprintf(stderr,
                "sta_data_count mismatch at %s:%u expected=%u actual=%u\n",
                Record->SourceLog,
                (unsigned int)Record->SourceLine,
                (unsigned int)Record->StaDataCountExpected,
                (unsigned int)Record->StaDataCountActual);
        return -1;
    }

    if (0 != area_parse_mac_text(Record->CcoMacText, CcoMac))
    {
        fprintf(stderr,
                "invalid cco_mac at %s:%u value=%s\n",
                Record->SourceLog,
                (unsigned int)Record->SourceLine,
                Record->CcoMacText);
        return -1;
    }

    Status = area_record_frequency_diff(CcoMac,
                                        Record->CcoData,
                                        Record->CcoDataCountActual,
                                        Record->StaData,
                                        Record->StaDataCountActual);
    area_update_run_stats(Stats, Status);

    printf_s("record[%u] %s:%u mac=%s status=%s(%d)\n",
           (unsigned int)Stats->Records,
           Record->SourceLog,
           (unsigned int)Record->SourceLine,
           Record->CcoMacText,
           area_status_to_string(Status),
           Status);
    return 0;
}

/**
 * @brief 解析一行键值对并写入输入记录。
 *
 * @param [in,out] Record 输入记录
 * @param [in] Key 键名
 * @param [in] Value 键值
 * @param [in] ParseContext 解析上下文
 * @return int 成功返回 0，失败返回 -1
 */
static int area_parse_extract_key_value(AreaFreqInputRecord_t *Record,
                                        const char *Key,
                                        const char *Value,
                                        const AreaFreqParseContext_t *ParseContext)
{
    uint32_t ParsedU32 = 0U;

    if (0 == strcmp(Key, "source_log"))
    {
        snprintf(Record->SourceLog, sizeof(Record->SourceLog), "%s", Value);
        Record->HasSourceLog = 1U;
        return 0;
    }

    if (0 == strcmp(Key, "source_line"))
    {
        if (0 != area_parse_uint32(Value, &ParsedU32))
        {
            fprintf(stderr,
                    "invalid source_line at %s:%u\n",
                    ParseContext->InputPath,
                    (unsigned int)ParseContext->InputLineNo);
            return -1;
        }

        Record->SourceLine = ParsedU32;
        Record->HasSourceLine = 1U;
        return 0;
    }

    if (0 == strcmp(Key, "cco_mac"))
    {
        snprintf(Record->CcoMacText, sizeof(Record->CcoMacText), "%s", Value);
        Record->HasCcoMac = 1U;
        return 0;
    }

    if (0 == strcmp(Key, "cco_data_count"))
    {
        if ((0 != area_parse_uint32(Value, &ParsedU32)) || (AREA_FREQ_MAX_INPUT_DATA_COUNT < ParsedU32))
        {
            fprintf(stderr,
                    "invalid cco_data_count at %s:%u\n",
                    ParseContext->InputPath,
                    (unsigned int)ParseContext->InputLineNo);
            return -1;
        }

        Record->CcoDataCountExpected = (uint16_t)ParsedU32;
        Record->HasCcoDataCount = 1U;
        return 0;
    }

    if (0 == strcmp(Key, "cco_data"))
    {
        if (0 != area_parse_int16_list(Value, Record->CcoData, &Record->CcoDataCountActual))
        {
            fprintf(stderr,
                    "invalid cco_data list at %s:%u\n",
                    ParseContext->InputPath,
                    (unsigned int)ParseContext->InputLineNo);
            return -1;
        }

        Record->HasCcoData = 1U;
        return 0;
    }

    if (0 == strcmp(Key, "sta_data_count"))
    {
        if ((0 != area_parse_uint32(Value, &ParsedU32)) || (AREA_FREQ_MAX_INPUT_DATA_COUNT < ParsedU32))
        {
            fprintf(stderr,
                    "invalid sta_data_count at %s:%u\n",
                    ParseContext->InputPath,
                    (unsigned int)ParseContext->InputLineNo);
            return -1;
        }

        Record->StaDataCountExpected = (uint16_t)ParsedU32;
        Record->HasStaDataCount = 1U;
        return 0;
    }

    if (0 == strcmp(Key, "sta_data"))
    {
        if (0 != area_parse_int16_list(Value, Record->StaData, &Record->StaDataCountActual))
        {
            fprintf(stderr,
                    "invalid sta_data list at %s:%u\n",
                    ParseContext->InputPath,
                    (unsigned int)ParseContext->InputLineNo);
            return -1;
        }

        Record->HasStaData = 1U;
        return 0;
    }

    fprintf(stderr,
            "unknown key '%s' at %s:%u\n",
            Key,
            ParseContext->InputPath,
            (unsigned int)ParseContext->InputLineNo);
    return -1;
}

/**
 * @brief 处理并清空当前待提交输入记录。
 *
 * @param [in,out] CurrentRecord 当前输入记录
 * @param [in,out] Stats 运行统计信息
 * @return int 成功返回 0，失败返回 -1
 */
static int area_flush_pending_input_record(AreaFreqInputRecord_t *CurrentRecord, AreaFreqRunStats_t *Stats)
{
    if (0 == area_record_pending(CurrentRecord))
    {
        return 0;
    }

    if (0 != area_process_input_record(CurrentRecord, Stats))
    {
        return -1;
    }

    area_reset_input_record(CurrentRecord);
    return 0;
}

/**
 * @brief 加载并解析提取结果文件。
 *
 * @param [in] Path 输入文件路径
 * @param [in,out] Stats 运行统计信息
 * @return int 成功返回 0，失败返回 -1
 */
static int area_load_extract_file(const char *Path, AreaFreqRunStats_t *Stats)
{
    FILE *Input = NULL;
    char Line[AREA_FREQ_MAX_INPUT_LINE_LEN];
    uint32_t InputLineNo = 0U;
    uint32_t StartRecords = 0U;
    int Result = -1;
    AreaFreqInputRecord_t CurrentRecord;

    if ((NULL == Path) || (NULL == Stats))
    {
        return -1;
    }

    Input = fopen(Path, "r");
    if (NULL == Input)
    {
        perror(Path);
        return -1;
    }

    StartRecords = Stats->Records;
    Stats->Files++;
    area_reset_input_record(&CurrentRecord);

    while (NULL != fgets(Line, sizeof(Line), Input))
    {
        char *Equals = NULL;
        const char *Key = NULL;
        const char *Value = NULL;
        size_t Len = strlen(Line);
        AreaFreqParseContext_t ParseContext;

        InputLineNo++;
        if ((0U < Len) && ('\n' != Line[Len - 1U]) && (0 == feof(Input)))
        {
            fprintf(stderr, "line too long at %s:%u\n", Path, (unsigned int)InputLineNo);
            goto EXIT;
        }

        area_trim_line_end(Line);

        if (0 != area_line_is_blank(Line))
        {
            if (0 != area_flush_pending_input_record(&CurrentRecord, Stats))
            {
                goto EXIT;
            }

            continue;
        }

        if (0 == strcmp(Line, "AREA_FREQ_RECORD_V1"))
        {
            if (0 != area_flush_pending_input_record(&CurrentRecord, Stats))
            {
                goto EXIT;
            }

            CurrentRecord.HasMarker = 1U;
            continue;
        }

        if ('#' == Line[0])
        {
            continue;
        }

        Equals = strchr(Line, '=');
        if (NULL == Equals)
        {
            fprintf(stderr, "invalid line at %s:%u\n", Path, (unsigned int)InputLineNo);
            goto EXIT;
        }

        *Equals = '\0';
        Key = Line;
        Value = Equals + 1;
        ParseContext.InputPath = Path;
        ParseContext.InputLineNo = InputLineNo;

        if (0 != area_parse_extract_key_value(&CurrentRecord, Key, Value, &ParseContext))
        {
            goto EXIT;
        }
    }

    if (0 != ferror(Input))
    {
        perror(Path);
        goto EXIT;
    }

    if (0 != area_flush_pending_input_record(&CurrentRecord, Stats))
    {
        goto EXIT;
    }

    if (Stats->Records == StartRecords)
    {
        fprintf(stderr, "no records loaded from %s\n", Path);
        goto EXIT;
    }

    Result = 0;

EXIT:
    fclose(Input);
    return Result;
}

/**
 * @brief 打印运行统计摘要。
 *
 * @param [in] Stats 运行统计信息
 * @return void 无
 */
static void area_print_run_summary(const AreaFreqRunStats_t *Stats)
{
    printf_s("summary files=%u records=%u ok=%u invalid_param=%u invalid_data=%u mac_table_full=%u\n",
           (unsigned int)Stats->Files,
           (unsigned int)Stats->Records,
           (unsigned int)Stats->StatusOk,
           (unsigned int)Stats->InvalidParam,
           (unsigned int)Stats->InvalidData,
           (unsigned int)Stats->MacTableFull);
}

/**
 * @brief 运行内置演示数据。
 *
 * @return int 运行结果
 */
static int area_run_demo(void)
{
    int16_t CcoData[] = {
        1057, 1052, 1053, 1050, 1067, 1051, 1051, 1052, 1042, 1070, 1051, 1056,
        1045, 1062, 1053, 1058, 1053, 1057, 1052, 1064, 1051, 1036, 1077, 1049,
        1049, 1051, 1063, 1089, 1103, 1105, 1107, 1109, 1109, 1097, 1106, 1118
    };
    int16_t StaData[] = {
        358, 346, 353, 351, 355, 353, 351, 350, 360, 351, 351, 225,
        73, 73, 77, 76, 76, 77, 75, 70, 80, 77, 73, 79,
        70, 75, 76, 76, 71, 81, 61, 87, 79, 78, 70, 76
    };
    uint8_t CcoMacA[AREA_FREQ_MAC_LEN] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15};
    uint8_t CcoMacB[AREA_FREQ_MAC_LEN] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25};
    int Status = 0;

    Status = area_record_frequency_diff(CcoMacA,
                                        CcoData,
                                        (uint16_t)(sizeof(CcoData) / sizeof(CcoData[0])),
                                        StaData,
                                        (uint16_t)(sizeof(StaData) / sizeof(StaData[0])));
    printf_s("record mac A status = %d\n", Status);

    Status = area_record_frequency_diff(CcoMacA,
                                        CcoData,
                                        (uint16_t)(sizeof(CcoData) / sizeof(CcoData[0])),
                                        StaData,
                                        (uint16_t)(sizeof(StaData) / sizeof(StaData[0])));
    printf_s("record mac A again status = %d\n", Status);

    Status = area_record_frequency_diff(CcoMacB,
                                        CcoData,
                                        (uint16_t)(sizeof(CcoData) / sizeof(CcoData[0])),
                                        StaData,
                                        (uint16_t)(sizeof(StaData) / sizeof(StaData[0])));
    printf_s("record mac B status = %d\n", Status);

    area_dump_frequency_records();
    return 0;
}

/**
 * @brief 调试入口函数。
 *
 * @param [in] Argc 参数个数
 * @param [in] Argv 参数列表
 * @return int 进程返回值
 */
int main(int Argc, char *Argv[])
{
    int ArgIndex = 0;
    AreaFreqRunStats_t Stats;

    area_reset_frequency_records();

    if (1 == Argc)
    {
        return area_run_demo();
    }

    memset(&Stats, 0, sizeof(Stats));
    for (ArgIndex = 1; Argc > ArgIndex; ArgIndex++)
    {
        if (0 != area_load_extract_file(Argv[ArgIndex], &Stats))
        {
            return 1;
        }
    }

    area_print_run_summary(&Stats);
    area_dump_frequency_records();
    area_print_belonging_cco_mac();
    return 0;
}

#endif /* TEST_CODE */

#endif /* FEEDER_ZONE_IDENTIFICATION */