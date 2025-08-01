#include "custom_flash.h"
#define LOG_TAG             "[custom_flash]"
#define FLASH_CACHE_ENABLE  		0

/*====================  spi 四线定义 ==============================*/

// 对 spi.h 中提供的固定 spi1 结构体进行配置 
const struct spi_platform_data spi1_p_data = {
    .port = {TCFG_HW_SPI1_PORT_CLK, TCFG_HW_SPI1_PORT_MOSI, TCFG_HW_SPI1_PORT_MISO, (u8)-1, (u8)-1},
    .mode = TCFG_HW_SPI1_MODE,
    .role = TCFG_HW_SPI1_ROLE,
    .clk = TCFG_HW_SPI1_BAUD,
};



// 自定义 flash spi 句柄
const static spi_handle_t custom_flash_spi = {
    .spi_hdl = SPI1,
    .spi_cs = IO_PORTG_07,
};





const static spi_handle_t* custom_flash_spi_info = &custom_flash_spi;

// CS 线配置
// only support 4-wire mode
#define spi_cs_init() \
    do { \
        gpio_write(custom_flash_spi_info->spi_cs, 1); \
        gpio_set_direction(custom_flash_spi_info->spi_cs, 0); \
        gpio_set_die(custom_flash_spi_info->spi_cs, 1); \
    } while (0)
#define spi_cs_uninit() \
    do { \
        gpio_set_die(custom_flash_spi_info->spi_cs, 0); \
        gpio_set_direction(custom_flash_spi_info->spi_cs, 1); \
        gpio_set_pull_up(custom_flash_spi_info->spi_cs, 0); \
        gpio_set_pull_down(custom_flash_spi_info->spi_cs, 0); \
    } while (0)

#define spi_cs_h()                  gpio_write(custom_flash_spi_info->spi_cs, 1)
#define spi_cs_l()                  gpio_write(custom_flash_spi_info->spi_cs, 0)

#define spi_read_byte()             spi_recv_byte(custom_flash_spi_info->spi_hdl, NULL)
#define spi_write_byte(x)           spi_send_byte(custom_flash_spi_info->spi_hdl, x)
#define spi_dma_read(x, y)          spi_dma_recv(custom_flash_spi_info->spi_hdl, x, y)
#define spi_dma_write(x, y)         spi_dma_send(custom_flash_spi_info->spi_hdl, x, y)
#define spi_set_width(x)            spi_set_bit_mode(custom_flash_spi_info->spi_hdl, x)
#define spi_init()              spi_open(custom_flash_spi_info->spi_hdl)
#define spi_closed()            spi_close(custom_flash_spi_info->spi_hdl)
#define spi_suspend()           hw_spi_suspend(custom_flash_spi_info->spi_hdl)
#define spi_resume()            hw_spi_resume(custom_flash_spi_info->spi_hdl)

    

// 该函数用于读取FLASH芯片的JEDEC ID，通过SPI发送JEDEC ID指令（0x9F），
// 然后连续读取3个字节的数据，最终将这3个字节拼接成一个32位的ID返回。
// 该ID用于识别FLASH芯片的厂商和型号。
static u32 _norflash_read_jedec_id()
{
    u8 cmd[1] = {WINBOND_JEDEC_ID};
    u8 id[3];
    spi_cs_l();
    spi_write_byte(WINBOND_JEDEC_ID);
    for (u8 i = 0; i < sizeof(id); i++) {
        id[i] = spi_read_byte();
    }
    // spi_dma_write(cmd, sizeof(cmd));
    // spi_dma_read(id, sizeof(id));

    spi_cs_h();
    return id[0] << 16 | id[1] << 8 | id[2];
}

// 该函数通过SPI接口读取FLASH芯片的ID（使用DMA方式可扩展），
// 主要用于获取FLASH芯片的厂商和设备ID信息。
// 发送0x90指令（WINBOND__ID），然后读取2个字节的ID数据，
// 最终将这2个字节拼接成一个16位的ID返回。
static u32 _norflash_read_id()
{
    u8 cmd[4] = {WINBOND__ID, 0x00, 0x00, 0x00};
    u8 id[2];
    spi_cs_l();
    spi_write_byte(WINBOND_JEDEC_ID);
    for (u8 i = 0; i < sizeof(id); i++) {
        id[i] = spi_read_byte();
    }
    // spi_dma_write(cmd, sizeof(cmd));
    // spi_dma_read(id, sizeof(id));

    spi_cs_h();
    return id[0] << 8 | id[1];
}

