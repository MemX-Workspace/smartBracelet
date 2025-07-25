#include "custom_flash.h"
#define LOG_TAG             "[custom_flash]"


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



/**************************  flash_device **************************** */



void flash_task()
{
    u8_t tx_buf[6] = {0x90, 0x00, 0x00, 0x00};
    u8_t rx_buf[10] = {0};
    printf("custom flash task start\n");
    while(1)
    {
        // gpio_write(custom_flash_spi_info->spi_cs, 0);
        // // spi_dma_send(custom_flash_spi_info->spi_hdl, tx_buf, sizeof(tx_buf));
        // spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[0]); // 发送一个字节
        // spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[1]);
        // spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[2]);
        // spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[3]);

        // spi_send_byte(custom_flash_spi_info->spi_hdl, 0x00); // 发送一个字节以接收数据
        // spi_send_byte(custom_flash_spi_info->spi_hdl, 0x00);

        // spi_dma_recv(custom_flash_spi_info->spi_hdl, rx_buf, sizeof(rx_buf));
        // gpio_write(custom_flash_spi_info->spi_cs, 1);
        // log_info("Received data: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        //          rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4], rx_buf[5],
        //          rx_buf[6], rx_buf[7], rx_buf[8], rx_buf[9]);
        // // printf("tx_buf: %02x %02x %02x %02x\n", tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3]);


        gpio_write(custom_flash_spi_info->spi_cs, 0);
        spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[0]);
        spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[1]);
        spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[2]);
        spi_send_byte(custom_flash_spi_info->spi_hdl, tx_buf[3]);

        spi_send_byte(custom_flash_spi_info->spi_hdl, 0x00); // 发送一个字节以接收数据
        spi_send_byte(custom_flash_spi_info->spi_hdl, 0x00);
        spi_dma_recv(custom_flash_spi_info->spi_hdl, rx_buf, sizeof(rx_buf));


        gpio_write(custom_flash_spi_info->spi_cs, 1);
        os_time_dly(5); // 5ms 发送一次
    }
}


void custom_flash_test(void)
{
    spi_cs_init();
    spi_init();
    task_create(flash_task, NULL, "flash_task");
}



