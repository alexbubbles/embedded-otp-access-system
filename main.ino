
// 1. DEFINIZIONE COSTANTI E STATI DEGLI AUTOMI (LIVELLO MV0)

#define KEY_STAR        10
#define KEY_HASH        11
#define KEY_NONE        0xFF

#define SEG_DASH        10
#define SEG_BLANK       11

#define DEBOUNCE_MS     16     // 16 ms di stabilizzazione antirimbalzo

#define OTP_TIMEOUT_S   10     // 10 secondi di validita OTP

typedef enum {
    ST_STANDBY   = 0,
    ST_WAIT_STAR = 1,
    ST_ENTER_PIN = 2,
    ST_WAIT_HASH = 3,
    ST_ENTER_OTP = 4,
    ST_FAIL      = 5,
    ST_OPEN      = 6
} SysState;

typedef enum {
    KP_IDLE      = 0,
    KP_VER_PRESS = 1,
    KP_WAIT_REL  = 2,
    KP_VER_REL   = 3
} KpState;

// 2. STRUTTURE DATI GLOBALI E REGISTRI CONDIVISI TRA AUTOMI

typedef struct {
    uint8_t tick;      // Flag cadenza 1 ms per scansione foreground (1 bit)
    uint8_t timer_en;  // Abilitazione countdown OTP (1 bit)
    uint8_t expired;   // Flag scadenza 10 secondi (1 bit)
    uint8_t sec_left;  // Secondi residui (4 bit effettivi: 0..10)
} Clock;

typedef struct {
    uint8_t buf[6];    // Vettore visivo (6 x 4 bit: 0..11)
    uint8_t digit;     // Indice cifra attiva multiplexing (3 bit: 0..5)
} Display;

typedef struct {
    KpState state;     // Stato FSM antirimbalzo (2 bit)
    uint8_t raw;       // Coordinata grezza 3*r + c (4 bit)
    uint8_t stable;    // Coordinata in verifica (4 bit)
    uint8_t ms_cnt;    // Contatore campioni stabili (5 bit: 0..16)
    uint8_t key;       // Tasto validato a 4 bit (0..11, 0xFF=nullo)
} Keypad;

typedef struct {
    SysState state;    // Stato FSM centrale (3 bit)
    uint8_t  pir_flag; // Flag fronte di salita sensore PIR (1 bit)
    uint8_t  idx;      // Indice cifra in inserimento (3 bit: 0..6)
    uint8_t  code_ok;  // Accumulatore booleano verifica in serie (1 bit)
    uint8_t  last_key; // Memoria tasto precedente per reset *# (4 bit)
    uint8_t  otp[6];   // Cifre OTP in binario puro (6 x 4 bit: 0..9)
    uint16_t seed;     // Contatore entropia a 16 bit per Xorshift
} System;

volatile static Clock   clk  = {0, 0, 0, 0};
volatile static Display disp = {{SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK}, 0};
volatile static Keypad  kp   = {KP_IDLE, KEY_NONE, KEY_NONE, 0, KEY_NONE};
volatile static System  sys  = {ST_STANDBY, 0, 0, 1, KEY_NONE, {0, 0, 0, 0, 0, 0}, 0xACE1};

// Costanti cablate e tabelle di decodifica
static const uint8_t SECRET_PIN[6] = {1, 2, 3, 4, 5, 6};

static const uint8_t SEG_MAP[12] = {
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
    0b01000000, // 10: -
    0b00000000  // 11: spento
};

static const uint8_t KEY_MAP[12] = {
    1, 2, 3,
    4, 5, 6,
    7, 8, 9,
    KEY_STAR, 0, KEY_HASH
};
 
// 3. PRIMITIVE HARDWARE 

static inline void setDisp(uint8_t val) {
    for (uint8_t i = 0; i < 6; i++) {
        disp.buf[i] = val;
    }
}

// Invio seriale e impulso di Latch 
static inline void srSend(uint8_t val) {
    for (uint8_t i = 0; i < 8; i++) {
        if ((val & 0x80) != 0) {
            PORTD |= (1 << 4);  // D4 (DS) HIGH (Istruzione SBI)
        } else {
            PORTD &= ~(1 << 4); // D4 (DS) LOW  (Istruzione CBI)
        }
        PORTD |= (1 << 5);      // D5 (SHCP Clock) HIGH
        PORTD &= ~(1 << 5);     // D5 (SHCP Clock) LOW
        val <<= 1;              // Scorri al bit successivo
    }
    PORTD |= (1 << 6);          // D6 (STCP Latch) HIGH
    PORTD &= ~(1 << 6);         // D6 (STCP Latch) LOW
}

static inline void uartTx(uint8_t b) {
    while ((UCSR0A & (1 << UDRE0)) == 0);
    UDR0 = b;
}

