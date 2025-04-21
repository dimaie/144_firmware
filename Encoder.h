#ifndef ENCODER_H
#define ENCODER_H

#include <Arduino.h>

class Encoder {
  public:
    Encoder();
    bool init(int a_pin, int b_pin, int btn_pin);
    void reset();
    int32_t get_position();
    int32_t get_clicks();
    bool get_button();
    inline void enable(bool _enable) {
      this->_enable = _enable;
    }

  private:
    static void update_encoder();
    static Encoder* instance;

    volatile int32_t pos;
    int32_t old_pos;
    bool _enable;
    int channel_a_pin, channel_b_pin, button_pin;
};

#endif
