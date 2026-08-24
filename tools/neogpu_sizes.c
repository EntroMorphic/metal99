/*
 * Measure NeoGPU's message-layer struct sizes and the static cost of its
 * capacity constants. Produced the table in docs/DESIGN.md 7.4.
 *
 *   gcc -I tools/compat -I <neogpu>/include tools/neogpu_sizes.c -o /tmp/sizes
 *
 * Pointer-heavy structs measure larger on x86-64 than on Xtensa (8 vs 4 byte
 * pointers); Message/Payload/HSSubmitSlot are POD and unaffected.
 */
#include <stdio.h>
#include "hs_core.h"

int main(void)
{
    double lg = (double)sizeof(Message) * 65536.0;
    double pl = (double)sizeof(Payload) * 4096.0;
    double sq = (double)sizeof(HSSubmitSlot) * 1024.0;

    printf("Message      = %3zu B\n", sizeof(Message));
    printf("Payload      = %3zu B\n", sizeof(Payload));
    printf("HSSubmitSlot = %3zu B\n\n", sizeof(HSSubmitSlot));

    printf("DESKTOP  log 65536 = %7.1fK  payload 4096 = %7.1fK  submit 1024 = %7.1fK  TOTAL %7.1fK\n",
           lg / 1024, pl / 1024, sq / 1024, (lg + pl + sq) / 1024);

    lg = (double)sizeof(Message) * 512.0;
    pl = (double)sizeof(Payload) * 128.0;
    sq = (double)sizeof(HSSubmitSlot) * 64.0;
    printf("EMBEDDED log   512 = %7.1fK  payload  128 = %7.1fK  submit   64 = %7.1fK  TOTAL %7.1fK\n",
           lg / 1024, pl / 1024, sq / 1024, (lg + pl + sq) / 1024);
    printf("\nESP32-S3 internal SRAM = 512K; metal99 linker regions claim 320K\n");
    return 0;
}
