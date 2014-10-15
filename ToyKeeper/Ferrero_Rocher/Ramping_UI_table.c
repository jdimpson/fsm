/*
 * NANJG 105C Diagram
 *            ---
 *          -|   |- VCC
 *   Star 4 -|   |- Voltage ADC
 *  Red LED -|   |- PWM
 *      GND -|   |- Green LED
 *            ---
 */

#define F_CPU 4800000UL

// PWM Mode
#define PHASE 0b00000001
#define FAST  0b00000011

/*
 * =========================================================================
 * Settings to modify per driver
 */

#define VOLTAGE_MON     // Comment out to disable - ramp down and eventual shutoff when battery is low
// Must be low to high
#define MODES           0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,5,5,6,6,7,7,8,9,10,11,12,13,14,16,17,19,21,23,25,27,30,33,36,39,43,47,52,56,62,68,74,81,89,97,106,116,127,139,153,167,183,200,219,239,255
// counter at which the PWM will loop (variable frequency PWM)
#define CEILINGS        255,255,228,203,177,147,116,81,52,248,225,199,171,140,237,212,185,241,217,253,229,253,230,247,222,234,242,246,247,247,244,240,251,244,249,252,252,251,247,251,253,252,249,251,251,254,250,253,254,252,253,254,253,252,252,253,253,254,254,254,254,254,254,255
//#define TURBO           // Comment out to disable - full output with a step down after n number of seconds
                        // If turbo is enabled, it will be where 255 is listed in the modes above
#define TURBO_TIMEOUT   5625 // How many WTD ticks before before dropping down (.016 sec each)
                        // 90  = 5625
                        // 120 = 7500

#define ADC_42          185 // the ADC value we expect for 4.20 volts
#define VOLTAGE_FULL    169 // 3.9 V, 4 blinks
#define VOLTAGE_GREEN   154 // 3.6 V, 3 blinks
#define VOLTAGE_YELLOW  139 // 3.3 V, 2 blinks
#define VOLTAGE_RED     124 // 3.0 V, 1 blink
#define ADC_LOW         123 // When do we start ramping down
#define ADC_CRIT        113 // When do we shut the light off
#define ADC_DELAY       188 // Delay in ticks between low-bat rampdowns (188 ~= 3s)
#define OWN_DELAY       // replace default _delay_ms() with ours
#define BLINK_ON_POWER  // blink once when power is received
// Switch handling
#define RAMP_TIMEOUT    120 // un-reverse ramp dir about 2s after button release
//#define LONG_PRESS_DUR   21 // How many WDT ticks until we consider a press a long press
#define LONG_PRESS_DUR   4 // How many WDT ticks until we consider a press a long press
                            // 32 is roughly .5 s, 21 is roughly 1/3rd second


/*
 * =========================================================================
 */

#ifdef OWN_DELAY
#include <util/delay_basic.h>
// Having own _delay_ms() saves some bytes AND adds possibility to use variables as input
static void _delay_ms(uint16_t n)
{
    while(n-- > 0)
        _delay_loop_2(950);
}
#else
#include <util/delay.h>
#endif

#include <avr/pgmspace.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
//#include <avr/power.h>

#define SWITCH_PIN  PB3     // what pin the switch is connected to, which is Star 4
#define PWM_PIN     PB1
#define VOLTAGE_PIN PB2
#define RED_PIN     PB4     // pin 3
#define GREEN_PIN   PB0     // pin 5
#define ADC_CHANNEL 0x01    // MUX 01 corresponds with PB2
#define ADC_DIDR    ADC1D   // Digital input disable bit corresponding with PB2
#define ADC_PRSCL   0x06    // clk/64

#define PWM_LVL     OCR0B   // OCR0B is the output compare register for PB1
#define CEIL_LVL    OCR0A   // OCR0A is the number of "frames" per PWM loop

#define DB_REL_DUR  0b00001111 // time before we consider the switch released after
                               // each bit of 1 from the right equals 16ms, so 0x0f = 64ms

/*
 * The actual program
 * =========================================================================
 */

/*
 * global variables
 */
PROGMEM const uint8_t modes[]     = { MODES };
PROGMEM const uint8_t ceilings[]  = { CEILINGS };
volatile int8_t mode_idx = 0;
volatile int8_t ramp_dir = 1;
uint8_t press_duration  = 0;
uint8_t voltages[] = { VOLTAGE_FULL, VOLTAGE_FULL, VOLTAGE_FULL, VOLTAGE_FULL };

// Debounced switch press value
int is_pressed()
{
    // Keep track of last switch values polled
    static uint8_t buffer = 0x00;
    // Shift over and tack on the latest value, 0 being low for pressed, 1 for pulled-up for released
    buffer = (buffer << 1) | ((PINB & (1 << SWITCH_PIN)) == 0);
    return (buffer & DB_REL_DUR);
}

