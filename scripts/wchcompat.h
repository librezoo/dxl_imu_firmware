/* Maps WCH's patched-GCC interrupt attribute onto the standard one so the
   firmware builds with the stock xPack riscv-none-elf-gcc. The CH32V20x
   hardware prologue (HPE) saves registers; the extra software save is harmless. */
#define interrupt(x) interrupt("machine")
