#ifndef MiLibreria_h
#define MiLibreria_h

#include <Arduino.h> // Para usar funciones como pinMode, digitalRead, etc.

class MiLibreriaClass {
  public:
    MiLibreriaClass(int pin);
    void encender();
    void apagar();
  private:
    int _pin;
};

#endif