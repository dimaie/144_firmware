#include "si5351.h"
#include "Wire.h"
#include "Encoder.h"
#include <TM1637Display.h>

#pragma GCC optimize ("O0")

enum RxTx {
  RX = 0, TX = 1
};

struct Band {
  unsigned long min_rf;
  unsigned long max_rf;
  unsigned long current_vfo; // VFO offset from IF
};

enum MenuDataType {
  INT_ARRAY,
  STRING_ARRAY,
  RANGE        
};

struct MenuConfig {
  MenuDataType type;
  int num_items;      // array length; ignored for RANGE
  int initial_value;  // The starting index or starting value
  const void* data_ptr;
  int divisor;
  int min_val;        // for RANGE mode
  int max_val;        // for RANGE mode
};

Band bands[] = {
  { 2800000000ULL, 2970000000ULL, 1913297000ULL }, 
  { 2100000000ULL, 2145000000ULL, 1213297000ULL }
};

typedef void(*button_handler)(void);

struct NamedHandler {
  const char* label;
  button_handler handler;
};

// Define connections to TM1637
const int soft_clk_pin      = 0; // P0.0
const int soft_dio_pin      = 8; // P0.5
const int channel_a_pin     = 9; // P1.4
const int channel_b_pin     = 1; // P0.1
const int enc_button_pin    = 2; // P0.3
const int ptt_pin           = 3; // P0.4
const int b28_pin           = 4; // P0.14
const int b21_pin           = 5; // P0.15
const int amp_pin           = 6; // P2.0

const unsigned int cw_shift = 50000;
const unsigned long intermediate_frequency = 886723000ULL;
const int16_t keyer_speed_factor = 400;
const int16_t tx_timeout = 700;
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

const char* disp_options[] = {
  "FLO ",
  "FHi "
};

const char* band_codes[] = {
  "28  ",
  "21  "
};

void tune();
void change_band();
void change_step();
void change_speed();
void change_display_mode();

NamedHandler menu[] = {
  { "tUnE", tune },
  { "bAnd", change_band },
  { "StEP", change_step },
  { "SPd ", change_speed },
  { "dISP", change_display_mode }
};

const int num_handlers = sizeof(menu) / sizeof(menu[0]);

int display_mode = 1; // 0 = Standard kHz, 1 = High Res (100Hz)
const char* mode_names[] = { "F_LO", "F_Hi" }; 

int extract_display_val(unsigned long freq) {
  if (display_mode == 0) {
    // Standard: 28.011.5 -> 2801
    return freq / 1000000;
  } else {
    // High Res: 28.011.5 -> 0115
    return (freq / 10000) % 10000;
  }
}

uint8_t encodeASCII(char c) {
  // Convert to uppercase for easier matching
  if (c >= 'a' && c <= 'z') c -= 32;

  switch (c) {
    case '0'...'9': return display.encodeDigit(c - '0');
    case 'A': return 0x77; // A
    case 'B': return 0x7C; // b
    case 'C': return 0x39; // C
    case 'D': return 0x5E; // d
    case 'E': return 0x79; // E
    case 'F': return 0x71; // F
    case 'G': return 0x3D; // G
    case 'H': return 0x76; // H
    case 'I': return 0x06; // I
    case 'L': return 0x38; // L
    case 'N': return 0x54; // n
    case 'O': return 0x3F; // O
    case 'P': return 0x73; // P
    case 'S': return 0x6D; // S
    case 'T': return 0x78; // t (lowercase style is clearer)
    case 'U': return 0x3E; // U
    case '-': return 0x40; // dash
    case ' ': return 0x00; // space
    default:  return 0x00;
  }
}

void showString(const char* s) {
  uint8_t data[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; i++) {
    if (s[i] == '\0') break;
    data[i] = encodeASCII(s[i]);
  }
  display.setSegments(data);
}


inline unsigned long get_max_frequency() {
  return bands[current_band_idx].max_rf - intermediate_frequency;
}

inline unsigned long get_min_frequency() {
  return bands[current_band_idx].min_rf - intermediate_frequency;
}

inline unsigned long& get_frequency() {
  return bands[current_band_idx].current_vfo;
}

