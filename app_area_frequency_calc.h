#ifndef __APP_AREA_FREQUENCY_CALC_H__
#define __APP_AREA_FREQUENCY_CALC_H__

#ifdef FEEDER_ZONE_IDENTIFICATION

#ifndef TEST_CODE
#include "types.h"
#include "libc.h"
#include "string.h"
#include "printf_zc.h"

#ifndef memcpy
#define memcpy __memcpy
#endif

#else

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef printf_s
#define printf_s printf
#endif /* printf_s */

#endif /* TEST_CODE */

typedef struct
{
    uint8_t CcoMac[6];
    uint16_t DiffCount;
    float AverageDiff;
    float SelectionScore;
} AreaFreqSelectResult_t;

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
extern int area_record_frequency_diff(const uint8_t *CcoMac,
                               const int16_t *CcoData,
                               uint16_t CcoDataCount,
                               const int16_t *StaData,
                               uint16_t StaDataCount);

/**
 * @brief 获取归属关系最优的 CCO MAC。
 *
 * @param [out] Result 选择结果
 * @return bool true 表示成功获取，false 表示未获取到结果
 */
extern bool area_get_belonging_cco_mac(AreaFreqSelectResult_t *Result);

/**
 * @brief 清空所有频差记录。
 *
 * @return void 无
 */
extern void area_reset_frequency_records(void);

/**
 * @brief 打印归属选择结果。
 *
 * @return void 无
 */
extern void area_print_belonging_cco_mac(void);

/**
 * @brief 打印当前缓存的频差记录。
 *
 * @return void 无
 */
extern void area_dump_frequency_records(void);

#endif /* FEEDER_ZONE_IDENTIFICATION */

#endif /* __APP_AREA_FREQUENCY_CALC_H__ */