// ==============================================================================
// 1. MODEL: DATI, STATO DEL SISTEMA E ROM
// ==============================================================================

// [COSTANTI DI STATO]
const unsigned char ST_STANDBY   = 0;
const unsigned char ST_WAIT_STAR = 1;
const unsigned char ST_ENTER_PIN = 2;
const unsigned char ST_WAIT_HASH = 3;
const unsigned char ST_ENTER_OTP = 4;
const unsigned char ST_FAIL      = 5;
const unsigned char ST_OPEN      = 6;

// [VOLATILE]
volatile unsigned char current_sys_state = 0; 
volatile unsigned char display_buffer[6] = {11, 11, 11, 11, 11, 11}; 
volatile unsigned char pir_triggered_flag = 0; 

// [DATI SENSIBILI E BUFFER]
const unsigned char CORRECT_PIN[6] = {1, 2, 3, 4, 5, 6}; 
char generated_otp[6]; 
unsigned char buffer_pos = 0; 
unsigned char otp_seconds_left = 0; 

// [ENTROPIA E TASTIERA]
volatile unsigned int fast_entropy_counter = 0xACE1; 
char last_pressed_key = 0; 

const char CHAR_MAP[4][3] = {{'1','2','3'}, {'4','5','6'}, {'7','8','9'}, {'*','0','#'}};
static unsigned char key_state = 0;
static unsigned char last_coord = 0xFF;
static unsigned char current_coord = 0xFF;
static unsigned int debounce_timer = 0;

// Mappatura: 0bGFEDCBAx 
const unsigned char SEV_SEG_MAP[12] = {
    0b00111111, // 0 
    0b00000110, // 1 
    0b01011011, // 2 
    0b01001111, // 3 
    0b01100110, // 4 
    0b01101101, // 5 
    0b01111101, // 6 
    0b00000111, // 7 
    0b01111111, // 8 
    0b01101111, // 9 
    0b01000000, // 10: Trattino '-'
    0b00000000  // 11: Vuoto
};

// ==============================================================================
// 2. HARDWARE ABSTRACTION LAYER: BARE METAL DRIVERS
// ==============================================================================

void uart_init() {
    UBRR0H = 0; 
    UBRR0L = 103; // 9600 bps a 16MHz
    UCSR0B = (1 << TXEN0); 
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); 
}

void uart_tx(char c) {
    while (!(UCSR0A & (1 << UDRE0))); 
    UDR0 = c; 
}

void uart_print(const char* str) {
    while (*str) uart_tx(*str++);
    uart_tx('\r'); uart_tx('\n'); 
}

void generate_and_print_otp() {
    unsigned int state = fast_entropy_counter; 
    for(int i = 0; i < 6; i++) {
        state ^= state << 7; state ^= state >> 9; state ^= state << 8;
        unsigned char digit = state & 0x0F; 
        if (digit > 9) digit -= 6; 
        generated_otp[i] = digit + '0'; 
    }
    for(int i=0; i<6; i++) uart_tx(generated_otp[i]); 
    uart_tx('\r'); uart_tx('\n');
}

void start_otp_timer() {
    otp_seconds_left = 10; 
    TCCR1A = 0; 
    TCCR1B = (1 << WGM12) | (1 << CS12) | (1 << CS10); // CTC, Prescaler 1024
    OCR1A = 15624; // 1.0 secondi
    TIFR1 = (1 << OCF1A); 
}

void set_display_dash() { for(int i=0; i<6; i++) display_buffer[i] = 10; } 
void set_display_fail() { for(int i=0; i<6; i++) display_buffer[i] = 0; }  
void set_display_open() { for(int i=0; i<6; i++) display_buffer[i] = 1; }  
void set_display_blank() { for(int i=0; i<6; i++) display_buffer[i] = 11; } 

// ==============================================================================
// 3. BACKGROUND: ISR BARE METAL (VIEW E SENSORI)
// ==============================================================================

