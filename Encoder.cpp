#include "Encoder.h"

Encoder* Encoder::instance = nullptr;

Encoder::Encoder()
  : pos(0), old_pos(0), channel_a_pin(-1), channel_b_pin(-1), button_pin(-1) {}

bool Encoder::init(int a_pin, int b_pin, int btn_pin) {
  if (instance) return false;
  instance = this;

  channel_a_pin = a_pin;
  channel_b_pin = b_pin;
  button_pin = btn_pin;

  pinMode(channel_a_pin, INPUT_PULLUP);
  pinMode(channel_b_pin, INPUT_PULLUP);
  pinMode(button_pin, INPUT_PULLUP);

  _enable = true;

  attachInterrupt(digitalPinToInterrupt(channel_a_pin), update_encoder, CHANGE);
  return true;
}

void Encoder::reset() {
  pos = 0;
}

int32_t Encoder::get_position() {
  noInterrupts();
  int32_t result = pos;
  interrupts();
  return result;
}

int32_t Encoder::get_clicks() {
  int32_t result = get_position();
  result = result - old_pos;
  old_pos = get_position();
  return result;
}

bool Encoder::get_button() {
  return digitalRead(button_pin) == LOW;
}

void Encoder::update_encoder() {
  if (!instance || !instance->_enable) return;

  bool a = digitalRead(instance->channel_a_pin);
  bool b = digitalRead(instance->channel_b_pin);

  if (a != b) {
    instance->pos++;
  } else {
    instance->pos--;
  }
}