// 该函数用于向FLASH芯片发送写使能命令（Write Enable）。
// 写使能命令（0x06）是对FLASH进行写操作（如写入、擦除）前必须发送的指令。
// 通过拉低片选（CS），发送写使能指令，然后拉高片选，完成写使能过程。
static void _norflash_send_write_enable()
{
    spi_cs_l();                          // 拉低片选信号，选中FLASH芯片
    spi_write_byte(WINBOND_WRITE_ENABLE); // 发送写使能指令（0x06）
    spi_cs_h();                          // 拉高片选信号，释放FLASH芯片
}

static void _norflash_unlock()
{
    spi_cs_l();                          // 拉低片选信号，选中FLASH芯片
    spi_write_byte(WINBOND_UNLOCK); // 发送写使能指令（0x06）
    spi_cs_h();                          // 拉高片选信号，释放FLASH芯片
}

static u8 _norflash_check_status()
{
    spi_cs_l();                          // 拉低片选信号，选中FLASH芯片
    spi_write_byte(WINBOND_READ_SR1); // 发送写使能指令（0x06）
    u8 status = spi_read_byte();
    spi_cs_h();                          // 拉高片选信号，释放FLASH芯片
    return status;
}


static void _norflash_write_data(u32 addr, u8 *data, u32 len)
{
    // _norflash_send_write_enable();
    spi_cs_l();
    spi_write_byte(WINBOND_PAGE_PROGRAM);
    spi_write_byte(addr >> 16);
    spi_write_byte(addr >> 8);
    spi_write_byte(addr);
    for (u32 i = 0; i < len; i++)
    {
        spi_write_byte(data[i]);
    }
    // spi_dma_write(data, len);
    spi_cs_h();
}

static void _norflash_read_data(u32 addr, u8 *data, u32 len)
{
    spi_cs_l();
    spi_write_byte(WINBOND_READ_DATA);
    spi_write_byte(addr >> 16);
    spi_write_byte(addr >> 8);
    spi_write_byte(addr);
    for (u32 i = 0; i < len; i++)
    {
        data[i] = spi_read_byte();
    }

    // spi_dma_read(data, len);
    spi_cs_h();
}


// static u8 is4byte_mode = 0;
// static void _norflash_send_addr(u32 addr)
// {
//     if (is4byte_mode) {
//         spi_write_byte(addr >> 24);
//     }
//     spi_write_byte(addr >> 16);
//     spi_write_byte(addr >> 8);
//     spi_write_byte(addr);
// }








// /*
//  * _norflash_write_pages
//  * 
//  * 功能说明：
//  *   向NOR Flash指定地址(addr)写入指定长度(len)的数据(buf)，
//  *   支持跨页写入，自动处理页边界（每页256字节）。
//  *   写入前自动发送写使能命令，采用DMA方式写数据。
//  * 
//  * 参数说明：
//  *   addr - 目标Flash写入的起始地址
//  *   buf  - 待写入的数据缓冲区指针
//  *   len  - 写入的数据长度（字节数）
//  * 
//  * 返回值：
//  *   0 - 写入成功
//  *   1 - 写入失败（等待写入完成超时或其他错误）
//  * 
//  * 实现细节：
//  *   - 首先计算首页可写入的字节数（first_page_len），
//  *     若addr未对齐页边界，则先写满当前页。
//  *   - 写完首页后，循环写入后续完整页（每次最多256字节），
//  *     直到所有数据写入完成。
//  *   - 每次写入前都需发送写使能命令。
//  *   - 每次写入后等待Flash写入完成。
//  */
// static int _norflash_write_pages(u32 addr, u8 *buf, u32 len)
// {
//     /* y_printf("flash write addr = %d, num = %d\n", addr, len); */

//     int reg;
//     u32 first_page_len = 256 - (addr % 256);
//     first_page_len = len > first_page_len ? first_page_len : len;
//     _norflash_send_write_enable();
//     spi_cs_l();
//     spi_write_byte(WINBOND_PAGE_PROGRAM);
//     _norflash_send_addr(addr) ;
//     spi_dma_write(buf, first_page_len);
//     spi_cs_h();
//     reg = _norflash_wait_ok();
//     if (reg) {
//         return 1;
//     }
//     addr += first_page_len;
//     buf += first_page_len;
//     len -= first_page_len;
//     while (len) {
//         u32 cnt = len > 256 ? 256 : len;
//         _norflash_send_write_enable();
//         spi_cs_l();
//         spi_write_byte(WINBOND_PAGE_PROGRAM);
//         _norflash_send_addr(addr) ;
//         spi_dma_write(buf, cnt);
//         spi_cs_h();
//         reg = _norflash_wait_ok();
//         if (reg) {
//             return 1;
//         }
//         addr += cnt;
//         buf += cnt;
//         len -= cnt;
//     }
//     return 0;
// }


