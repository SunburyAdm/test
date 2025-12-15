#ifndef USB_CMD_H
#define USB_CMD_H

#include <Arduino.h>
#include "systemState.h"

// Forward declaration (evita incluir todo tu proyecto aquí)
enum AppState : int;

// Inicializa el módulo de comandos
void usbCmd_init(Stream& io);

// Procesa bytes disponibles y ejecuta comandos.
// Devuelve true si pide un cambio de estado global.
// Si regresa true, newState contiene el estado solicitado.
bool usbCmd_process(SystemState& newState);

#endif // USB_CMD_H
