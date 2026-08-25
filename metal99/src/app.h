/*
 * The application boundary. Pure ISO C99.
 *
 * WHY. Everything an interface did used to live inline in main.c, tangled
 * through boot, clock setup, the self-test and the pacing loop - 597 lines in
 * which changing a button meant editing the firmware's entry point. An
 * application could not be written, reviewed or replaced without risking the
 * parts that make the board come up.
 *
 * An app is now three callbacks and a name. main.c owns bringing the hardware
 * up and proving it works; the app owns what is on the screen.
 *
 * THE SAME APP RUNS ON THE HOST. tests/host/gfx_png links this interface
 * against the real gfx and elide, so a layout can be rendered to an image and
 * LOOKED at without a board - which for most of this project's life was the
 * one thing that could not be done. The simulator synthesises events; the app
 * cannot tell the difference.
 *
 * There is exactly one app, resolved at link time. No registry, no dispatch: a
 * bare-metal image runs one program, and pretending otherwise would be
 * machinery in place of a decision.
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include "ui.h"

typedef struct {
    const char *name;

    /* Once, after gfx_init(). Draw anything that does not change here. */
    void (*init)(void);

    /* Once per frame, before gfx_present(). `f` counts frames since start.
     * DESCRIBE the whole scene: gfx diffs it against what the panel holds, so
     * redescribing something unchanged costs nothing (DESIGN.md 5.2). */
    void (*frame)(uint32_t f);

    /* For each input event, before frame(). May be NULL. */
    void (*event)(const ui_event *e);
} app_t;

/* Defined by exactly one application source file. */
extern const app_t APP;

#endif /* APP_H */