void ramp() {
    mode_idx += ramp_dir;
    if (mode_idx <= 0) {
        mode_idx = 1;
        //ramp_dir = 1;
    }
    if (mode_idx > sizeof(modes)-1) {
        mode_idx = sizeof(modes) - 1;
        //ramp_dir = -1;
    }
}

void reverse() {
    ramp_dir = -ramp_dir;
    if (mode_idx <= 1) { ramp_dir = 1; }
    if (mode_idx >= sizeof(modes)-1) { ramp_dir = -1; }
}

inline void prev_mode() {
    // only used for turbo step-down and low-voltage step-down...
    // ... so go back quite a bit and don't turn entirely off
    mode_idx -= sizeof(modes) / 8;
    if (mode_idx <= 0) { mode_idx = 1; }
}

inline void PCINT_on() {
    // Enable pin change interrupts
    GIMSK |= (1 << PCIE);
}

inline void PCINT_off() {
    // Disable pin change interrupts
    GIMSK &= ~(1 << PCIE);
}

// Need an interrupt for when pin change is enabled to ONLY wake us from sleep.
// All logic of what to do when we wake up will be handled in the main loop.
EMPTY_INTERRUPT(PCINT0_vect);

inline void WDT_on() {
    // Setup watchdog timer to only interrupt, not reset, every 16ms.
    cli();                          // Disable interrupts
    wdt_reset();                    // Reset the WDT
    WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
    WDTCR = (1<<WDTIE);             // Enable interrupt every 16ms
    sei();                          // Enable interrupts
}

inline void WDT_off()
{
    cli();                          // Disable interrupts
    wdt_reset();                    // Reset the WDT
    MCUSR &= ~(1<<WDRF);            // Clear Watchdog reset flag
    WDTCR |= (1<<WDCE) | (1<<WDE);  // Start timed sequence
    WDTCR = 0x00;                   // Disable WDT
    sei();                          // Enable interrupts
}

inline void ADC_on() {
    ADMUX  = (1 << REFS0) | (1 << ADLAR) | ADC_CHANNEL; // 1.1v reference, left-adjust, ADC1/PB2
    DIDR0 |= (1 << ADC_DIDR);                           // disable digital input on ADC pin to reduce power consumption
    ADCSRA = (1 << ADEN ) | (1 << ADSC ) | ADC_PRSCL;   // enable, start, prescale
}

inline void ADC_off() {
    ADCSRA &= ~(1<<7); //ADC off
}

void sleep_until_switch_press()
{
    // This routine takes up a lot of program memory :(
    // Turn the WDT off so it doesn't wake us from sleep
    // Will also ensure interrupts are on or we will never wake up
    WDT_off();
    // Need to reset press duration since a button release wasn't recorded
    press_duration = 0;
    // Enable a pin change interrupt to wake us up
    // However, we have to make sure the switch is released otherwise we will wake when the user releases the switch
    while (is_pressed()) {
        _delay_ms(16);
    }
    PCINT_on();
    // turn red+green LEDs off
    DDRB = (1 << PWM_PIN); // note the lack of red/green pins here
    // with this commented out, the LEDs dim instead of turning off entirely
    //PORTB &= 0xff ^ ((1 << RED_PIN) | (1 << GREEN_PIN));  // red+green off
    // Enable sleep mode set to Power Down that will be triggered by the sleep_mode() command.
    //set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    // Now go to sleep
    sleep_mode();
    // Hey, someone must have pressed the switch!!
    // Disable pin change interrupt because it's only used to wake us up
    PCINT_off();
    // Turn the WDT back on to check for switch presses
    WDT_on();
    // Go back to main program
}

