
#include "si5351.h"
#include "Wire.h"
#include "Encoder.h"
#include <TM1637Display.h>

#pragma GCC optimize ("O0")
enum CWKeyUpDownState {
  UP = 0, DOWN = 1
};

enum CWKeyDitDahState {
  DIT = 1, DAH = 3
};

enum RxTx {
  RX = 0, TX = 1
};

struct Band {
  const char* name;
  unsigned long min_rf;
  unsigned long max_rf;
  unsigned long current_vfo; // VFO offset from IF
};


// intermediate_frequency = 886,723,000
// 28 MHz min_vfo = 2,800,000,000 - 886,723,000 = 1,913,277,000
// Initial frequency = min_vfo + 20,000 (20 kHz)

Band bands[] = {
  { "28", 2800000000ULL, 2970000000ULL, 1913297000ULL }, 
  { "21", 2100000000ULL, 2145000000ULL, 1213297000ULL }
};

// Define connections to TM1637
const int soft_clk_pin      = 0; // P0.0
const int soft_dio_pin      = 8; // P0.5
const int channel_a_pin     = 9; // P1.4
const int channel_b_pin     = 1; // P0.1
const int enc_button_pin    = 2; // P0.3
const int cw_ptt_pin        = 3; // P0.4
const int speaker_pin       = 4; // P0.14
const int dit_pin           = 5; // P0.15
const int dah_pin           = 6; // P2.0

const unsigned int cw_shift = 50000;
const unsigned long intermediate_frequency = 886723000ULL;
const int16_t keyer_speed_factor = 400;
const int16_t tx_timeout = 700;
typedef void(*button_handler)(void);
button_handler handlers[4];
const long handler_interval = 500;
const long minimum_press_time = 50;

int current_band_idx;
unsigned long old_frequency;
int16_t keyer_speed = 20;
RxTx rx_tx_state = RX;
unsigned int frequency_step = 10000; // Default to 100 Hz
// Options for frequency steps
const unsigned int step_options[] = {1000, 5000, 10000, 100000};
int current_step_idx = 2; // Index for 100 Hz

Si5351 si5351;
Encoder encoder;
TM1637Display display(soft_clk_pin, soft_dio_pin);

//#define DEBUG
inline unsigned long get_max_frequency() {
  return bands[current_band_idx].max_rf - intermediate_frequency;
}

inline unsigned long get_min_frequency() {
  return bands[current_band_idx].min_rf - intermediate_frequency;
}

inline unsigned long& get_frequency() {
  return bands[current_band_idx].current_vfo;
}

inline int extract_display_khz(unsigned long freq) {
  return (freq / 10000) % 10000;
}

void handle_encoder_button() {
  int8_t handler_index = 0;
  long start = millis();
  bool selection_started = false;
  size_t num_handlers = sizeof(handlers) / sizeof(handlers[0]);

  // 1. Enter the selection loop while button is held
  while (encoder.is_button_pressed()) {
    long duration = millis() - start;

    // Only provide visual feedback if we've passed the "Ghost Click" threshold
    if (duration > minimum_press_time) {
      if (!selection_started) {
        selection_started = true;
      }

      // Calculate which handler index we are currently over
      // (Using integer math to cycle every 'handler_interval')
      handler_index = (duration / handler_interval) % num_handlers;

      uint8_t segments[4] = {0};
      segments[handler_index] = 0x40; // Display the "-" dash
      display.setSegments(segments, 4, 0);
    }
    delay(10);
  }

  // 2. Execution Phase (only if it wasn't a ghost click)
  if (selection_started) {
    if (handlers[handler_index] != nullptr) {
      handlers[handler_index]();
    }
    
    // Restore frequency on the display after handler finishes
    display.showNumberDec(extract_display_khz(get_frequency() + intermediate_frequency), true);
    
    // Small debounce delay to prevent immediate re-triggering on button bounce
    delay(50); 
  }
}