int select_from_range(MenuConfig config) {
  int current_val = config.initial_value;
  int8_t click_accumulator = 0;
  const int8_t clicks_per_step = 4;
  bool confirmed = false;

  auto update_display = [&](int val) {
    if (config.type == INT_ARRAY) {
      const unsigned int* vals = (const unsigned int*)config.data_ptr;
      display.showNumberDec(vals[val] / config.divisor, false);
    } else if (config.type == STRING_ARRAY) {
      const char** vals = (const char**)config.data_ptr;
      showString(vals[val]);
    } else if (config.type == RANGE) {
      display.showNumberDec(val, false); // Direct display for speed
    }
  };

  update_display(current_val);
  while (encoder.is_button_pressed()) { delay(10); }
  delay(100);

  while (!confirmed) {
    int8_t clicks = encoder.get_clicks();
    if (clicks != 0) {
      click_accumulator += clicks;
      if (abs(click_accumulator) >= clicks_per_step) {
        int direction = (click_accumulator > 0) ? 1 : -1;
        current_val += direction;
        click_accumulator = 0;

        // Bounds Logic
        if (config.type == RANGE) {
          // Constrain to min/max without wrapping
          if (current_val < config.min_val) current_val = config.min_val;
          if (current_val > config.max_val) current_val = config.max_val;
        } else {
          // Wrap for arrays
          if (current_val >= config.num_items) current_val = 0;
          if (current_val < 0) current_val = config.num_items - 1;
        }
        update_display(current_val);
      }
    }
    if (encoder.is_button_pressed()) confirmed = true;
    delay(10);
  }

  while (encoder.is_button_pressed()) { delay(10); }
  return current_val;
}

void handle_encoder_button() {
  int8_t handler_index = 0;
  long start = millis();
  bool selection_started = false;

  while (encoder.is_button_pressed()) {
    long duration = millis() - start;

    if (duration > minimum_press_time) {
      if (!selection_started) selection_started = true;

      handler_index = (duration / handler_interval) % num_handlers;

      showString(menu[handler_index].label); 
    }
    delay(1000);
  }

  if (selection_started) {
    if (menu[handler_index].handler != nullptr) {
      menu[handler_index].handler();
    }
    
    // Restore freq display
    display.showNumberDec(extract_display_val(get_frequency() + intermediate_frequency), true);
    delay(50); 
  }
}

void change_display_mode() {
  MenuConfig config = {
    STRING_ARRAY, 
    2,                // Number of items
    display_mode,     // Initial index
    disp_options,     // Pointer to our strings
    1,                // No divisor needed
    0,
    1
  };

  display_mode = select_from_range(config);
  
  // Visual confirmation
  showString(disp_options[display_mode]);
  delay(500);
}

void change_step() {
  MenuConfig config = {
    INT_ARRAY, 
    4, 
    current_step_idx, 
    step_options, 
    100
  };

  current_step_idx = select_from_range(config);
  frequency_step = step_options[current_step_idx];

  // Visual flash confirmation
  for(int i=0; i<2; i++) {
    display.clear(); delay(100);
    display.showNumberDec(frequency_step / 100, false); delay(100);
  }
}

void change_band() {
  MenuConfig config = {
    STRING_ARRAY,     // type
    2,                // num_items (Size of band_codes)
    current_band_idx, // initial_value (The current selection)
    band_codes,       // data_ptr (The list of strings)
    1,                // divisor (unused for strings)
    0,                // min_val (unused)
    0                 // max_val (unused)
  };

  int new_idx = select_from_range(config);

  if (new_idx != current_band_idx) {
    current_band_idx = new_idx;
    // Hardware update
    si5351.set_freq(get_frequency(), SI5351_CLK0);
    old_frequency = get_frequency();

    uint8_t done_segs[] = {0x40, 0x40, 0x40, 0x40};
    display.setSegments(done_segs);
    delay(300);
  }
}

void change_speed() {
  MenuConfig config = {
    RANGE, 
    0,            // num_items not needed for RANGE
    keyer_speed,  // initial_value
    nullptr,      // data_ptr not needed
    1,            // divisor
    5,            // min_val (WPM)
    24            // max_val (WPM)
  };

  keyer_speed = select_from_range(config);

  // Brief flash to confirm
  for(int i=0; i<2; i++) {
    display.clear(); delay(80);
    display.showNumberDec(keyer_speed, false); delay(80);
  }
}

void tune() {
    uint8_t segments[4] = {0};
    segments[0] = 0x79;  
    display.setSegments(segments, 4, 0);
    digitalWrite(ptt_pin, HIGH);
    set_rx_tx(TX);
    bool pressed = false;
    while (!pressed) {
      for (int8_t i = 0; i < 5; ++i) {
        pressed = encoder.is_button_pressed();
        delay(20);
      }
    }
    set_rx_tx(RX);
    digitalWrite(ptt_pin, LOW);
    segments[0] = 0x40;  
    display.setSegments(segments, 4, 0);
    delay(500);
}

void setup() {
  bool i2c_found;

  encoder.init(channel_a_pin, channel_b_pin, enc_button_pin);
  encoder.enable(true);

  current_band_idx = 0;

  unsigned long& frequency = get_frequency();
  i2c_found = si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_8MA);
  si5351.set_freq(frequency, SI5351_CLK0);

  old_frequency = frequency;

  display.setBrightness(0x03);
  display.showNumberDec(extract_display_val(frequency + intermediate_frequency), true);

  pinMode(ptt_pin, OUTPUT);
  pinMode(b28_pin, OUTPUT);
  pinMode(b21_pin, OUTPUT);
  pinMode(amp_pin, OUTPUT);
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
      display.showNumberDec(extract_display_val(frequency + intermediate_frequency), true);
    }
  }

  handle_encoder_button();
  delay(1);
}