// The watchdog timer is called every 16ms
ISR(WDT_vect) {

#ifdef TURBO
    static uint16_t turbo_ticks = 0;
#endif
    static uint8_t  ontime_ticks = 0;
    static uint8_t  lowbatt_cnt = 0;
    static int8_t   saved_mode_idx = 0;
    uint16_t        voltage = 0;
    uint8_t         i = 0;

    if (mode_idx == 0) {
        ontime_ticks = 0;
    }

    if (is_pressed()) {
        if (press_duration < 255) {
            press_duration++;
        }

        if (press_duration == ((LONG_PRESS_DUR*8)-1)) {
            // Long press
            ramp();
        }
        // let the user keep holding the button to keep cycling through modes
        if (press_duration == LONG_PRESS_DUR*8) {
            press_duration = LONG_PRESS_DUR*7;
        }
#ifdef TURBO
        // Just always reset turbo timer whenever the button is pressed
        turbo_ticks = 0;
#endif
        // Same with the ramp down delay
        lowbatt_cnt = 0;
    } else {
        if (mode_idx != 0) {
            if (ontime_ticks < 255) {
                ontime_ticks ++;
            }
        }
        // Not pressed
        if (press_duration > 0 && press_duration < (LONG_PRESS_DUR*7)) {
            // Short press
            //prev_mode();
            if ( mode_idx == 0 ) {
                mode_idx = saved_mode_idx;
            } else {
                saved_mode_idx = mode_idx;
                mode_idx = 0;
            }
        } else if ( press_duration > 0 ) { // long press was just released
            reverse(); // reverse for the first couple seconds
        } else {
            if (ontime_ticks == RAMP_TIMEOUT) {
                reverse(); // un-reverse after a couple seconds
            }
#ifdef TURBO
            // Only do turbo check when switch isn't pressed
            if (modes[mode_idx] == 255) {
                turbo_ticks++;
                if (turbo_ticks > TURBO_TIMEOUT) {
                    // Go to the previous mode
                    prev_mode();
                }
            }
#endif
            // Only do voltage monitoring when the switch isn't pressed
#ifdef VOLTAGE_MON
            // See if conversion is done
            if (ADCSRA & (1 << ADIF)) {
                // Get an average of the past few readings
                for (i=0;i<3;i++) {
                    voltages[i] = voltages[i+1];
                }
                voltages[3] = ADCH;
                voltage = (voltages[0]+voltages[1]+voltages[2]+voltages[3]) >> 2;
                // See if voltage is lower than what we were looking for
                if (voltage < ((mode_idx == 1) ? ADC_CRIT : ADC_LOW)) {
                    ++lowbatt_cnt;
                } else {
                    lowbatt_cnt = 0;
                }
                if (voltage > VOLTAGE_GREEN) {
                    // turn on green LED
                    DDRB = (1 << PWM_PIN) | (1 << GREEN_PIN);
                    PORTB |= (1 << GREEN_PIN);
                    PORTB &= 0xff ^ (1 << RED_PIN);  // red off
                }
                else if (voltage > VOLTAGE_YELLOW) {
                    // turn on red+green LED (yellow)
                    DDRB = (1 << PWM_PIN) | (1 << GREEN_PIN) | (1 << RED_PIN); // bright green + bright red
                    //DDRB = (1 << PWM_PIN) | (1 << GREEN_PIN); // bright green + dim red
                    PORTB |= (1 << GREEN_PIN) | (1 << RED_PIN);
                }
                else {
                    // turn on red LED
                    DDRB = (1 << PWM_PIN) | (1 << RED_PIN);
                    PORTB |= (1 << RED_PIN);
                    PORTB &= 0xff ^ (1 << GREEN_PIN);  // green off
                }
            }

            // See if it's been low for a while, and maybe step down
            if (lowbatt_cnt >= ADC_DELAY) {
                prev_mode();
                lowbatt_cnt = 0;
            }

            // Make sure conversion is running for next time through
            ADCSRA |= (1 << ADSC);
        #endif
        }
        press_duration = 0;
    }
}

int main(void)
{
    // Set all ports to input, and turn pull-up resistors on for the inputs we are using
    DDRB = 0x00;
    PORTB = (1 << SWITCH_PIN);

    // Set the switch as an interrupt for when we turn pin change interrupts on
    PCMSK = (1 << SWITCH_PIN);

    // Set PWM pin to output
    DDRB = (1 << PWM_PIN);

    // Set timer to do PWM for correct output pin and set prescaler timing
    //TCCR0A = 0x23; // phase corrected PWM is 0x21 for PB1, fast-PWM is 0x23
    //TCCR0A = 0x25; // phase corrected PWM is 0x21 for PB1, fast-PWM is 0x23
    TCCR0A = 0x21; // trying to set variable-speed PWM
    //TCCR0B = 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)
    TCCR0B = 0x08 | 0x01; // pre-scaler for timer (1 => 1, 2 => 8, 3 => 64...)

    // Turn features on or off as needed
    #ifdef VOLTAGE_MON
    ADC_on();
    #else
    ADC_off();
    #endif
    ACSR   |=  (1<<7); //AC off

    #ifdef BLINK_ON_POWER
    // blink once to let the user know we have power
    //TCCR0A = PHASE | 0b00100000;  // Only use the normal output
    CEIL_LVL = 255;
    PWM_LVL = 255;
    _delay_ms(3);
    PWM_LVL = 0;
    _delay_ms(1);
    #endif

    // Enable sleep mode set to Power Down that will be triggered by the sleep_mode() command.
    set_sleep_mode(SLEEP_MODE_PWR_DOWN);
    sleep_until_switch_press();

    uint8_t last_mode_idx = 0;

    while(1) {
        // We will never leave this loop.  The WDT will interrupt to check for switch presses and 
        // will change the mode if needed.  If this loop detects that the mode has changed, run the
        // logic for that mode while continuing to check for a mode change.
        if (mode_idx != last_mode_idx) {
            // The WDT changed the mode.
            PWM_LVL     = pgm_read_byte(modes + mode_idx);
            CEIL_LVL    = pgm_read_byte(ceilings + mode_idx);
            last_mode_idx = mode_idx;
            _delay_ms(1);
            if (mode_idx == 0) {
                // Finish executing instructions for PWM level change
                // and/or voltage readout mode before shutdown.
                do {
                    _delay_ms(1);
                } while (0);
                // Go to sleep
                sleep_until_switch_press();
            }
        }
    }

    return 0; // Standard Return Code
}
