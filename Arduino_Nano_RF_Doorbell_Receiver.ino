// Arduino Nano - 433 MHz ASK/OOK receiver + passive buzzer
// Receiver DATA -> D2; passive buzzer (+) -> D8; common GND.
// Compatible with STM32_Door_Lock_With_RF_Doorbell.ino.
const byte RF_RX_PIN = 2;
const byte BUZZER_PIN = 8;
const uint32_t BELL_CODE = 0xA65C39UL;
const unsigned long BUZZ_MS = 2000;
const unsigned long SYNC_MIN_US = 3000;
const unsigned long SYNC_MAX_US = 5500;
unsigned long lastRing = 0;
bool hasRung = false;

bool receiveFrame() {
  // Wait for a 4 ms HIGH synchronization pulse.
  unsigned long sync = pulseIn(RF_RX_PIN, HIGH, 25000UL);
  if (sync < SYNC_MIN_US || sync > SYNC_MAX_US) return false;
  uint32_t received = 0;
  for (byte i = 0; i < 24; ++i) {
    unsigned long width = pulseIn(RF_RX_PIN, HIGH, 4500UL);
    if (width < 250 || width > 2200) return false;
    received = (received << 1) | (width > 950 ? 1UL : 0UL);
  }
  return received == BELL_CODE;
}

void setup() {
  pinMode(RF_RX_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  Serial.begin(9600);
  Serial.println("Wireless doorbell receiver ready");
}

void loop() {
  if (receiveFrame()) {
    unsigned long now = millis();
    if (!hasRung || now - lastRing >= BUZZ_MS) {
      hasRung = true;
      lastRing = now;
      Serial.println("Doorbell received!");
      tone(BUZZER_PIN, 1800, BUZZ_MS);
    }
  }
}
