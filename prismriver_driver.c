#include <linux/init.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/types.h>

// Informações sobre o driver
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Davi Araujo");
MODULE_DESCRIPTION("Driver para a guitarra controle RedOctane do PS3");

// Constantes importantes
#define GUITAR_ID_VENDOR  0x12BA
#define GUITAR_ID_PRODUCT 0x0100

// Report Descriptor da guitarra
// Foi utilizado o hid-tools para descobrir os valores de descritor
// https://gitlab.freedesktop.org/libevdev/hid-tools
static const u8 guitar_report[] = {
    0x05, 0x01,                    // Usage Page (Generic Desktop)        0
    0x09, 0x05,                    // Usage (Game Pad)                    2
    0xa1, 0x01,                    // Collection (Application)            4
    0x15, 0x00,                    //  Logical Minimum (0)                6
    0x25, 0x01,                    //  Logical Maximum (1)                8
    0x35, 0x00,                    //  Physical Minimum (0)               10
    0x45, 0x01,                    //  Physical Maximum (1)               12
    0x75, 0x01,                    //  Report Size (1)                    14
    0x95, 0x0d,                    //  Report Count (13)                  16
    0x05, 0x09,                    //  Usage Page (Button)                18
    0x19, 0x01,                    //  Usage Minimum (1)                  20
    0x29, 0x0d,                    //  Usage Maximum (13)                 22
    0x81, 0x02,                    //  Input (Data,Var,Abs)               24
    0x95, 0x03,                    //  Report Count (3)                   26
    0x81, 0x01,                    //  Input (Cnst,Arr,Abs)               28
    0x05, 0x01,                    //  Usage Page (Generic Desktop)       30
    0x25, 0x07,                    //  Logical Maximum (7)                32
    0x46, 0x3b, 0x01,              //  Physical Maximum (315)             34
    0x75, 0x04,                    //  Report Size (4)                    37
    0x95, 0x01,                    //  Report Count (1)                   39
    0x65, 0x14,                    //  Unit (EnglishRotation: deg)        41
    0x09, 0x39,                    //  Usage (Hat switch)                 43
    0x81, 0x42,                    //  Input (Data,Var,Abs,Null)          45
    0x65, 0x00,                    //  Unit (None)                        47
    0x95, 0x01,                    //  Report Count (1)                   49
    0x81, 0x01,                    //  Input (Cnst,Arr,Abs)               51
    0x26, 0xff, 0x00,              //  Logical Maximum (255)              53
    0x46, 0xff, 0x00,              //  Physical Maximum (255)             56
    0x09, 0x30,                    //  Usage (X)                          59
    0x09, 0x31,                    //  Usage (Y)                          61
    0x09, 0x32,                    //  Usage (Z)                          63
    0x09, 0x35,                    //  Usage (Rz)                         65
    0x75, 0x08,                    //  Report Size (8)                    67
    0x95, 0x04,                    //  Report Count (4)                   69
    0x81, 0x02,                    //  Input (Data,Var,Abs)               71
    0x06, 0x00, 0xff,              //  Usage Page (Vendor Defined Page 1) 73
    0x09, 0x20,                    //  Usage (Vendor Usage 0x20)          76
    0x09, 0x21,                    //  Usage (Vendor Usage 0x21)          78
    0x09, 0x22,                    //  Usage (Vendor Usage 0x22)          80
    0x09, 0x23,                    //  Usage (Vendor Usage 0x23)          82
    0x09, 0x24,                    //  Usage (Vendor Usage 0x24)          84
    0x09, 0x25,                    //  Usage (Vendor Usage 0x25)          86
    0x09, 0x26,                    //  Usage (Vendor Usage 0x26)          88
    0x09, 0x27,                    //  Usage (Vendor Usage 0x27)          90
    0x09, 0x28,                    //  Usage (Vendor Usage 0x28)          92
    0x09, 0x29,                    //  Usage (Vendor Usage 0x29)          94
    0x09, 0x2a,                    //  Usage (Vendor Usage 0x2a)          96
    0x09, 0x2b,                    //  Usage (Vendor Usage 0x2b)          98
    0x95, 0x0c,                    //  Report Count (12)                  100
    0x81, 0x02,                    //  Input (Data,Var,Abs)               102
    0x0a, 0x21, 0x26,              //  Usage (Vendor Usage 0x2621)        104
    0x95, 0x08,                    //  Report Count (8)                   107
    0xb1, 0x02,                    //  Feature (Data,Var,Abs)             109
    0x0a, 0x21, 0x26,              //  Usage (Vendor Usage 0x2621)        111
    0x91, 0x02,                    //  Output (Data,Var,Abs)              114
    0x26, 0xff, 0x03,              //  Logical Maximum (1023)             116
    0x46, 0xff, 0x03,              //  Physical Maximum (1023)            119
    0x09, 0x2c,                    //  Usage (Vendor Usage 0x2c)          122
    0x09, 0x2d,                    //  Usage (Vendor Usage 0x2d)          124
    0x09, 0x2e,                    //  Usage (Vendor Usage 0x2e)          126
    0x09, 0x2f,                    //  Usage (Vendor Usage 0x2f)          128
    0x75, 0x10,                    //  Report Size (16)                   130
    0x95, 0x04,                    //  Report Count (4)                   132
    0x81, 0x02,                    //  Input (Data,Var,Abs)               134
    0xc0                          // End Collection                      136
};

// Tabela de IDs
// Estou tratando apenas um dispositivo portanto só há um no array
static struct hid_device_id device_table[] = {
    { HID_USB_DEVICE(GUITAR_ID_VENDOR, GUITAR_ID_PRODUCT), .driver_data = 1 << 14 },
    {  } // Tem de haver um indice vazio por algum motivo
};

// Setando tabela de dispositivos
MODULE_DEVICE_TABLE(hid, device_table);

// Executada quando o Dongle USB é conectado
static int guitar_connect(struct hid_device *interface, const struct hid_device_id *id) {
    printk("Guitarra conectada\n");

    return 0;
}

// Executada quando o Dongle USB é desconectado
static void guitar_disconnect(struct hid_device *inferface) {
    printk("Guitarra desconectada\n");
}

// Não sei exatamente o que isso faz
static const u8 *guitar_report_fixup(struct hid_device *hdev, u8* rdesc, u32 *rsize) {
    *rsize = sizeof(guitar_report);
    return guitar_report;
}

// Configurando driver
static struct hid_driver guitar = {
    .name         = "Guitarra RedOctane PS3",
    .id_table     = device_table,
    .probe        = guitar_connect,
    .remove       = guitar_disconnect,
    .report_fixup = guitar_report_fixup
};

// Ponto de entrada do driver
static int __init guitarInit(void) {
    printk("Modulo inicializado");

    return hid_register_driver(&guitar);
}

// Ponto de saída do driver
static void __exit guitarExit(void) { 
    printk("Modulo terminado");

    hid_unregister_driver(&guitar);
}

// Configurando pontos de entrada e saída
module_init(guitarInit);
module_exit(guitarExit);