void change_step() {
  bool confirmed = false;
  int temp_idx = current_step_idx;
  int8_t click_accumulator = 0;
  const int8_t clicks_per_step = 4; // Consistency with change_band stiffness

  // Show current step immediately (e.g., "100")
  display.showNumberDec(step_options[temp_idx] / 100, false);

  // Wait for button release from the menu trigger
  while (encoder.is_button_pressed()) { delay(10); }
  delay(100); 

  while (!confirmed) {
    int8_t clicks = encoder.get_clicks();
    
    if (clicks != 0) {
      click_accumulator += clicks;

      if (click_accumulator >= clicks_per_step) {
        temp_idx++;
        click_accumulator = 0;
      } 
      else if (click_accumulator <= -clicks_per_step) {
        temp_idx--;
        click_accumulator = 0;
      }

      // Wrap around the 4 options
      if (temp_idx >= 4) temp_idx = 0;
      if (temp_idx < 0) temp_idx = 3;

      // Update display with the step value
      display.showNumberDec(step_options[temp_idx] / 100, false);
    }

    if (encoder.is_button_pressed()) {
      confirmed = true;
    }
    delay(10);
  }

  // Set the new global frequency step
  current_step_idx = temp_idx;
  frequency_step = step_options[current_step_idx];

  // Visual confirmation (flashing the selected step)
  for(int i=0; i<2; i++) {
    display.clear();
    delay(100);
    display.showNumberDec(frequency_step / 100, false);
    delay(100);
  }
  
  while (encoder.is_button_pressed()) { delay(10); }
}

void change_band() {
  bool confirmed = false;
  int temp_idx = current_band_idx;
  int8_t click_accumulator = 0;
  const int8_t clicks_per_step = 4; // Adjust this for "stiffness" (4-8 is usually good)
  
  display.showNumberDec(atoi(bands[temp_idx].name), false);

  while (encoder.is_button_pressed()) { delay(10); }
  delay(100); 

  while (!confirmed) {
    int8_t clicks = encoder.get_clicks();
    
    if (clicks != 0) {
      click_accumulator += clicks;

      // Check if we have accumulated enough clicks to move UP
      if (click_accumulator >= clicks_per_step) {
        temp_idx++;
        click_accumulator = 0; // Reset
      } 
      // Check if we have accumulated enough clicks to move DOWN
      else if (click_accumulator <= -clicks_per_step) {
        temp_idx--;
        click_accumulator = 0; // Reset
      }

      // Handle array wrapping
      int num_bands = sizeof(bands) / sizeof(bands[0]);
      if (temp_idx >= num_bands) temp_idx = 0;
      if (temp_idx < 0) temp_idx = num_bands - 1;

      // Update display only when the index actually changes
      display.showNumberDec(atoi(bands[temp_idx].name), false);
    }

    if (encoder.is_button_pressed()) {
      confirmed = true;
    }
    delay(10);
  }

  // Final confirmation and hardware update
  if (temp_idx != current_band_idx) {
    current_band_idx = temp_idx;
    si5351.set_freq(get_frequency(), SI5351_CLK0);
    old_frequency = get_frequency();

    uint8_t done_segs[] = {0x40, 0x40, 0x40, 0x40};
    display.setSegments(done_segs);
    delay(300);
  }
  
  while (encoder.is_button_pressed()) { delay(10); }
}

void tune() {
    uint8_t segments[4] = {0};
    segments[0] = 0x79;  
    display.setSegments(segments, 4, 0);
    digitalWrite(cw_ptt_pin, HIGH);
    set_rx_tx(TX);
    bool pressed = false;
    while (!pressed) {
      for (int8_t i = 0; i < 5; ++i) {
        pressed = encoder.is_button_pressed();
        delay(20);
      }
    }
    set_rx_tx(RX);
    digitalWrite(cw_ptt_pin, LOW);
    segments[0] = 0x40;  
    display.setSegments(segments, 4, 0);
    delay(500);
}