static inline void makeOtp(void) {
    uint16_t s = sys.seed;
    if (s == 0) {
        s = 0xACE1;
    }
    for (uint8_t i = 0; i < 6; i++) {
        s ^= (s << 7);
        s ^= (s >> 9);
        s ^= (s << 8);
        uint8_t d = (uint8_t)(s & 0x0F);
        if (d > 9) {
            d -= 6;
        }
        sys.otp[i] = d;
        uartTx(d + 48); // Conversione ASCII ('0' = 48)
    }
    uartTx(13); // CR
    uartTx(10); // LF
}

static inline uint8_t scanKeys(void) {
    for (uint8_t r = 0; r < 4; r++) {
        PORTC |= 0x0F;                  // Righe PC0..PC3 HIGH
        PORTC &= ~(1 << r);             // Riga r-esima LOW
        asm volatile("nop\n\tnop\n\t"); // Assestamento linee (125 ns)

        if ((PINC & (1 << PINC4)) == 0) return (uint8_t)(r * 3 + 0);
        if ((PINC & (1 << PINC5)) == 0) return (uint8_t)(r * 3 + 1);
        if ((PIND & (1 << PIND3)) == 0) return (uint8_t)(r * 3 + 2);
    }
    PORTC |= 0x0F;
    return KEY_NONE;
}

// 4. INIZIALIZZAZIONE HARDWARE PERIFERICHE 

static inline void initIO(void) {
    DDRD |= (1 << DDD4) | (1 << DDD5) | (1 << DDD6); // D4, D5, D6 Output (74HC595)
    DDRB |= 0x3F;                                    // PB0..PB5 Output (Anodi)

    DDRC  |= 0x0F;                                   // Righe PC0..PC3 Output
    PORTC |= 0x0F;

    DDRC  &= ~((1 << DDC4) | (1 << DDC5));           // Colonne PC4, PC5 Input Pull-Up
    PORTC |= (1 << PORTC4) | (1 << PORTC5);
    DDRD  &= ~((1 << DDD3) | (1 << DDD2));           // Colonna PD3 e PIR PD2 Input Pull-Up
    PORTD |= (1 << PORTD3) | (1 << PORTD2);
}

