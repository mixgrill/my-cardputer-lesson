#include "M5Cardputer.h"
void setup() {
  M5Cardputer.begin();

  M5Cardputer.Display.setTextSize(2);
  M5Cardputer.Display.setCursor(10, 10);
  M5Cardputer.Display.println("Hello World!");
}

void loop() {
}