void send_serial_byte(unsigned char val) {
    asm volatile (
        "ldi r25, 8 \n\t"
        "lwhile%=:\n\t"
        "sbrc %0, 7 \n\t" "sbi 0x0B, 4 \n\t" // D4 (Data) HIGH
        "sbrs %0, 7 \n\t" "cbi 0x0B, 4 \n\t" // D4 (Data) LOW
        "nop \n\t nop \n\t nop \n\t nop \n\t"
        "sbi 0x0B, 5 \n\t"                   // D5 (Clock) HIGH
        "nop \n\t nop \n\t" 
        "cbi 0x0B, 5 \n\t"                   // D5 (Clock) LOW
        "nop \n\t nop \n\t" 
        "add %0, %0 \n\t"                    // Shift
        "subi r25, 1 \n\t"
        "brne lwhile%= \n\t"
        : "+r" (val) : : "r25"   
    );
}

ISR(INT0_vect) {
    if (current_sys_state == ST_STANDBY) {
        current_sys_state = ST_WAIT_STAR;
        set_display_dash();
        pir_triggered_flag = 1; 
    }
}

ISR(TIMER2_COMPA_vect) {
    if (current_sys_state == ST_STANDBY) {
        PORTB &= ~0x3F; 
        return;
    }
    static unsigned char current_digit = 0;
    
    PORTB &= ~0x3F; // Spegne transistor
    unsigned char mask = ~SEV_SEG_MAP[display_buffer[current_digit]]; // Anodo comune
    send_serial_byte(mask); 
    
    PORTD |= (1 << 6);  // D6 (Latch) HIGH
    PORTD &= ~(1 << 6); // D6 (Latch) LOW
    
    PORTB |= (1 << current_digit); // Riaccende l'alimentazione cifra corrente
    
    current_digit++;
    if (current_digit >= 6) current_digit = 0;
}

// ==============================================================================
// 4. FOREGROUND: CONTROLLER LOGICO E FSM TASTIERA
// ==============================================================================

unsigned char check_arrays_match(const unsigned char* input, const void* target, unsigned char is_otp) {
    for(int i = 0; i < 6; i++) {
        unsigned char target_val = is_otp ? (((const char*)target)[i] - '0') : ((const unsigned char*)target)[i];
        if (input[i] != target_val) return 0; 
    }
    return 1; 
}

void sys_st_standby(char key) { }
void sys_st_wait_star(char key) {
    if (key == '*') { current_sys_state = ST_ENTER_PIN; buffer_pos = 0; set_display_dash(); }
}
void sys_st_enter_pin(char key) {
    if (key >= '0' && key <= '9' && buffer_pos < 6) {
        display_buffer[buffer_pos] = key - '0'; buffer_pos++;
        if (buffer_pos == 6) { 
            if (check_arrays_match((const unsigned char*)display_buffer, (const void*)CORRECT_PIN, 0)) {
                current_sys_state = ST_WAIT_HASH; set_display_dash();
                generate_and_print_otp(); start_otp_timer();
            } else {
                current_sys_state = ST_FAIL; set_display_fail();
            }
        }
    }
}
void sys_st_wait_hash(char key) {
    if (key == '#') { current_sys_state = ST_ENTER_OTP; buffer_pos = 0; set_display_dash(); }
}
void sys_st_enter_otp(char key) {
    if (key >= '0' && key <= '9' && buffer_pos < 6) {
        display_buffer[buffer_pos] = key - '0'; buffer_pos++;
        if (buffer_pos == 6) { 
            TCCR1B = 0; // Ferma il timer
            if (check_arrays_match((const unsigned char*)display_buffer, (const void*)generated_otp, 1)) {
                current_sys_state = ST_OPEN; set_display_open();
            } else {
                current_sys_state = ST_FAIL; set_display_fail();
            }
        }
    }
} 
void sys_st_fail(char key) { }
void sys_st_open(char key) { }

void (*const system_fsm_table[7])(char) = {
    sys_st_standby, sys_st_wait_star, sys_st_enter_pin, 
    sys_st_wait_hash, sys_st_enter_otp, sys_st_fail, sys_st_open
};

