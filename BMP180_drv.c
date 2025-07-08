#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/device.h>

#define DEV_NAME "mybmp"
#define I2C_BUS_NUM    1
#define BMP180_ADDR    0x77

static struct i2c_adapter *i2c_adap;
static struct i2c_client *i2c_client;
static int major_num;
static struct class *bmp180_class;
static struct device *bmp180_device;
static DEFINE_MUTEX(bmp180_mutex);

/* 캘리브레이션 데이터 저장용 */
static short AC1, AC2, AC3;
static u16 AC4, AC5, AC6;
static short B1, B2, MB, MC, MD;

/* 측정 결과 저장용 */
static char result_msg[128] = "";

static short read_word(u8 reg)
{
    u8 buf[2];
    struct i2c_msg msgs[2] = {
        { .addr = BMP180_ADDR, .flags = 0, .len = 1, .buf = &reg },
        { .addr = BMP180_ADDR, .flags = I2C_M_RD, .len = 2, .buf = buf }
    };
    int ret = i2c_transfer(i2c_adap, msgs, 2);
    pr_info("read_word: reg=0x%02X, ret=%d, buf[0]=0x%02X, buf[1]=0x%02X\n",
            reg, ret, buf[0], buf[1]);
    if (ret != 2) {
        pr_err("Failed to read register 0x%02X\n", reg);
        return -EIO;
    }
    return (buf[0] << 8) | buf[1];
}

static long read_pressure_raw(void)
{
    u8 reg = 0xF6;
    u8 buf[3];
    struct i2c_msg msgs[2] = {
        { .addr = BMP180_ADDR, .flags = 0, .len = 1, .buf = &reg },
        { .addr = BMP180_ADDR, .flags = I2C_M_RD, .len = 3, .buf = buf }
    };
    int ret = i2c_transfer(i2c_adap, msgs, 2);
    pr_info("read_pressure_raw: ret=%d, buf[0]=0x%02X, buf[1]=0x%02X, buf[2]=0x%02X\n",
            ret, buf[0], buf[1], buf[2]);
    if (ret != 2) {
        pr_err("Failed to read pressure\n");
        return -EIO;
    }
    return (((long)buf[0] << 16) | ((long)buf[1] << 8) | buf[2]) >> (8 - 0);
}

static void measure_bmp180(void)
{
    u8 temp_cmd[2] = { 0xF4, 0x2E };
    if (i2c_master_send(i2c_client, temp_cmd, 2) != 2) {
        pr_err("Temp command failed\n");
        return;
    }
    msleep(5);

    short UT = read_word(0xF6);
    if (UT < 0) return;

    long X1 = ((UT - AC6) * AC5) >> 15;
    long X2 = (MC << 11) / (X1 + MD);
    long B5 = X1 + X2;
    int temperature = ((B5 + 8) >> 4);

    u8 press_cmd[2] = { 0xF4, 0x34 };
    if (i2c_master_send(i2c_client, press_cmd, 2) != 2) {
        pr_err("Pressure command failed\n");
        return;
    }
    msleep(5);

    long UP = read_pressure_raw();
    if (UP < 0) return;

    long B6 = B5 - 4000;
    X1 = (B2 * ((B6 * B6) >> 12)) >> 11;
    X2 = (AC2 * B6) >> 11;
    long X3 = X1 + X2;
    long B3 = (((((long)AC1) * 4 + X3) + 2) / 4);

    X1 = (AC3 * B6) >> 13;
    X2 = (B1 * ((B6 * B6) >> 12)) >> 16;
    X3 = ((X1 + X2) + 2) >> 2;
    u32 B4 = (AC4 * (u32)(X3 + 32768)) >> 15;
    u32 B7 = ((u32)(UP - B3)) * 50000;

    long p;
    if (B7 < 0x80000000)
        p = (B7 * 2) / B4;
    else
        p = (B7 / B4) * 2;

    X1 = (p >> 8) * (p >> 8);
    X1 = (X1 * 3038) >> 16;
    X2 = (-7357 * p) >> 16;
    p = p + ((X1 + X2 + 3791) >> 4);

    long pressure_int = p / 100;
    long pressure_frac = p % 100;

    snprintf(result_msg, sizeof(result_msg),
        "Temperature: %d.%d C\nPressure: %ld.%02ld hPa\n",
        temperature / 10, temperature % 10,
        pressure_int, pressure_frac);
}

