#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//#define CFG_TUSB_MCU             OPT_MCU_RP2350
//#define CFG_TUSB_OS              OPT_OS_NONE
//#define CFG_TUSB_DEBUG           0

#if USB_HOST_MODE

/* ================= USB HOST role (HID keyboard + mouse via hub) ================
 * Selected by -DUSB_HOST_MODE=1 (CMake option USB_HOST_MODE). The single native
 * PHY is host OR device, so only the host stack is configured here. Buffers are
 * kept minimal: the ~480 KB framebuffer leaves little SRAM spare. */
#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED

#define CFG_TUH_HUB                 1           /* keyboard AND mouse -> need a hub */
#define CFG_TUH_HID                 4           /* HID interface instances (kbd, mouse, combos) */
/* Downstream devices behind the hub (hub itself is counted separately). */
#define CFG_TUH_DEVICE_MAX          (CFG_TUH_HUB + 3)
#define CFG_TUH_ENUMERATION_BUFSIZE 256

#else

/* ================= USB DEVICE role (CDC console) ============================= */
#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED

#define CFG_TUD_ENDPOINT0_SIZE     64

/* ------ Device Classes Enabled ------ */
#define CFG_TUD_CDC                 1           /* one CDC interface (console) */
#define CFG_TUD_VENDOR              0

#define CFG_TUD_MSC                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MIDI                0

/* ------ CDC Buffer Sizes ------ */
#define CFG_TUD_CDC_RX_BUFSIZE     256
#define CFG_TUD_CDC_TX_BUFSIZE     256
#define CFG_TUD_CDC_EP_BUFSIZE     64

#endif /* USB_HOST_MODE */

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
