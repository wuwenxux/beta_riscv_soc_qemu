/*
 * Beta SoC - Production MCPU BootROM (RV32)
 *
 * Boot flow:
 *   1. Initialize UART at 0xD0087000 (regshift=2, ns16550a)
 *   2. Print banner
 *   3. Set per-core ACPU_BOOT_ADDR_L/H to DDR0 base (0x4000000000)
 *   4. Write SW_RST_CTRL1 to release all 32 ACPU cores
 *   5. Spin
 *
 * SOC_TOP_CSR base from MCPU address space:
 *   TBD — currently using 0xE0000000 as placeholder
 *   (assumed MCPU IO Space window at 0xE000_0000 maps to 0x01_0000_0000)
 */

/* UART (ns16550a, regshift=2 → registers at 4-byte intervals) */
#define UART_BASE       0xD0087000
#define UART_THR        (UART_BASE + 0x00)
#define UART_LSR        (UART_BASE + 0x14)
#define UART_LSR_THRE   0x20
#define UART_LCR        (UART_BASE + 0x0C)
#define UART_DLL        (UART_BASE + 0x00)
#define UART_DLM        (UART_BASE + 0x04)
#define UART_FCR        (UART_BASE + 0x08)

/*
 * SOC_TOP_CSR base from MCPU address space:
 * MCPU IO Space @ 0xE000_0000 (256MB) maps to global 0x01_0000_0000
 * (from MGMT sheet: "MCPU IO Space" 0xE000_0000 - 0xEFFF_FFFF)
 */
#define SOC_CSR_BASE    0xE0000000

/* SOC_TOP_CSR register offsets (from SOC_TOP_CSR寄存器表单.xlsx) */
#define ACPU_BOOT_ADDR_L(n)  (SOC_CSR_BASE + 0x000 + (n) * 4)
#define ACPU_BOOT_ADDR_H(n)  (SOC_CSR_BASE + 0x100 + (n) * 4)
#define SW_RST_CTRL1         (SOC_CSR_BASE + 0x810)

/* DDR0 base address (where OpenSBI is loaded) */
#define DDR0_BASE_L     0x00000000
#define DDR0_BASE_H     0x00000040    /* 0x40_0000_0000 >> 32 */

/* Number of ACPU cores */
#define NUM_ACPU        32

    .section .text.start
    .global _start
_start:
    /* Set up stack */
    li   sp, 0x00010000

    /* Initialize UART */
    call uart_init

    /* Print banner */
    la   a0, msg_banner
    call uart_puts
    la   a0, msg_ready
    call uart_puts

    /* Set boot address for all 32 ACPU cores */
    la   a0, msg_bootaddr
    call uart_puts

    li   t0, 0           /* core index */
    li   t3, NUM_ACPU
set_boot_addr_loop:
    bge  t0, t3, set_boot_addr_done

    /* ACPU_BOOT_ADDR_L[core] = DDR0_BASE_L */
    slli t1, t0, 2       /* t1 = core * 4 */
    li   t2, SOC_CSR_BASE
    add  t2, t2, t1      /* t2 = SOC_CSR_BASE + core*4 */
    li   t4, DDR0_BASE_L
    sw   t4, 0(t2)       /* ACPU_BOOT_ADDR_L[core] = 0x00000000 */

    /* ACPU_BOOT_ADDR_H[core] = DDR0_BASE_H */
    addi t2, t2, 0x100   /* offset 0x100 for _H registers */
    li   t4, DDR0_BASE_H
    sw   t4, 0(t2)       /* ACPU_BOOT_ADDR_H[core] = 0x40 */

    addi t0, t0, 1
    j    set_boot_addr_loop
set_boot_addr_done:

    /* Release all 32 ACPU cores via SW_RST_CTRL1 */
    /* bits[31:1] = 1 (release cores 0-30), bit 0 = BACPU (leave 0) */
    la   a0, msg_releasing
    call uart_puts

    li   t0, SOC_CSR_BASE
    li   t1, 0xFFFFFFFE  /* bits 31:1 = all 1s */
    li   t2, 0x810        /* SW_RST_CTRL1 offset */
    add  t0, t0, t2
    sw   t1, 0(t0)

    la   a0, msg_released
    call uart_puts

spin:
    wfi
    j    spin

/* ---------------------------------------------------------------------------
 * UART functions (ns16550a, regshift=2)
 * -------------------------------------------------------------------------*/
uart_init:
    li   t0, UART_BASE
    /* Set DLAB to access divisor latches */
    li   t1, 0x83
    sb   t1, 0x0C(t0)      /* LCR: DLAB=1, 8N1 */
    /* Set divisor = 2 (baud 115200 @ 3.6864MHz) */
    li   t1, 2
    sb   t1, 0x00(t0)      /* DLL = 2 */
    sb   zero, 0x04(t0)    /* DLM = 0 */
    /* Clear DLAB, set 8N1 */
    li   t1, 0x03
    sb   t1, 0x0C(t0)      /* LCR: 8N1, DLAB=0 */
    /* Enable and reset FIFOs */
    li   t1, 0xC7
    sb   t1, 0x08(t0)      /* FCR */
    /* Disable all interrupts */
    sb   zero, 0x04(t0)    /* IER = 0 */
    ret

uart_putc:
    li   t0, UART_BASE
1:  lb   t1, 0x14(t0)      /* LSR */
    andi t1, t1, 0x20      /* THRE */
    beqz t1, 1b
    sb   a0, 0x00(t0)
    ret

uart_puts:
    mv   t2, ra
    mv   t3, a0
1:  lb   a0, 0(t3)
    beqz a0, 2f
    call uart_putc
    addi t3, t3, 1
    j    1b
2:  mv   ra, t2
    ret

/* ---------------------------------------------------------------------------
 * Data
 * -------------------------------------------------------------------------*/
    .section .rodata
msg_banner:
    .asciz "\r\n=== Beta SoC Production MCPU BootROM ===\r\n"
msg_ready:
    .asciz "MCPU: Boot ROM running, UART OK\r\n"
msg_bootaddr:
    .asciz "MCPU: Setting ACPU boot addresses to DDR0...\r\n"
msg_releasing:
    .asciz "MCPU: Releasing all 32 ACPU cores via SW_RST_CTRL1...\r\n"
msg_released:
    .asciz "MCPU: All ACPU cores released. MCPU idle.\r\n"
