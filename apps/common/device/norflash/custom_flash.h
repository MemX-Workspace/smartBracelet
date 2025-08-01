#ifndef __CUSTOM_FLASH_H__
#define __CUSTOM_FLASH_H__

#include "app_config.h"
#include "system/includes.h"
#include "audio_config.h"
#include "debug.h"
#include "asm/spi.h"
#include "timer.h"
#include "jiffies.h"
#include "norflash_sfc.h"

typedef unsigned char u8_t;
typedef unsigned short int u16_t;
typedef unsigned int u32_t;
typedef int i32_t;
typedef short int i16_t;
typedef signed char i8_t;

struct norflash_dev_platform_data {
    int spi_hw_num;         //只支持SPI1或SPI2
    u32 spi_cs_port;        //cs的引脚
    u32 spi_read_width;     //flash读数据的线宽
    const struct spi_platform_data *spi_pdata;
    u32 start_addr;         //分区起始地址
    u32 size;               //分区大小，若只有1个分区，则这个参数可以忽略
};

#define NORFLASH_DEV_PLATFORM_DATA_BEGIN(data) \
	const struct norflash_dev_platform_data data = {

#define NORFLASH_DEV_PLATFORM_DATA_END()  \
};


extern const struct device_operations norflash_dev_ops;
extern const struct device_operations norfs_dev_ops;

void custom_flash_test();





#endif