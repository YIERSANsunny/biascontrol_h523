# UART Command "No Response" Troubleshooting

Date: 2026-04-20
Platform: STM32H523CET6 + newlib-nano + ST-Link VCP

## Summary

During UART bring-up, the board appeared to have a one-way serial link:

- Boot logs printed correctly from MCU to PC
- Commands sent from the PC seemed to get no reply
- The symptom looked like a UART RX wiring or peripheral problem

The real root cause was different:

- The MCU did receive the command
- The command handler started executing
- The reply path hit `printf("%.3f")`
- Float formatting overflowed the Cortex-M33 stack
- The firmware entered a HardFault before the reply completed

This issue was fixed by increasing `_Min_Stack_Size` from `0x1000` to `0x2000`
in `cubemx/STM32H523xx_FLASH.ld`.

## Observed Symptoms

The following behavior was seen on the bench:

1. Power-on logs were visible over `USART1` at `115200 8N1`
2. Commands such as `status`, `set bp custom 45`, and `set pilot 100`
   produced no visible response
3. The problem looked like "TX works, RX does not"

This was misleading because the lack of a visible reply was caused by a crash
in the response formatting path, not by a missing receive path.

## What Was Actually Happening

The command path was healthy:

1. `USART1` received bytes from the host
2. `app_uart_process()` drained the RX register and assembled one ASCII line
3. `app_handle_command(cmd)` was called in main-loop context
4. The `status` branch started printing the reply
5. The first float-formatting call used the newlib-nano float conversion path
6. Stack usage exceeded the CubeMX default stack reservation
7. Cortex-M33 raised a stack overflow HardFault

The UART receive implementation is in:

- `app/src/app_uart.c`

The `status` reply path is in:

- `app/src/app_main.c`

Relevant `status` reply lines include float prints such as:

```c
printf("Bias:  %.3f V\r\n", (double)bias_ctrl_get_bias_voltage(&ctx.bias_ctrl));
printf("Cal:   Vpi=%.3fV  null=%+.3fV  peak=%+.3fV  quad+=%+.3fV\r\n", ...);
```

These calls are safe only when the stack is large enough for float formatting.

## Root Cause

### Immediate Cause

The linker script still used the CubeMX default stack size:

```ld
_Min_Stack_Size = 0x1000;
```

On STM32H5 with newlib-nano float formatting enabled, this stack size was too
small for `%f` / `%.3f` formatting in the command reply path.

### Why Float `printf` Is Special

Integer formatting such as `%d` is relatively cheap: it mostly performs simple
division and digit extraction.

Float formatting is much heavier:

- It converts binary floating-point data into decimal text
- It applies precision and rounding rules
- It handles signs, fractional digits, and special cases
- It enters deeper library code such as `_dtoa_r`

That means `%f` consumes much more stack than `%d` or `%s`.

### Project-Specific Note

This repository already documented the issue in `CLAUDE.md`:

- `-u _printf_float` is required to enable float `printf`
- `_Min_Stack_Size` must be at least `0x2000`
- Keeping the default `0x1000` causes a Cortex-M33 `STKOF` HardFault

In other words, the problem was a known platform constraint that had not yet
been applied in the active linker script.

## Evidence That Confirmed The Diagnosis

Several observations ruled out a pure UART wiring problem:

### 1. Boot Logs Proved TX Was Fine

The board printed normal startup text, for example:

```text
[board] init ok, SYSCLK=250 MHz
[adc] ID=0x22 ok
[adc] configured: OSR=128, GAIN=1, 64kSPS
[app] hardware: ADC=READY DAC=READY
```

So `MCU -> PC` transmission was already working.

### 2. Internal Buffers Proved Commands Were Received

Debugger reads showed that command buffers contained strings such as `status`,
which means the firmware had already received and parsed the host input.

So `PC -> MCU` reception was also working.

### 3. The Failing Commands Shared One Trait

Problematic commands all produced replies that included float formatting:

- `status`
- `set bp custom 45`
- `set pilot 100`

