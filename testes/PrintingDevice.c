#include "linux/printk.h"
#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Davi");
MODULE_DESCRIPTION("Printando conexão e desconexão");

#define GUITAR_ID_VENDOR  0x12BA
#define GUITAR_ID_PRODUCT 0x0100

static struct hid_device_id table[] = {
    { HID_USB_DEVICE(GUITAR_ID_VENDOR, GUITAR_ID_PRODUCT), .driver_data = 1 << 14 },
    {  }
};

MODULE_DEVICE_TABLE(hid, table);

static int guitarConnect(struct hid_device *interface, const struct hid_device_id *id) {
    printk(KERN_INFO "Guitarra conectada\n");

    return 0;
}

static void guitarDisconnect(struct hid_device *inferface) {
    printk(KERN_INFO "Guitarra desconectada\n");
}

static struct hid_driver guitar = {
    .name = "Guitarra PS3",
    .id_table = table,
    .probe = guitarConnect,
    .remove = guitarDisconnect
};

static int __init guitarInit(void) {
    printk(KERN_INFO "Modulo inicializado");

    return hid_register_driver(&guitar);
}

static void __exit guitarExit(void) { 
    printk(KERN_INFO "Modulo terminado");

    hid_unregister_driver(&guitar);
}

module_init(guitarInit);
module_exit(guitarExit);
