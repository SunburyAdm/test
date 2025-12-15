#ifndef EstadoLed_h
#define EstadoLed_h

#include <Arduino.h> // Para usar funciones como pinMode, digitalRead, etc.

class EstadoLedClass {
  public:
    EstadoLedClass(int pin);
    void encender();
    void apagar();
    void boot();  // 3 blinks
  private:
    int _pin;
};

#endif