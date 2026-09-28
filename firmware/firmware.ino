// SPDX-License-Identifier: GPL-3.0-only
#include "src/App.h"

App app;

void setup() {
  app.begin();
}

void loop() {
  app.step();
}