void setup() {
  bool i2c_found;

  encoder.init(channel_a_pin, channel_b_pin, enc_button_pin);
  encoder.enable(true);

  for (int8_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
      handlers[i] = nullptr;
  }

  handlers[0] = tune;
  handlers[1] = change_band;
  handlers[2] = change_step;

  current_band_idx = 0;

  unsigned long& frequency = get_frequency();
  i2c_found = si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_8MA);
  si5351.set_freq(frequency, SI5351_CLK0);

  old_frequency = frequency;

  display.setBrightness(0x03);
  display.showNumberDec(extract_display_khz(frequency + intermediate_frequency), true);

  pinMode(cw_ptt_pin, OUTPUT);
  pinMode(speaker_pin, OUTPUT);
  pinMode(dit_pin, INPUT_PULLUP);
  pinMode(dah_pin, INPUT_PULLUP);
}

void set_rx_tx(RxTx _rx_tx_state) {
  if (get_rx_tx() == _rx_tx_state) {
    return;
  }
  rx_tx_state = _rx_tx_state;
  if (rx_tx_state == RX) {
    encoder.enable(true);
  } else {
    encoder.enable(false);
  }
}

RxTx get_rx_tx() {
  return rx_tx_state;
}

bool is_key_pressed(uint8_t key, uint8_t line_state) {
  for (uint8_t k = 0; k < 5; ++k) {
    if (digitalRead(key) != line_state) {
      return false;
    }
    delay(1);
  }
  return true;
}

bool _tone;
void press_cw_key(CWKeyUpDownState key_state) {
  if (key_state == DOWN) {
    if (get_rx_tx() == RX) {
      set_rx_tx(TX);
    }
    _tone = true;
    digitalWrite(cw_ptt_pin, HIGH);
  } else if (key_state == UP) {
    digitalWrite(cw_ptt_pin, LOW);
    _tone = false;
  }
}

void tone_delay(int16_t delay_calc) {
  int start = millis();
  while(millis() - start < delay_calc) {
    digitalWrite(speaker_pin, _tone ? HIGH : LOW);
    delayMicroseconds(250);
    digitalWrite(speaker_pin, LOW);
    delayMicroseconds(250);
  }
}

long _time;
void handle_key(CWKeyDitDahState key_state, bool active) {
  if (active) {
    press_cw_key(DOWN);
  } 
  int16_t single_delay = keyer_speed_factor / keyer_speed;
  int16_t delay_calc = single_delay * static_cast<int>(key_state);
  tone_delay(delay_calc);
  if (active) {
    press_cw_key(UP);
    // reset timer
    _time = millis();
  }
}

void keyer() {
  // set time variable with the value 0,
  // so that the diff calculated at
  // the end of the function will be always
  // bigger than the timeout value
  _time = 0;
  long timeout = millis();
  bool finished = true;
  bool keying_complete = true;
  do {
    if (is_key_pressed(dit_pin, LOW)) {
      handle_key(DIT, true);
      finished = false;
    } else if (is_key_pressed(dah_pin, LOW)) {
      handle_key(DAH, true);
      finished = false;
    } else {
      finished = true;
    }
    // pause between elements
    if (!finished) {
      handle_key(DIT, false);
    }  
    timeout = millis() - _time;
    keying_complete = finished && (timeout >= tx_timeout);
  } while (!keying_complete);
  // switch to rx
  if (get_rx_tx() == TX) {
    set_rx_tx(RX);
  }
}

void loop() {
  int8_t clicks = encoder.get_clicks();
  int32_t frequency_update = frequency_step * clicks;
  
  if (frequency_update) {
    // We get a reference to the actual frequency in the struct
    unsigned long& frequency = get_frequency(); 
    
    frequency += frequency_update;

    // Bounds checking
    if (frequency < get_min_frequency()) frequency = get_min_frequency();
    if (frequency > get_max_frequency()) frequency = get_max_frequency();

    if (old_frequency != frequency) {
      si5351.set_freq(frequency, SI5351_CLK0);
      old_frequency = frequency;
      display.showNumberDec(extract_display_khz(frequency + intermediate_frequency), true);
    }
  }

  keyer();
  handle_encoder_button();
  delay(1);
}
