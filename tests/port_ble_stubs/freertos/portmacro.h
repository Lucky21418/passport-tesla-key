#ifndef PORT_BLE_STUB_PORTMACRO_H
#define PORT_BLE_STUB_PORTMACRO_H

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux)  ((void)(mux))

#endif
