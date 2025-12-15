#include "MiLibreria.h"

MiLibreriaClass::MiLibreriaClass(int pin) {
  _pin = pin;
  pinMode(_pin, OUTPUT);
}

void MiLibreriaClass::encender() {
  digitalWrite(_pin, HIGH);
}

void MiLibreriaClass::apagar() {
  digitalWrite(_pin, LOW);
}
