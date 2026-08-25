/*
 * FT3168 capacitive touch over I2C. Pure ISO C99.
 *
 * BOARD REVISION MATTERS, again. This is the ORIGINAL revision's controller,
 * the FocalTech FT3168, which speaks the FT5x06 register protocol at address
 * 0x38. Waveshare's V2 board carries a CST816/CST820 instead - a different
 * chip with a different register map - so this driver would read nonsense
 * there, in the same way the CO5300 panel driver draws nothing on an SH8601.
 * touch_init() checks the vendor ID rather than assuming.
 *
 * THERE IS NO RESET PIN. BSP_LCD_TOUCH_RST is not connected on this board,
 * exactly as the panel's is not, so the controller can only be addressed as it
 * comes up.
 *
 * The INT line (GPIO21) goes low while a finger is present. Polling it is a
 * GPIO read; polling the controller is an I2C transaction of ~150 us at
 * 400 kHz. touch_poll() checks the pin first so an idle screen costs a register
 * read rather than a bus transaction - the same "do not pay for what did not
 * change" reasoning the graphics layer is built on.
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

#define TOUCH_MAX_POINTS 2      /* FT3168 reports two; the struct bounds the read */

#define TOUCH_OK        0
#define TOUCH_E_ABSENT (-1)     /* nothing ACKed at 0x38                     */
#define TOUCH_E_ID     (-2)     /* answered, but not a FocalTech part        */
#define TOUCH_E_IO     (-3)     /* I2C error - see i2c.h for which           */

typedef struct {
    uint16_t x, y;
    uint8_t  id;                /* controller's tracking id, stable per contact */
} touch_point;

typedef struct {
    uint8_t     n;              /* contacts currently down, 0..TOUCH_MAX_POINTS */
    touch_point p[TOUCH_MAX_POINTS];
} touch_state;

/* Probe the controller and read its identity. Returns TOUCH_OK if an FT-family
 * part answered. */
int touch_init(void);

/* Current contacts. Cheap when nothing is touching: the INT pin is checked
 * first and no I2C happens if it is high. */
int touch_poll(touch_state *st);

/* Poll accounting, used to decide whether INT gating is safe to enable:
 * int_low should track with_touch closely if the line is an active-low level. */
int      touch_dbg_io(void);   /* last i2c error code from touch_poll */
uint32_t touch_dbg_polls(void);
uint32_t touch_dbg_int_low(void);
uint32_t touch_dbg_with_touch(void);

/* Chip and vendor id read at init, for diagnostics. */
uint8_t touch_chip_id(void);
uint8_t touch_vendor_id(void);

#endif /* TOUCH_H */
