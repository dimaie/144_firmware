
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
const unsigned int frequency_step = 10000;
const unsigned long transverter_crystal_error = 240000ULL;
const unsigned long intermediate_frequency = 806400000ULL - transverter_crystal_error;
const unsigned long max_frequency = 2970000000ULL - intermediate_frequency;
const unsigned long min_frequency = 2800000000ULL - intermediate_frequency;
const int16_t keyer_speed_factor = 400;
const int16_t tx_timeout = 700;
typedef void(*button_handler)(void);
button_handler handlers[4];
const long handler_interval = 500;
const long minimum_press_time = 50;

unsigned long frequency = min_frequency + 2000000ULL;
unsigned long old_frequency;
int16_t keyer_speed = 20;
RxTx rx_tx_state = RX;

Si5351 si5351;
Encoder encoder;
TM1637Display display(soft_clk_pin, soft_dio_pin);

//#define DEBUG

inline int extract_display_khz(unsigned long freq) {
  return (freq / 10000) % 10000;
}

void handle_encoder_button() {
  int8_t handler_index = 0;
  long start = millis();
  bool pressed = false;
  size_t num_handlers = sizeof(handlers) / sizeof(handlers[0]);

  while (encoder.is_button_pressed()) {
    if (!pressed) {
      pressed = true;
      uint8_t segments[4] = {0};
      segments[handler_index] = 0x40;
      display.setSegments(segments, 4, 0);
    }
    if ((millis() - start) > (handler_index + 1) * handler_interval) {
      handler_index++;
      if (handler_index >= num_handlers) {
        handler_index = 0;
        start = millis();
      }
      uint8_t segments[4] = {0};
      segments[handler_index] = 0x40;
      display.setSegments(segments, 4, 0);
    }
    delay(10);
  }

  if (pressed) {
    if (handlers[handler_index] != nullptr) {
      handlers[handler_index]();
    }
    // restore frequency on the display
    display.showNumberDec(extract_display_khz(frequency + intermediate_frequency), true);
  }
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

  Serial.begin(9600);

  encoder.init(channel_a_pin, channel_b_pin, enc_button_pin);
  encoder.enable(true);

  for (int8_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
      handlers[i] = nullptr;
  }

  // Assign the first handler
  handlers[0] = tune;

  i2c_found = si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0, 0);
  
  si5351.drive_strength(SI5351_CLK0, SI5351_DRIVE_8MA);
  si5351.drive_strength(SI5351_CLK1, SI5351_DRIVE_8MA);
  si5351.set_freq(frequency, SI5351_CLK0);

  old_frequency = frequency;

  display.setBrightness(0x01);
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
    si5351.output_enable(SI5351_CLK1, 0);
    Serial.println("RX");
  } else {
    encoder.enable(false);
    si5351.output_enable(SI5351_CLK1, 1);
    si5351.set_freq(frequency + intermediate_frequency + cw_shift, SI5351_CLK1);
    Serial.println("TX");
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
#ifdef DEBUG
  if (clicks) {
    Serial.println(clicks);
  }
#endif   
  int32_t frequency_update = frequency_step * clicks;
  if (frequency_update) {
    frequency += frequency_update;
    if (frequency < min_frequency) {
      frequency = min_frequency;
    } else if (frequency > max_frequency) {
      frequency = max_frequency;
    }
    if (old_frequency != frequency) {
      si5351.set_freq(frequency, SI5351_CLK0);
      old_frequency = frequency;
      display.showNumberDec(extract_display_khz(frequency + intermediate_frequency), true);
#ifdef DEBUG      
      Serial.println(frequency);
#endif  
      //encoder.reset();
    }
  }
  keyer();
  handle_encoder_button();
  delay(1);
}
