/* The ROM bootloader hands control over with the hardware watchdogs armed and
 * expects the loaded image to take responsibility for them. Nothing else will,
 * so this must run before anything slow. Addresses: ESP32-S3 TRM. */
#include "io.h"

#define TIMG0_WDTCONFIG0 REG32(0x6001F048u)
#define TIMG0_WDTPROTECT REG32(0x6001F064u)
#define TIMG1_WDTCONFIG0 REG32(0x60020048u)
#define TIMG1_WDTPROTECT REG32(0x60020064u)
#define RTC_WDTCONFIG0   REG32(0x60008098u)
#define RTC_WDTPROTECT   REG32(0x600080B0u)
#define RTC_SWD_CONF     REG32(0x600080B4u)
#define RTC_SWD_PROTECT  REG32(0x600080B8u)

#define MWDT_WKEY 0x50D83AA1u
#define RWDT_WKEY 0x50D83AA1u
#define SWD_WKEY  0x8F1D312Au
#define SWD_AUTO_FEED (1u << 31)

void wdt_disable_all(void)
{
    TIMG0_WDTPROTECT = MWDT_WKEY;  TIMG0_WDTCONFIG0 = 0u;  TIMG0_WDTPROTECT = 0u;
    TIMG1_WDTPROTECT = MWDT_WKEY;  TIMG1_WDTCONFIG0 = 0u;  TIMG1_WDTPROTECT = 0u;
    RTC_WDTPROTECT   = RWDT_WKEY;  RTC_WDTCONFIG0   = 0u;  RTC_WDTPROTECT   = 0u;

    /* The super-watchdog cannot be disabled, only set to feed itself. */
    RTC_SWD_PROTECT = SWD_WKEY;
    RTC_SWD_CONF    = RTC_SWD_CONF | SWD_AUTO_FEED;
    RTC_SWD_PROTECT = 0u;
}