void process_valid_key(char key) {
    if (last_pressed_key == '*' && key == '#') {
        if (current_sys_state != ST_FAIL && current_sys_state != ST_OPEN) {
            current_sys_state = ST_STANDBY;
            set_display_blank();
            pir_triggered_flag = 0; 
            last_pressed_key = key; return;
        }
    }
    last_pressed_key = key;
    system_fsm_table[current_sys_state](key);
}

unsigned char get_raw_key_coord() {
    for (unsigned char r = 0; r < 4; r++) {
        PORTC |= 0x0F; PORTC &= ~(1 << r); 
        asm volatile("nop\n\tnop\n\tnop\n\tnop\n\t"); 
        
        if (!(PINC & (1 << 4))) return (r << 4) | 0; 
        if (!(PINC & (1 << 5))) return (r << 4) | 1; 
        if (!(PIND & (1 << 3))) return (r << 4) | 2; 
    }
    return 0xFF; 
}

void fsm_idle() { if (current_coord != 0xFF) { last_coord = current_coord; debounce_timer = 0; key_state = 1; } }
void fsm_verify_press() {
    debounce_timer++;
    if (debounce_timer > 15) {
        if (current_coord == last_coord) {
            unsigned char r = current_coord >> 4;
            process_valid_key(CHAR_MAP[r][current_coord & 0x0F]); key_state = 2; 
        } else key_state = 0; 
    }
}
void fsm_wait_release() { if (current_coord == 0xFF) { debounce_timer = 0; key_state = 3; } }
void fsm_verify_release() {
    debounce_timer++;
    if (debounce_timer > 15) { if (current_coord == 0xFF) key_state = 0; else key_state = 2; }
}

void (*const keypad_fsm_table[4])(void) = { 
  fsm_idle, fsm_verify_press, fsm_wait_release, fsm_verify_release 
};

// ==============================================================================
// 5. LOOP DISPATCHER E TIMEOUT
// ==============================================================================

void loop_task_standby() {
    if (pir_triggered_flag) {
        pir_triggered_flag = 0; 
    }
}

void loop_task_active_standard() {
    current_coord = get_raw_key_coord();
    keypad_fsm_table[key_state](); 
}

void loop_task_active_timer() {
    if (TIFR1 & (1 << OCF1A)) {
        TIFR1 = (1 << OCF1A); 
        otp_seconds_left--; 
        if (otp_seconds_left == 0) { 
            TCCR1B = 0; 
            current_sys_state = ST_FAIL;
            set_display_fail();
            return;
        }
    }
    loop_task_active_standard();
}

void (*const loop_task_table[7])(void) = {
    loop_task_standby, loop_task_active_standard, loop_task_active_standard, 
    loop_task_active_timer, loop_task_active_timer, loop_task_active_standard, loop_task_active_standard
};


// ==============================================================================
// 6. SETUP E MAIN
// ==============================================================================

void sys_setup() {
    DDRD |= 0x70; // Pin D4, D5, D6 Output
    DDRB |= 0x3F; // Pin B0-B5 Output
    DDRC |= 0x0F; PORTC |= 0x0F; // Righe tastiera
    
    // Pull-ups colonne tastiera
    DDRC &= ~0x30; PORTC |= 0x30; DDRD &= ~(1 << 3); PORTD |= (1 << 3); 
    // Pull-up PIR
    DDRD &= ~(1 << 2); PORTD |= (1 << 2);

    uart_init();

    // Timer2
    TCCR2A = (1 << WGM21); TCCR2B = (1 << CS22); OCR2A  = 249;  TIMSK2 = (1 << OCIE2A); 
  
    // Interruzione esterna
    EICRA = (1 << ISC01) | (1 << ISC00);  
    EIMSK = (1 << INT0);  
  
    // Global Interrupts
    SREG |= (1 << 7); 
}

int main(void) {
    sys_setup();
    
    while (1) {
        fast_entropy_counter++; 
        loop_task_table[current_sys_state](); 
    }
    
    return 0;
}
