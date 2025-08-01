#include "custom_flash.h"
// #include "norflash.h"
#include "app_config.h"
#include "asm/clock.h"
#include "system/timer.h"
#include "cpu.h"
#include "system/generic/atomic.h"
#define LOG_TAG      "[custom_flash]"
#define FLASH_CACHE_ENABLE  		0



#define MAX_NORFLASH_PART_NUM       4

struct norflash_partition {
    const char *name;
    u32 start_addr;
    u32 size;
    struct device device;
};

static struct norflash_partition nor_part[MAX_NORFLASH_PART_NUM];

struct norflash_info {
    u32 flash_id;
    u32 flash_capacity;
    int spi_num;
    int spi_err;
    u8 spi_cs_io;
    u8 spi_r_width;
    u8 part_num;
    u8 open_cnt;
    struct norflash_partition *const part_list;
    OS_MUTEX mutex;
    u32 max_end_addr;
};

static struct norflash_info _norflash = {
    .spi_num = (int) - 1,
    .part_list = nor_part,
};

int _norflash_read(u32 addr, u8 *buf, u32 len, u8 cache);
int _norflash_eraser(u8 eraser, u32 addr);
static void _norflash_cache_sync_timer(void *priv);
static int _norflash_write_pages(u32 addr, u8 *buf, u32 len);



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

static struct norflash_partition *norflash_find_part(const char *name)
{
    struct norflash_partition *part = NULL;
    u32 idx;
    for (idx = 0; idx < MAX_NORFLASH_PART_NUM; idx++) {
        part = &_norflash.part_list[idx];
        if (part->name == NULL) {
            continue;
        }
        if (!strcmp(part->name, name)) {
            return part;
        }
    }
    return NULL;
}

static struct norflash_partition *norflash_new_part(const char *name, u32 addr, u32 size)
{
    struct norflash_partition *part;
    u32 idx;
    for (idx = 0; idx < MAX_NORFLASH_PART_NUM; idx++) {
        part = &_norflash.part_list[idx];
        if (part->name == NULL) {
            break;
        }
    }
    if (part->name != NULL) {
        log_error("create norflash part fail\n");
        return NULL;
    }
    memset(part, 0, sizeof(*part));
    part->name = name;
    part->start_addr = addr;
    part->size = size;
    if (part->start_addr + part->size > _norflash.max_end_addr) {
        _norflash.max_end_addr = part->start_addr + part->size;
    }
    _norflash.part_num++;
    return part;
}

static void norflash_delete_part(const char *name)
{
    struct norflash_partition *part;
    u32 idx;
    for (idx = 0; idx < MAX_NORFLASH_PART_NUM; idx++) {
        part = &_norflash.part_list[idx];
        if (part->name == NULL) {
            continue;
        }
        if (!strcmp(part->name, name)) {
            part->name = NULL;
            _norflash.part_num--;
        }
    }
}

