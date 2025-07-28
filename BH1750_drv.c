#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/mutex.h>

#define DEV_NAME "mybh"
#define I2C_BUS_NUM    1
#define BH1750_ADDR    0x23

static struct i2c_adapter *i2c_adap;
static struct i2c_client *i2c_client;
static int major_num;
static struct class *bh1750_class;
static struct device *bh1750_device;
static DEFINE_MUTEX(bh1750_mutex);

static const unsigned char init_seq[] = { 0x01 };
static const unsigned char one_time_h_resolution_mode[] = { 0x10 };

static ssize_t dev_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
    char msg[32];
    uint8_t data[2];
    int ret, lux;

    mutex_lock(&bh1750_mutex);

    // 측정 명령 전송
    ret = i2c_master_send(i2c_client, one_time_h_resolution_mode, sizeof(one_time_h_resolution_mode));
    if (ret != sizeof(one_time_h_resolution_mode)) {
        pr_err("BH1750: Failed to send measure command\n");
        mutex_unlock(&bh1750_mutex);
        return -EIO;
    }

    msleep(180); // 데이터시트 권장 대기시간

    // 데이터 수신
    ret = i2c_master_recv(i2c_client, data, 2);
    if (ret != 2) {
        pr_err("BH1750: Failed to receive data\n");
        mutex_unlock(&bh1750_mutex);
        return -EIO;
    }

    // lux 계산 (실수 오차 줄이기 위해 1.2 대신 12/10 사용)
    lux = (((data[0] << 8) | data[1]) * 12) / 10;

    snprintf(msg, sizeof(msg), "%d lux\n", lux);

    *offset = 0;
    ret = simple_read_from_buffer(buf, len, offset, msg, strlen(msg));
    mutex_unlock(&bh1750_mutex);
    return ret;
}

static int dev_open(struct inode *inode, struct file *file) { return 0; }
static int dev_release(struct inode *inode, struct file *file) { return 0; }

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = dev_open,
    .release = dev_release,
    .read = dev_read,
};

static int __init bh1750_init(void)
{
    struct i2c_board_info board_info = {
        I2C_BOARD_INFO("bh1750", BH1750_ADDR)
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

    // 초기화 명령 전송
    ret = i2c_master_send(i2c_client, init_seq, sizeof(init_seq));
    if (ret != sizeof(init_seq)) {
        pr_err("BH1750: Init command failed\n");
        ret = -EIO;
        goto unregister_client;
    }

    major_num = register_chrdev(0, DEV_NAME, &fops);
    if (major_num < 0) {
        pr_err("Device registration failed\n");
        ret = major_num;
        goto unregister_client;
    }

    bh1750_class = class_create("bh1750_class");
    if (IS_ERR(bh1750_class)) {
        pr_err("Failed to create class\n");
        ret = PTR_ERR(bh1750_class);
        goto unregister_chrdev;
    }

    bh1750_device = device_create(bh1750_class, NULL, MKDEV(major_num, 0), NULL, DEV_NAME);
    if (IS_ERR(bh1750_device)) {
        pr_err("Failed to create device\n");
        ret = PTR_ERR(bh1750_device);
        goto destroy_class;
    }

    pr_info("BH1750 driver initialized. Major number: %d\n", major_num);
    return 0;

destroy_class:
    class_destroy(bh1750_class);
unregister_chrdev:
    unregister_chrdev(major_num, DEV_NAME);
unregister_client:
    i2c_unregister_device(i2c_client);
put_adapter:
    i2c_put_adapter(i2c_adap);
    return ret;
}

static void __exit bh1750_exit(void)
{
    device_destroy(bh1750_class, MKDEV(major_num, 0));
    class_destroy(bh1750_class);
    unregister_chrdev(major_num, DEV_NAME);
    i2c_unregister_device(i2c_client);
    i2c_put_adapter(i2c_adap);
    pr_info("BH1750 driver removed\n");
}

module_init(bh1750_init);
module_exit(bh1750_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hong&sim");
MODULE_DESCRIPTION("BH1750 I2C Driver (auto /dev/mybh)");