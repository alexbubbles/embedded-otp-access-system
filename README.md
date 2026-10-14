# Bare-Metal AVR Dual-Factor Access Control System (PIN + OTP)

A bare-metal embedded access control firmware written in C and AVR Inline Assembly for the **ATmega328P**, simulated on **Wokwi**. 

The system implements a complete two-factor authentication (2FA) pipeline combining a static 6-digit PIN with a time-sensitive, dynamically generated 6-digit One-Time Password (OTP), without relying on external libraries or the Arduino framework.

---

## Key Technical Highlights

* **Zero-Branching FSM Architecture:** Both the system workflow (7 states), the non-blocking keypad debouncer (4 states), and the main loop task dispatcher are driven by **Function Pointer Lookup Tables**, avoiding `switch-case` overhead.
* **Cycle-Accurate Inline AVR Assembly:** Shift register serial transmission (`send_serial_byte`) is written directly in AVR assembly (`sbi`, `cbi`, `sbrc`, `sbrs`) to achieve deterministic bit-banging speeds during display refresh interrupts.
* **Interrupt-Driven 6-Digit Multiplexing:** `Timer2` (CTC Mode, 1 kHz) drives a 6-digit Common Anode 7-segment display via a shift register, completely decoupled from the main execution thread.
* **XORshift PRNG with Live Entropy:** Generates a 6-digit OTP using a 16-bit XORshift algorithm seeded by a free-running entropy counter (`fast_entropy_counter`) incremented in the main loop, making the OTP dependent on exact user interaction timing.
* **Hardware-Timed OTP Window:** `Timer1` (16-bit CTC Mode, 1 Hz tick) enforces a strict **10-second countdown** to complete the second authentication factor before locking the system.
* **Event-Driven Wake-Up:** Uses External Interrupt `INT0` triggered by a PIR motion sensor to wake the controller from a low-activity standby state (display blanked, keypad polling suspended).

---

## System Workflow & Authentication Protocol

1. **Standby (`ST_STANDBY`):** The display is powered off (`PORTB` anodes disabled) and the system waits for motion.
2. **Wake-Up (`ST_WAIT_STAR`):** When the PIR sensor triggers `INT0`, the display lights up showing `------`. Press `*` to initiate login.
3. **PIN Entry (`ST_ENTER_PIN`):** Enter the 6-digit master PIN (default: `123456`).
   * *Match:* Transitions to `ST_WAIT_HASH`, generates a 6-digit OTP, transmits it over **UART (9600 bps)**, and starts the **10s hardware timer**.
   * *Mismatch:* Transitions immediately to `ST_FAIL` (displays `000000`).
4. **OTP Prompt (`ST_WAIT_HASH`):** Press `#` to begin entering the received OTP while the timer is running.
5. **OTP Verification (`ST_ENTER_OTP`):** Enter the 6-digit OTP before the 10-second `Timer1` window expires.
   * *Valid & On Time:* Halts `Timer1` and transitions to `ST_OPEN` (displays `111111`).
   * *Invalid or Timeout:* Transitions to `ST_FAIL` (displays `000000`).
6. **Quick Sleep Combo (`*` then `#`):** Pressing `*` followed immediately by `#` during active non-terminal states resets the controller back to `ST_STANDBY` and turns off the display.

---

## Hardware Pinout (ATmega328P)

| Peripheral | Function | AVR Pin / Register | Arduino Equivalent | Direction |
| :--- | :--- | :--- | :--- | :--- |
| **PIR Sensor** | Wake-Up Interrupt (`INT0`) | `PD2` | D2 | Input (Pull-up) |
| **Shift Register (74HC595)** | Serial Data (`DS`) | `PD4` (`0x0B, bit 4`) | D4 | Output |
| **Shift Register (74HC595)** | Shift Clock (`SHCP`) | `PD5` (`0x0B, bit 5`) | D5 | Output |
| **Shift Register (74HC595)** | Latch Clock (`STCP`) | `PD6` | D6 | Output |
| **7-Segment Display** | Digit Anodes 1–6 | `PB0` – `PB5` | D8 – D13 | Output |
| **Matrix Keypad (4x3)** | Rows 0–3 | `PC0` – `PC3` | A0 – A3 | Output |
| **Matrix Keypad (4x3)** | Columns 0–1 | `PC4` – `PC5` | A4 – A5 | Input (Pull-up) |
| **Matrix Keypad (4x3)** | Column 2 | `PD3` | D3 | Input (Pull-up) |
| **UART Serial** | OTP Output (`TXD`) | `PD1` | D1 (TX) | Output (9600 8N1) |

---

## Software Architecture

The firmware is organized into 6 clean layers siguiendo il pattern **Model-View-Controller (MVC)** adattato per sistemi bare-metal:

1. **Model (State & ROM):** Holds system state constants, the `SEV_SEG_MAP` lookup table, the `display_buffer` VRAM, and volatile flags shared with ISRs.
2. **Hardware Abstraction Layer (HAL):** Bare-metal drivers for `USART0` (`9600 bps` @ 16 MHz), `Timer1` CTC initialization, and the XORshift OTP generator.
3. **Background Services (View & ISRs):**
   * `ISR(INT0_vect)`: Asynchronous wake-up handler.
   * `ISR(TIMER2_COMPA_vect)`: Multiplexes the 6 digits at 1 kHz (refresh rate ~166 Hz per digit) using inline assembly bit-banging.
4. **Foreground Controller (System & Keypad FSMs):**
   * `keypad_fsm_table[4]`: 4-state non-blocking debouncer (`idle` $\rightarrow$ `verify_press` $\rightarrow$ `wait_release` $\rightarrow$ `verify_release`).
   * `system_fsm_table[7]`: Handles state transitions and PIN/OTP validation.
5. **Task Dispatcher:** `loop_task_table[7]` maps the current system state to its execution context (`standby`, `active_standard`, or `active_timer` which polls `TIFR1` for `OCF1A` 1-second ticks).
6. **Setup & Main Loop:** Configures `DDRx`/`PORTx` registers, timers, external interrupts, global `SREG` I-bit, and runs the entropy accumulator.

---

## Running the Project

### Run on Wokwi
1. Open the simulation on [Wokwi](<INSERT_YOUR_WOKWI_PROJECT_LINK_HERE>).
2. Click **Start Simulation**.
3. Trigger the **PIR Motion Sensor** to wake up the display (`------`).
4. Press `*`, then type `123456`.
5. Check the **Serial Monitor** for the generated 6-digit OTP.
6. Within 10 seconds, press `#` and type the 6-digit OTP to unlock (`111111`).

### Compile Locally with `avr-gcc`
```bash
avr-gcc -Wall -Os -mmcu=atmega328p -DF_CPU=16000000UL main.c -o firmware.elf
avr-objcopy -O ihex -R .eeprom firmware.elf firmware.hex