static int norflash_verify_part(struct norflash_partition *p)
{
    struct norflash_partition *part = NULL;
    u32 idx;
    for (idx = 0; idx < MAX_NORFLASH_PART_NUM; idx++) {
        part = &_norflash.part_list[idx];
        if (part->name == NULL) {
            continue;
        }
        if ((p->start_addr >= part->start_addr) && (p->start_addr < part->start_addr + part->size)) {
            if (strcmp(p->name, part->name) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

#if FLASH_CACHE_ENABLE
static u32 flash_cache_addr;
static u8 *flash_cache_buf = NULL; //缓存4K的数据，与flash里的数据一样。
static u8 flash_cache_is_dirty;
static u16 flash_cache_timer;

#define FLASH_CACHE_SYNC_T_INTERVAL     60

static int _check_0xff(u8 *buf, u32 len)
{
    for (u32 i = 0; i < len; i ++) {
        if ((*(buf + i)) != 0xff) {
            return 1;
        }
    }
    return 0;
}
#endif


static u32 _pow(u32 num, int n)
{
    u32 powint = 1;
    int i;
    for (i = 1; i <= n; i++) {
        powint *= num;
    }
    return powint;
}

static int _norflash_wait_ok()
{
    u32 timeout = 8 * 1000 * 1000 / 100;
    while (timeout--) {
        spi_cs_l();
        spi_write_byte(WINBOND_READ_SR1);
        u8 reg_1 = spi_read_byte();
        spi_cs_h();
        if (!(reg_1 & BIT(0))) {
            break;
        }
        delay(100);
    }
    if (timeout == 0) {
        log_error("norflash_wait_ok timeout!\r\n");
        return 1;
    }
    return 0;
}

// 该函数用于读取FLASH芯片的JEDEC ID，通过SPI发送JEDEC ID指令（0x9F），
// 然后连续读取3个字节的数据，最终将这3个字节拼接成一个32位的ID返回。
// 该ID用于识别FLASH芯片的厂商和型号。
static u32 _norflash_read_id()
{
    // u8 cmd[1] = {WINBOND_JEDEC_ID};
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
static u32 _norflash_read_id_yf()
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



/// @brief 临时测试写数据的功能
/// @param addr 
/// @param data 
/// @param len 
static void _norflash_write_data_yf(u32 addr, u8 *data, u32 len)
{
    spi_cs_l();
    spi_write_byte(WINBOND_PAGE_PROGRAM);
    spi_write_byte(addr >> 16);
    spi_write_byte(addr >> 8);
    spi_write_byte(addr);
    // for (u32 i = 0; i < len; i++)
    // {
    //     spi_write_byte(data[i]);
    // }
    spi_dma_write(data, len);
    spi_cs_h();
}

/// @brief 临时测试读数据的功能
/// @param addr 
/// @param data 
/// @param len 
static void _norflash_read_data_yf(u32 addr, u8 *data, u32 len)
{
    spi_cs_l();
    spi_write_byte(WINBOND_READ_DATA);
    spi_write_byte(addr >> 16);
    spi_write_byte(addr >> 8);
    spi_write_byte(addr);
    // for (u32 i = 0; i < len; i++)
    // {
    //     data[i] = spi_read_byte();
    // }
    spi_dma_read(data, len);
    spi_cs_h();
}




/// @brief 发送读写地址，根据地址长度选择发送地址的位数，针对W25Q64JVIG现在不需要is4byte_mode
/// @param addr 
static u8 is4byte_mode = 0;
static void _norflash_send_addr(u32 addr)
{
    if (is4byte_mode) {
        spi_write_byte(addr >> 24);
    }
    spi_write_byte(addr >> 16);
    spi_write_byte(addr >> 8);
    spi_write_byte(addr);
}

int _norflash_init(const char *name, struct norflash_dev_platform_data *pdata)
{
    // log_info("norflash_init ! %x %x", pdata->spi_cs_port, pdata->spi_read_width);
    if (_norflash.spi_num == (int) - 1) {
        _norflash.spi_num = pdata->spi_hw_num;
        _norflash.spi_cs_io = pdata->spi_cs_port;
        _norflash.spi_r_width = pdata->spi_read_width;
        _norflash.flash_id = 0;
        _norflash.flash_capacity = 0;
        os_mutex_create(&_norflash.mutex);
        _norflash.max_end_addr = 0;
        _norflash.part_num = 0;
    }
    ASSERT(_norflash.spi_num == pdata->spi_hw_num);
    ASSERT(_norflash.spi_cs_io == pdata->spi_cs_port);
    ASSERT(_norflash.spi_r_width == pdata->spi_read_width);
    struct norflash_partition *part;
    part = norflash_find_part(name);
    if (!part) {
        part = norflash_new_part(name, pdata->start_addr, pdata->size);
        ASSERT(part, "not enough norflash partition memory in array\n");
        ASSERT(norflash_verify_part(part) == 0, "norflash partition %s overlaps\n", name);
        log_info("norflash new partition %s\n", part->name);
    } else {
        ASSERT(0, "norflash partition name already exists\n");
    }
    return 0;
}



static void clock_critical_enter()
{

}



static void clock_critical_exit()
{
    if (!(_norflash.flash_id == 0 || _norflash.flash_id == 0xffff)) {
        spi_set_baud(_norflash.spi_num, spi_get_baud(_norflash.spi_num));
    }
}
CLOCK_CRITICAL_HANDLE_REG(spi_norflash, clock_critical_enter, clock_critical_exit);

void norflash_enter_4byte_addr()
{
    spi_cs_l();
    spi_write_byte(0xb7);
    spi_cs_h();
}
void norflash_exit_4byte_addr()
{
    spi_cs_l();
    spi_write_byte(0xe9);
    spi_cs_h();
}


int _norflash_open(void *arg)
{
    int reg = 0;
    os_mutex_pend(&_norflash.mutex, 0);
    log_info("norflash open\n");
    if (!_norflash.open_cnt) {
        spi_cs_init();
        
        spi_open(_norflash.spi_num);
        _norflash.flash_id = _norflash_read_id();
        log_info("norflash_read_id: 0x%x\n", _norflash.flash_id);
        if ((_norflash.flash_id == 0) || (_norflash.flash_id == 0xffffff)) {
            log_error("read norflash id error !\n");
            reg = -ENODEV;
            goto __exit;
        }
        _norflash.flash_capacity = 64 * _pow(2, (_norflash.flash_id & 0xff) - 0x10) * 1024;
        log_info("norflash_capacity: 0x%x\n", _norflash.flash_capacity);

        is4byte_mode = 0;
        if (_norflash.flash_capacity > 16 * 1024 * 1024) {
            norflash_enter_4byte_addr();
            is4byte_mode = 1;
        }
#if FLASH_CACHE_ENABLE
        flash_cache_buf = (u8 *)malloc(4096);
        ASSERT(flash_cache_buf, "flash_cache_buf is not ok\n");
        flash_cache_addr = 4096;//先给一个大于4096的数
        _norflash_read(0, flash_cache_buf, 4096, 1);
        flash_cache_addr = 0;
#endif
        log_info("norflash open success !\n");
    }
    if (_norflash.flash_id == 0 || _norflash.flash_id == 0xffffff)  {
        log_error("re-open norflash id error !\n");
        reg = -EFAULT;
        goto __exit;
    }
    ASSERT(_norflash.max_end_addr <= _norflash.flash_capacity, "max partition end address is greater than flash capacity\n");
    _norflash.open_cnt++;

__exit:
    os_mutex_post(&_norflash.mutex);
    return reg;
}


int _norflash_close(void)
{
    os_mutex_pend(&_norflash.mutex, 0);
    log_info("norflash close\n");
    if (_norflash.open_cnt) {
        _norflash.open_cnt--;
    }
    if (!_norflash.open_cnt) {
#if FLASH_CACHE_ENABLE
        if (flash_cache_is_dirty) {
            flash_cache_is_dirty = 0;
            _norflash_eraser(FLASH_SECTOR_ERASER, flash_cache_addr);
            _norflash_write_pages(flash_cache_addr, flash_cache_buf, 4096);
        }
        free(flash_cache_buf);
        flash_cache_buf = NULL;
#endif
        spi_close(_norflash.spi_num);
        spi_cs_uninit();

        log_info("norflash close done\n");
    }
    os_mutex_post(&_norflash.mutex);
    return 0;
}







/*
 * _norflash_write_pages
 * 
 * 功能说明：
 *   向NOR Flash指定地址(addr)写入指定长度(len)的数据(buf)，
 *   支持跨页写入，自动处理页边界（每页256字节）。
 *   写入前自动发送写使能命令，采用DMA方式写数据。
 * 
 * 参数说明：
 *   addr - 目标Flash写入的起始地址
 *   buf  - 待写入的数据缓冲区指针
 *   len  - 写入的数据长度（字节数）
 * 
 * 返回值：
 *   0 - 写入成功
 *   1 - 写入失败（等待写入完成超时或其他错误）
 * 
 * 实现细节：
 *   - 首先计算首页可写入的字节数（first_page_len），
 *     若addr未对齐页边界，则先写满当前页。
 *   - 写完首页后，循环写入后续完整页（每次最多256字节），
 *     直到所有数据写入完成。
 *   - 每次写入前都需发送写使能命令。
 *   - 每次写入后等待Flash写入完成。
 */
static int _norflash_write_pages(u32 addr, u8 *buf, u32 len)
{
    /* y_printf("flash write addr = %d, num = %d\n", addr, len); */

    int reg;
    u32 first_page_len = 256 - (addr % 256);
    first_page_len = len > first_page_len ? first_page_len : len;
    _norflash_send_write_enable();
    spi_cs_l();
    spi_write_byte(WINBOND_PAGE_PROGRAM);
    _norflash_send_addr(addr) ;
    spi_dma_write(buf, first_page_len);
    spi_cs_h();
    reg = _norflash_wait_ok();
    if (reg) {
        return 1;
    }
    addr += first_page_len;
    buf += first_page_len;
    len -= first_page_len;
    while (len) {
        u32 cnt = len > 256 ? 256 : len;
        _norflash_send_write_enable();
        spi_cs_l();
        spi_write_byte(WINBOND_PAGE_PROGRAM);
        _norflash_send_addr(addr) ;
        spi_dma_write(buf, cnt);
        spi_cs_h();
        reg = _norflash_wait_ok();
        if (reg) {
            return 1;
        }
        addr += cnt;
        buf += cnt;
        len -= cnt;
    }
    return 0;
}


int _norflash_read(u32 addr, u8 *buf, u32 len, u8 cache)
{
    int reg = 0;
    u32 align_addr;
    os_mutex_pend(&_norflash.mutex, 0);

    /* y_printf("flash read  addr = %d, len = %d\n", addr, len); */
#if FLASH_CACHE_ENABLE
    if (!cache) {
        goto __no_cache1;
    }
    u32 r_len = 4096 - (addr % 4096);
    if ((addr >= flash_cache_addr) && (addr < (flash_cache_addr + 4096))) {
        if (len <= r_len) {
            memcpy(buf, flash_cache_buf + (addr - flash_cache_addr), len);
            goto __exit;
        } else {
            memcpy(buf, flash_cache_buf + (addr - flash_cache_addr), r_len);
            addr += r_len;
            buf += r_len;
            len -= r_len;
        }
    }
__no_cache1:
#endif
    spi_cs_l();
    if (_norflash.spi_r_width == 2) {
        spi_write_byte(WINBOND_FAST_READ_DUAL_OUTPUT);
        _norflash_send_addr(addr);
        spi_write_byte(0);
        spi_set_width(SPI_MODE_UNIDIR_2BIT);
        spi_dma_read(buf, len);
        spi_set_width(SPI_MODE_BIDIR_1BIT);
    } else if (_norflash.spi_r_width == 4) {
        spi_write_byte(0x6b);
        _norflash_send_addr(addr);
        spi_write_byte(0);
        spi_set_width(SPI_MODE_UNIDIR_4BIT);
        spi_dma_read(buf, len);
        spi_set_width(SPI_MODE_BIDIR_1BIT);
    } else {
        spi_write_byte(WINBOND_FAST_READ_DATA);
        _norflash_send_addr(addr);
        spi_write_byte(0);
        spi_dma_read(buf, len);
    }
    spi_cs_h();
//#if FLASH_CACHE_ENABLE
//    if (!cache) {
//        goto __no_cache2;
//    }
//    align_addr = (addr + len) / 4096 * 4096;
//    if ((int)len - (int)((addr + len) - align_addr) >= 4096) {
//        align_addr -= 4096;
//        if (flash_cache_addr != align_addr) {
//            flash_cache_addr = align_addr;
//            memcpy(flash_cache_buf, buf + (align_addr - addr), 4096);
//        }
//    }
//__no_cache2:
//#endif
__exit:
    os_mutex_post(&_norflash.mutex);
    return reg;
}



#if FLASH_CACHE_ENABLE
static void _norflash_cache_sync_timer(void *priv)
{
    int reg = 0;
    os_mutex_pend(&_norflash.mutex, 0);
    if (flash_cache_is_dirty) {
        flash_cache_is_dirty = 0;
        reg = _norflash_eraser(FLASH_SECTOR_ERASER, flash_cache_addr);
        if (reg) {
            goto __exit;
        }
        reg = _norflash_write_pages(flash_cache_addr, flash_cache_buf, 4096);
    }
    if (flash_cache_timer) {
        sys_timeout_del(flash_cache_timer);
        flash_cache_timer = 0;
    }
__exit:
    os_mutex_post(&_norflash.mutex);
}
#endif

int _norflash_write(u32 addr, void *buf, u32 len, u8 cache)
{
    int reg = 0;
    os_mutex_pend(&_norflash.mutex, 0);

    u8 *w_buf = (u8 *)buf;
    u32 w_len = len;

    /* y_printf("flash write addr = %d, num = %d\n", addr, len); */
#if FLASH_CACHE_ENABLE
    if (!cache) {
        reg = _norflash_write_pages(addr, w_buf, w_len);
        goto __exit;
    }
    u32 align_addr = addr / 4096 * 4096;
    u32 align_len = 4096 - (addr - align_addr);
    align_len = w_len > align_len ? align_len : w_len;
    if (align_addr != flash_cache_addr) {
        if (flash_cache_is_dirty) {
            flash_cache_is_dirty = 0;
            reg = _norflash_eraser(FLASH_SECTOR_ERASER, flash_cache_addr);
            if (reg) {
                goto __exit;
            }
            reg = _norflash_write_pages(flash_cache_addr, flash_cache_buf, 4096);
            if (reg) {
                goto __exit;
            }
        }
        _norflash_read(align_addr, flash_cache_buf, 4096, 0);
        flash_cache_addr = align_addr;
    }
    memcpy(flash_cache_buf + (addr - align_addr), w_buf, align_len);
    if ((addr + align_len) % 4096) {
        flash_cache_is_dirty = 1;
        if (flash_cache_timer) {
            sys_timer_re_run(flash_cache_timer);
        } else {
            flash_cache_timer = sys_timeout_add(0, _norflash_cache_sync_timer, FLASH_CACHE_SYNC_T_INTERVAL);
        }
    } else {
        flash_cache_is_dirty = 0;
        reg = _norflash_eraser(FLASH_SECTOR_ERASER, align_addr);
        if (reg) {
            goto __exit;
        }
        reg = _norflash_write_pages(align_addr, flash_cache_buf, 4096);
        if (reg) {
            goto __exit;
        }
    }
    addr += align_len;
    w_buf += align_len;
    w_len -= align_len;
    while (w_len) {
        u32 cnt = w_len > 4096 ? 4096 : w_len;
        _norflash_read(addr, flash_cache_buf, 4096, 0);
        flash_cache_addr = addr;
        memcpy(flash_cache_buf, w_buf, cnt);
        if ((addr + cnt) % 4096) {
            flash_cache_is_dirty = 1;
            if (flash_cache_timer) {
                sys_timer_re_run(flash_cache_timer);
            } else {
                flash_cache_timer = sys_timeout_add(0, _norflash_cache_sync_timer, FLASH_CACHE_SYNC_T_INTERVAL);
            }
        } else {
            flash_cache_is_dirty = 0;
            reg = _norflash_eraser(FLASH_SECTOR_ERASER, addr);
            if (reg) {
                goto __exit;
            }
            reg = _norflash_write_pages(addr, flash_cache_buf, 4096);
            if (reg) {
                goto __exit;
            }
        }
        addr += cnt;
        w_buf += cnt;
        w_len -= cnt;
    }
#else
    reg = _norflash_write_pages(addr, w_buf, w_len);
#endif
__exit:
    os_mutex_post(&_norflash.mutex);
    return reg;
}

int _norflash_eraser(u8 eraser, u32 addr)
{
    u8 eraser_cmd;
    switch (eraser) {
    case FLASH_PAGE_ERASER:
        eraser_cmd = WINBOND_PAGE_ERASE;
        addr = addr / 256 * 256;
        break;
    case FLASH_SECTOR_ERASER:
        eraser_cmd = WINBOND_SECTOR_ERASE;
        //r_printf(">>>[test]:addr = %d\n", addr);
        addr = addr / 4096 * 4096;
        break;
    case FLASH_BLOCK_ERASER:
        eraser_cmd = WINBOND_BLOCK_ERASE;
        addr = addr / 65536 * 65536;
        break;
    case FLASH_CHIP_ERASER:
        eraser_cmd = WINBOND_CHIP_ERASE;
        break;
    }
    _norflash_send_write_enable();
    spi_cs_l();
    spi_write_byte(eraser_cmd);
    if (eraser_cmd != WINBOND_CHIP_ERASE) {
        _norflash_send_addr(addr);
    }
    spi_cs_h();
    return _norflash_wait_ok();
}

int _norflash_ioctl(u32 cmd, u32 arg, u32 unit, void *_part)
{
    int reg = 0;
    struct norflash_partition *part = _part;
    os_mutex_pend(&_norflash.mutex, 0);
    switch (cmd) {
    case IOCTL_GET_STATUS:
        *(u32 *)arg = 1;
        break;
    case IOCTL_GET_ID:
        *((u32 *)arg) = _norflash.flash_id;
        break;
    case IOCTL_GET_CAPACITY:
        if (_norflash.flash_capacity == 0)  {
            *(u32 *)arg = 0;
        } else if (_norflash.part_num == 1 && part->start_addr == 0) {
            *(u32 *)arg = _norflash.flash_capacity / unit;
        } else {
            *(u32 *)arg = part->size / unit;
        }
        break;
    case IOCTL_GET_BLOCK_SIZE:
        *(u32 *)arg = 512;
        break;
    case IOCTL_ERASE_PAGE:
        reg = _norflash_eraser(FLASH_PAGE_ERASER, arg * unit + part->start_addr);
        break;
    case IOCTL_ERASE_SECTOR:
        reg = _norflash_eraser(FLASH_SECTOR_ERASER, arg * unit + part->start_addr);
        break;
    case IOCTL_ERASE_BLOCK:
        reg = _norflash_eraser(FLASH_BLOCK_ERASER, arg * unit + part->start_addr);
        break;
    case IOCTL_ERASE_CHIP:
        reg = _norflash_eraser(FLASH_CHIP_ERASER, 0);
        break;
    case IOCTL_FLUSH:
#if FLASH_CACHE_ENABLE
        if (flash_cache_is_dirty) {
            flash_cache_is_dirty = 0;
            reg = _norflash_eraser(FLASH_SECTOR_ERASER, flash_cache_addr);
            if (reg) {
                goto __exit;
            }
            reg = _norflash_write_pages(flash_cache_addr, flash_cache_buf, 4096);
        }
#endif
        break;
    case IOCTL_CMD_RESUME:
        break;
    case IOCTL_CMD_SUSPEND:
        break;
    case IOCTL_GET_PART_INFO: {
        u32 *info = (u32 *)arg;
        u32 *start_addr = &info[0];
        u32 *part_size = &info[1];
        *start_addr = part->start_addr;
        *part_size = part->size;
    }
    break;
    default:
        reg = -EINVAL;
        break;
    }
__exit:
    os_mutex_post(&_norflash.mutex);
    return reg;
}


/*************************************************************************************
 *                                  挂钩 device_api
 ************************************************************************************/

static int norflash_dev_init(const struct dev_node *node, void *arg)
{
    struct norflash_dev_platform_data *pdata = arg;
    return _norflash_init(node->name, pdata);
}

static int norflash_dev_open(const char *name, struct device **device, void *arg)
{
    struct norflash_partition *part;
    part = norflash_find_part(name);
    if (!part) {
        log_error("no norflash partition is found\n");
        return -ENODEV;
    }
    *device = &part->device;
    (*device)->private_data = part;
    if (atomic_read(&part->device.ref)) {
        return 0;
    }
    return _norflash_open(arg);
}
static int norflash_dev_close(struct device *device)
{
    return _norflash_close();
}
static int norflash_dev_read(struct device *device, void *buf, u32 len, u32 offset)
{
    int reg;
    /* printf("flash read sector = %d, num = %d\n", offset, len); */
    offset = offset * 512;
    len = len * 512;
    struct norflash_partition *part;
    part = (struct norflash_partition *)device->private_data;
    if (!part) {
        log_error("norflash partition invalid\n");
        return -EFAULT;
    }
    offset += part->start_addr;
    reg = _norflash_read(offset, buf, len, 1);
    if (reg) {
        r_printf(">>>[r error]:\n");
        len = 0;
    }

    len = len / 512;
    return len;
}
static int norflash_dev_write(struct device *device, void *buf, u32 len, u32 offset)
{
    /* printf("flash write sector = %d, num = %d\n", offset, len); */
    int reg = 0;
    offset = offset * 512;
    len = len * 512;
    struct norflash_partition *part = device->private_data;
    if (!part) {
        log_error("norflash partition invalid\n");
        return -EFAULT;
    }
    offset += part->start_addr;
    reg = _norflash_write(offset, buf, len, 1);
    if (reg) {
        r_printf(">>>[w error]:\n");
        len = 0;
    }
    len = len / 512;
    return len;
}
static bool norflash_dev_online(const struct dev_node *node)
{
    return 1;
}
static int norflash_dev_ioctl(struct device *device, u32 cmd, u32 arg)
{
    struct norflash_partition *part = device->private_data;
    if (!part) {
        log_error("norflash partition invalid\n");
        return -EFAULT;
    }
    return _norflash_ioctl(cmd, arg, 512, part);
}

/**************************  flash_device **************************** */
const struct device_operations norflash_dev_ops = {
    .init   = norflash_dev_init,
    .online = norflash_dev_online,
    .open   = norflash_dev_open,
    .read   = norflash_dev_read,
    .write  = norflash_dev_write,
    .ioctl  = norflash_dev_ioctl,
    .close  = norflash_dev_close,
};


void flash_task()
{
    u8_t rx_buf[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
    u8_t tx_buf[5] = {0x01, 0x01, 0x01, 0x01, 0x01};
    u8_t tx_buf2[5] = {0x10, 0x10, 0x10, 0x10, 0x10};

    struct norflash_dev_platform_data pdata = {
        .spi_hw_num = 1,
        .spi_cs_port = IO_PORTG_07,
        .spi_read_width = 1,
        .spi_pdata = &spi1_p_data,
        .start_addr = 0x00000000,
        .size = 0x10000000,
    };
    _norflash_init("norflash", &pdata);
    _norflash_open(NULL);
    while(1)
    {
    _norflash_write(0x000000FF, tx_buf, 5, 0);
    _norflash_read(0x000000FF, rx_buf, 5, 0);
    printf("rx_buf: %02x %02x %02x %02x %02x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
    _norflash_eraser(FLASH_SECTOR_ERASER, 0x000000FF);
    _norflash_write(0x000000FF, tx_buf2, 5, 0);
    _norflash_read(0x000000FF, rx_buf, 5, 0);
    printf("rx_buf: %02x %02x %02x %02x %02x\n", rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3], rx_buf[4]);
    _norflash_eraser(FLASH_SECTOR_ERASER, 0x000000FF);
    os_time_dly(10);
    }






    // u8_t tx_buf[2] = {0x45, 0x67};
    // u8_t rx_buf[2] = {0x00, 0x00};
    // printf("custom flash task start\n");
    // _norflash_send_write_enable();
    // _norflash_unlock();
    // while (1)
    // {
    //     // u32 jedec_id = _norflash_read_jedec_id();

    //     _norflash_send_write_enable();
    //     _norflash_write_data_yf(0x00000000, tx_buf, 2);

    //     u8 status = _norflash_check_status();
    //     while ((status & 0x01) != 0x00)
    //     {
    //         os_time_dly(2); // 5ms 发送一次
    //         status = _norflash_check_status();
    //     }

    //     _norflash_read_data_yf(0x00000000, rx_buf, 2);
    //     printf("rx_buf: %02x\n", rx_buf[0]);
    //     os_time_dly(10);
    // }

}


void custom_flash_test(void)
{
    spi_cs_init();
    spi_init();

    task_create(flash_task, NULL, "flash_task");
}