static inline void initUART(void) {
    UBRR0H = 0;
    UBRR0L = 103; // 9600 baud @ 16 MHz
    UCSR0B = (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
}

static inline void initBaseTempi(void) {
    TCCR2A = (1 << WGM21);  // Modalita CTC
    TCCR2B = (1 << CS22);   // Prescaler 64
    OCR2A  = 249;           // Periodo 1 ms (1000 Hz)
    TIMSK2 = (1 << OCIE2A); // Abilita interrupt Compare Match A
}

static inline void initPIR(void) {
    EICRA = (1 << ISC01) | (1 << ISC00); // Fronte di salita su INT0 (PD2)
    EIMSK = (1 << INT0);
}

// 5. GESTORI DI INTERRUZIONE (ISR BACKGROUND: PIR, CLOCK E MULTIPLEXING)

ISR(INT0_vect) {
    sys.pir_flag = 1;
}

ISR(TIMER2_COMPA_vect) {
    static uint16_t ms_acc = 0; // Accumulatore locale per base tempi 1 secondo

    // 1. Segnale di sincronismo a 1 ms per gli automi in foreground
    clk.tick = 1;

    // 2. Prescaler a 1 secondo e countdown OTP (a 8 bit atomico)
    if (clk.timer_en != 0) {
        ms_acc++;
        if (ms_acc >= 1000) {
            ms_acc = 0;
            if (clk.sec_left > 0) {
                clk.sec_left--;
                if (clk.sec_left == 0) {
                    clk.timer_en = 0;
                    clk.expired  = 1;
                }
            }
        }
    } else {
        ms_acc = 0; // Mantiene allineata la fase del primo secondo
    }

    // 3. Multiplexing Display a 7 Segmenti (1 cifra ogni 1 ms)
    PORTB &= ~0x3F; // Spegne tutti gli anodi PB0..PB5
    if (sys.state == ST_STANDBY) {
        return;
    }

    uint8_t d = disp.digit;
    uint8_t mask = (uint8_t)(~SEG_MAP[disp.buf[d]]);
    srSend(mask);
    PORTB |= (1 << d); // Alimenta l'anodo della cifra corrente

    d++;
    if (d >= 6) {
        d = 0;
    }
    disp.digit = d;
}

// 6. AUTOMA TASTIERA IN FOREGROUND (FSM ANTIRIMBALZO A 4 STATI)

static inline void runKeypad(void) {
    uint8_t c = scanKeys();
    kp.raw = c;

    switch (kp.state) {
        case KP_IDLE:
            if (c != KEY_NONE) {
                kp.stable = c;
                kp.ms_cnt = 0;
                kp.state  = KP_VER_PRESS;
            }
            break;

        case KP_VER_PRESS:
            if (c == kp.stable) {
                kp.ms_cnt++;
                if (kp.ms_cnt >= DEBOUNCE_MS) {
                    kp.key   = KEY_MAP[c];
                    kp.state = KP_WAIT_REL;
                }
            } else {
                kp.state = KP_IDLE;
            }
            break;

        case KP_WAIT_REL:
            if (c == KEY_NONE) {
                kp.ms_cnt = 0;
                kp.state  = KP_VER_REL;
            }
            break;

        case KP_VER_REL:
            if (c == KEY_NONE) {
                kp.ms_cnt++;
                if (kp.ms_cnt >= DEBOUNCE_MS) {
                    kp.state = KP_IDLE;
                }
            } else {
                kp.state = KP_WAIT_REL;
            }
            break;
    }
}

// 7. AUTOMA CONTROLLO ACCESSI 

static inline void runSystem(void) {
    // 1. Guardia prioritaria: Scadenza timeout 10s
    if (clk.expired != 0) {
        clk.expired = 0;
        if (sys.state == ST_WAIT_HASH || sys.state == ST_ENTER_OTP) {
            setDisp(0);
            sys.state = ST_FAIL;
            return;
        }
    }

    // 2. Guardia di risveglio: Stato Standby ed evento PIR
    if (sys.state == ST_STANDBY) {
        if (sys.pir_flag != 0) {
            sys.pir_flag = 0;
            sys.last_key = KEY_NONE;
            setDisp(SEG_DASH);
            sys.state    = ST_WAIT_STAR;
        }
        kp.key = KEY_NONE;
        return;
    }
    sys.pir_flag = 0;

    // 3. Prelievo tasto validato dall'Automa Tastiera
    uint8_t k = kp.key;
    if (k == KEY_NONE) return;
    kp.key = KEY_NONE;

    // 4. Riconoscitore sequenza prioritaria di reset "*#"
    if (sys.state != ST_FAIL && sys.state != ST_OPEN) {
        if (sys.last_key == KEY_STAR && k == KEY_HASH) {
            clk.timer_en = 0; // Scrittura atomica a 8 bit
            clk.expired  = 0;
            sys.last_key = KEY_NONE;
            setDisp(SEG_BLANK);
            sys.state    = ST_STANDBY;
            return;
        }
        sys.last_key = k;
    }

    // 5. Evoluzione Macchina a Stati Finiti Centrale
    switch (sys.state) {
        case ST_WAIT_STAR:
            if (k == KEY_STAR) {
                sys.idx     = 0;
                sys.code_ok = 1;
                setDisp(SEG_DASH);
                sys.state   = ST_ENTER_PIN;
            }
            break;

        case ST_ENTER_PIN:
            if (k <= 9) {
                uint8_t i = sys.idx;
                disp.buf[i] = k;
                if (k != SECRET_PIN[i]) {
                    sys.code_ok = 0;
                }
                i++;
                sys.idx = i;

                if (i == 6) {
                    if (sys.code_ok != 0) {
                        setDisp(SEG_DASH);
                        makeOtp();
                        // Avvio atomico a 8 bit del timer da 10 secondi
                        clk.sec_left = OTP_TIMEOUT_S;
                        clk.expired  = 0;
                        clk.timer_en = 1;
                        sys.state    = ST_WAIT_HASH;
                    } else {
                        setDisp(0);
                        sys.state = ST_FAIL;
                    }
                }
            }
            break;

        case ST_WAIT_HASH:
            if (k == KEY_HASH) {
                sys.idx     = 0;
                sys.code_ok = 1;
                setDisp(SEG_DASH);
                sys.state   = ST_ENTER_OTP;
            }
            break;

        case ST_ENTER_OTP:
            if (k <= 9) {
                uint8_t i = sys.idx;
                disp.buf[i] = k;
                if (k != sys.otp[i]) {
                    sys.code_ok = 0;
                }
                i++;
                sys.idx = i;

                if (i == 6) {
                    clk.timer_en = 0; // Arresto atomico a 8 bit

                    if (sys.code_ok != 0) {
                        setDisp(1);
                        sys.state = ST_OPEN;
                    } else {
                        setDisp(0);
                        sys.state = ST_FAIL;
                    }
                }
            }
            break;

        default:
            break;
    }
}

// 8. FUNZIONE MAIN 

int main(void) {
    initIO();
    initUART();
    initBaseTempi();
    initPIR();

    // Abilitazione globale delle interruzioni (Bit 7 del registro SREG)
    SREG |= (1 << 7);

    while (1) {
        // Incremento incondizionato del contatore di entropia a 16 bit
        sys.seed++;

        // Scansione deterministica degli automi cadenzata dal tick di 1 ms
        if (clk.tick != 0) {
            clk.tick = 0;
            runKeypad();
            runSystem();
        }
    }
    return 0;
}