This strongly pointed to the reply formatting path rather than the receive path.

### 4. The Fix Removed The Symptom Immediately

After changing the linker stack to `0x2000`, rebuilding, and reflashing:

- `status` replied correctly
- `set bp quad` replied correctly
- `set bp custom 45` replied correctly
- `set pilot 100` replied correctly

That closes the loop on the diagnosis.

## Fix Applied

The linker script was updated from:

```ld
_Min_Stack_Size = 0x1000;
```

to:

```ld
_Min_Stack_Size = 0x2000;
```

File:

- `cubemx/STM32H523xx_FLASH.ld`

No functional UART pin or peripheral change was required to solve this specific
problem.

## Verified Result After The Fix

After rebuilding and flashing with `pyocd`:

```text
status
State: IDLE
HW:    ADC=READY  DAC=READY
Bias:  0.000 V
Lock:  NO
Cal:   INVALID

set bp quad
[bp] Quadrature

set bp custom 45
[bp] custom 45.0 deg (0.7854 rad) ...

set pilot 100
[pilot] 100 mVpp (peak=50.0 mV), scan clamp=+/-9.950 V
```

This verified that the UART receive path, command dispatch path, and reply path
were all functioning correctly.

## How To Troubleshoot Similar Problems In The Future

When serial commands appear to be ignored, use the following checklist.

### Step 1: Separate TX Failure From RX Failure

Ask:

- Does the board print anything at boot?
- If yes, TX is already working
- If no, investigate clock, pinmux, baud rate, `_write()`, and cable/probe setup

Do not assume "no command reply" means RX is broken.

### Step 2: Determine Whether The MCU Received The Command

Use one of the following:

- A debugger watch on the command buffer
- Temporary instrumentation in `app_uart_process()`
- A simple echo command with no float formatting

If the command buffer contains the host text, RX is not the root cause.

### Step 3: Check Whether The Reply Path Uses Float `printf`

Search the command handler for:

- `%f`
- `%.3f`
- `%e`
- other float formatting

If the failing command prints floats, immediately inspect stack size.

### Step 4: Inspect Linker Settings

Check:

- `CMakeLists.txt` includes `-u _printf_float`
- `cubemx/STM32H523xx_FLASH.ld` uses `_Min_Stack_Size = 0x2000`

If float formatting is enabled but the stack is still `0x1000`, suspect this
issue first.

### Step 5: Check HardFault Status Registers

For Cortex-M33 stack overflow diagnosis:

- Read `CFSR` at `0xE000ED28`
- `0x00100000` indicates `STKOF` (stack overflow)

This is a strong confirmation when the board appears to "silently hang".

### Step 6: Compare A Float-Free Command With A Float-Heavy Command

This is a fast isolation trick:

- Try a command that prints only short strings
- Compare it with a command that prints multiple floats

If only the float-heavy command fails, the stack hypothesis becomes much stronger.

### Step 7: Reduce Variables

If the diagnosis is unclear:

- Use polling RX instead of DMA temporarily
- Avoid ISR-side command dispatch
- Remove complex formatting from the first debug build

This makes it easier to distinguish transport problems from application crashes.

## Prevention Guidelines

To avoid this class of issue in future bring-up work:

1. Keep `_Min_Stack_Size >= 0x2000` whenever float `printf` is enabled on STM32H5
2. Do not add `%f` logging casually in low-level bring-up code
3. Prefer integer-scaled logs for routine diagnostics, for example:
   - print `50 mV` instead of `0.050 V`
   - print `785 mrad` instead of `0.7854 rad`
4. Keep command handling in main-loop context, not in UART ISR context
5. Treat "no reply" as a generic symptom, not proof of an RX hardware fault
6. Document bench findings immediately after confirmation

## Short Version

If UART boot logs work but `status` seems to get no reply, check stack size
before rewiring the board.

In this project, the command was received correctly. The firmware crashed while
formatting float output, and increasing `_Min_Stack_Size` from `0x1000` to
`0x2000` fixed the problem.