// int _norflash_read(u32 addr, u8 *buf, u32 len, u8 cache)
// {
//     int reg = 0;
//     u32 align_addr;
//     os_mutex_pend(&_norflash.mutex, 0);

//     /* y_printf("flash read  addr = %d, len = %d\n", addr, len); */
// #if FLASH_CACHE_ENABLE
//     if (!cache) {
//         goto __no_cache1;
//     }
//     u32 r_len = 4096 - (addr % 4096);
//     if ((addr >= flash_cache_addr) && (addr < (flash_cache_addr + 4096))) {
//         if (len <= r_len) {
//             memcpy(buf, flash_cache_buf + (addr - flash_cache_addr), len);
//             goto __exit;
//         } else {
//             memcpy(buf, flash_cache_buf + (addr - flash_cache_addr), r_len);
//             addr += r_len;
//             buf += r_len;
//             len -= r_len;
//         }
//     }
// __no_cache1:
// #endif
//     spi_cs_l();
//     if (_norflash.spi_r_width == 2) {
//         spi_write_byte(WINBOND_FAST_READ_DUAL_OUTPUT);
//         _norflash_send_addr(addr);
//         spi_write_byte(0);
//         spi_set_width(SPI_MODE_UNIDIR_2BIT);
//         spi_dma_read(buf, len);
//         spi_set_width(SPI_MODE_BIDIR_1BIT);
//     } else if (_norflash.spi_r_width == 4) {
//         spi_write_byte(0x6b);
//         _norflash_send_addr(addr);
//         spi_write_byte(0);
//         spi_set_width(SPI_MODE_UNIDIR_4BIT);
//         spi_dma_read(buf, len);
//         spi_set_width(SPI_MODE_BIDIR_1BIT);
//     } else {
//         spi_write_byte(WINBOND_FAST_READ_DATA);
//         _norflash_send_addr(addr);
//         spi_write_byte(0);
//         spi_dma_read(buf, len);
//     }
//     spi_cs_h();
// //#if FLASH_CACHE_ENABLE
// //    if (!cache) {
// //        goto __no_cache2;
// //    }
// //    align_addr = (addr + len) / 4096 * 4096;
// //    if ((int)len - (int)((addr + len) - align_addr) >= 4096) {
// //        align_addr -= 4096;
// //        if (flash_cache_addr != align_addr) {
// //            flash_cache_addr = align_addr;
// //            memcpy(flash_cache_buf, buf + (align_addr - addr), 4096);
// //        }
// //    }
// //__no_cache2:
// //#endif
// __exit:
//     os_mutex_post(&_norflash.mutex);
//     return reg;
// }

/**************************  flash_device **************************** */



void flash_task()
{


    u8_t tx_buf[2] = {0x45, 0x67};
    u8_t rx_buf[2] = {0x00, 0x00};
    printf("custom flash task start\n");
    _norflash_send_write_enable();
    _norflash_unlock();
    while (1)
    {
        // u32 jedec_id = _norflash_read_jedec_id();

        _norflash_send_write_enable();
        _norflash_write_data(0x00000000, tx_buf, 2);

        u8 status = _norflash_check_status();
        while ((status & 0x01) != 0x00)
        {
            os_time_dly(2); // 5ms 发送一次
            status = _norflash_check_status();
        }

        _norflash_read_data(0x00000000, rx_buf, 2);
        printf("rx_buf: %02x\n", rx_buf[0]);
        os_time_dly(10);
    }
    // spi_cs_h();



    // while(1)
    // {  
    //     u32 jedec_id = _norflash_read_jedec_id();
    //     // u32 id = _norflash_read_id();
        

    //     os_time_dly(5); // 5ms 发送一次
    // }
}


void custom_flash_test(void)
{
    spi_cs_init();
    spi_init();
    // 设置MISO为高阻态
     // 使能数字输入
    task_create(flash_task, NULL, "flash_task");
}



