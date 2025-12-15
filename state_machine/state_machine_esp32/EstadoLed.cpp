#include "EstadoLed.h"


#define BOOT_NUMBER_BLINKS 3
#define BOOT_DELAY_ON 500
#define BOOT_DELAY_OFF 500


EstadoLedClass::EstadoLedClass(int pin) {
  _pin = pin;
  pinMode(_pin, OUTPUT);
}

void EstadoLedClass::encender() {
  digitalWrite(_pin, HIGH);
}

void EstadoLedClass::apagar() {
  digitalWrite(_pin, LOW);
}

void EstadoLedClass::boot() {
  for(int ii=0; ii<BOOT_NUMBER_BLINKS; ii++)
  {
    digitalWrite(_pin, HIGH);
    delay(BOOT_DELAY_ON);   
    digitalWrite(_pin, LOW);
    delay(BOOT_DELAY_OFF);   
  }
}