static ssize_t dev_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
    ssize_t ret;
    mutex_lock(&bmp180_mutex);
    measure_bmp180();
    *offset = 0;
    ret = simple_read_from_buffer(buf, len, offset, result_msg, strlen(result_msg));
    mutex_unlock(&bmp180_mutex);
    return ret;
}

static int dev_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int dev_release(struct inode *inode, struct file *file)
{
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .read = dev_read,
};

static int read_calibration_data(void)
{
    AC1 = read_word(0xAA);
    AC2 = read_word(0xAC);
    AC3 = read_word(0xAE);
    AC4 = read_word(0xB0);
    AC5 = read_word(0xB2);
    AC6 = read_word(0xB4);
    B1 = read_word(0xB6);
    B2 = read_word(0xB8);
    MB = read_word(0xBA);
    MC = read_word(0xBC);
    MD = read_word(0xBE);

    pr_info("Calibration: AC1=%d AC2=%d AC3=%d AC4=%u AC5=%u AC6=%u B1=%d B2=%d MB=%d MC=%d MD=%d\n",
        AC1, AC2, AC3, AC4, AC5, AC6, B1, B2, MB, MC, MD);

    // 음수 값 허용, unsigned 값만 0 체크
    return (AC4 == 0 || AC5 == 0 || AC6 == 0) ? -EIO : 0;
}

static int __init bmp180_init(void)
{
    struct i2c_board_info board_info = {
        I2C_BOARD_INFO("bmp180", BMP180_ADDR)
    };
    int ret;

    i2c_adap = i2c_get_adapter(I2C_BUS_NUM);
    if (!i2c_adap) {
        pr_err("I2C adapter not found\n");
        return -ENODEV;
    }

    i2c_client = i2c_new_client_device(i2c_adap, &board_info);
    if (!i2c_client) {
        pr_err("Device registration failed\n");
        ret = -ENODEV;
        goto put_adapter;
    }

    if ((ret = read_calibration_data()) < 0) {
        pr_err("Calibration data read failed\n");
        goto unregister_client;
    }

    major_num = register_chrdev(0, DEV_NAME, &fops);
    if (major_num < 0) {
        pr_err("Device registration failed\n");
        ret = major_num;
        goto unregister_client;
    }

    // 커널 5.x 이상: class_create는 문자열 인자만 받음
    bmp180_class = class_create("bmp180_class");
    if (IS_ERR(bmp180_class)) {
        pr_err("Failed to create class\n");
        ret = PTR_ERR(bmp180_class);
        goto unregister_chrdev;
    }

    bmp180_device = device_create(bmp180_class, NULL, MKDEV(major_num, 0), NULL, DEV_NAME);
    if (IS_ERR(bmp180_device)) {
        pr_err("Failed to create device\n");
        ret = PTR_ERR(bmp180_device);
        goto destroy_class;
    }

    pr_info("BMP180 driver initialized. Major number: %d\n", major_num);
    return 0;

destroy_class:
    class_destroy(bmp180_class);
unregister_chrdev:
    unregister_chrdev(major_num, DEV_NAME);
unregister_client:
    i2c_unregister_device(i2c_client);
put_adapter:
    i2c_put_adapter(i2c_adap);
    return ret;
}

static void __exit bmp180_exit(void)
{
    device_destroy(bmp180_class, MKDEV(major_num, 0));
    class_destroy(bmp180_class);
    unregister_chrdev(major_num, DEV_NAME);
    i2c_unregister_device(i2c_client);
    i2c_put_adapter(i2c_adap);
    pr_info("BMP180 driver removed\n");
}

module_init(bmp180_init);
module_exit(bmp180_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hong@sim");
MODULE_DESCRIPTION("BMP180 I2C Driver (auto /dev/mybmp)");