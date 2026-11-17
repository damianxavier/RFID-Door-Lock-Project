#include <SPI.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// STM32 Blue Pill + RC522 + relay lock + 16x2 I2C LCD + exit button + keypad
// Keypad: 4x4 scan code, works with a 4x3 keypad (column 4 / PB11 simply unused)
// Relay IN is driven directly from PA8 (open-drain) with a 10k pull-up from IN to 5V.
// No level shifter is used.
// Only the cards listed in ALLOWED_UIDS open the door by card.
// No MFRC522.h or Keypad library required

#define SS_PIN      PA4
#define RST_PIN     PB0
#define RELAY_PIN   PA8     // open-drain, 5V-tolerant; relay IN with 10k pull-up to 5V
#define BELL_BUTTON_PIN PA1  // doorbell button to GND
#define RF_TX_PIN       PA2  // 433 MHz ASK transmitter DATA
#define BUTTON_PIN  PB4     // exit button: one side to PB4, other side to GND

#define LCD_ADDR    0x27    // try 0x3F if the LCD stays blank

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ---------- Wireless doorbell ----------
// Basic 433 MHz ASK/OOK modules. Transmit a 24-bit identifier with a
// 4 ms HIGH synchronization pulse, then 24 width-encoded data pulses.
// The Nano receiver must use the matching protocol and identifier.
const uint32_t BELL_CODE = 0xA65C39UL;
const unsigned long BELL_DEBOUNCE_MS = 30;
const unsigned long BELL_RETRIGGER_MS = 500;
bool bellButtonWasDown = false;
unsigned long lastBellPress = 0;

void rfSendBellFrame() {
  digitalWrite(RF_TX_PIN, HIGH); delayMicroseconds(4000);
  digitalWrite(RF_TX_PIN, LOW);  delayMicroseconds(2000);
  for (int8_t bit = 23; bit >= 0; --bit) {
    bool one = (BELL_CODE >> bit) & 1UL;
    digitalWrite(RF_TX_PIN, HIGH);
    delayMicroseconds(one ? 1500 : 500);
    digitalWrite(RF_TX_PIN, LOW);
    delayMicroseconds(500);
  }
  digitalWrite(RF_TX_PIN, LOW);
  delay(8);
}

void handleBellButton() {
  bool down = digitalRead(BELL_BUTTON_PIN) == LOW;
  if (!down) { bellButtonWasDown = false; return; }
  if (bellButtonWasDown) return;
  delay(BELL_DEBOUNCE_MS);
  if (digitalRead(BELL_BUTTON_PIN) != LOW) return;
  bellButtonWasDown = true;
  if (lastBellPress != 0 && millis() - lastBellPress < BELL_RETRIGGER_MS) return;
  lastBellPress = millis();
  Serial.println("Doorbell pressed - sending RF signal");
  for (byte i = 0; i < 4; ++i) rfSendBellFrame();
}

// ---------- Keypad ----------
const uint32_t rowPins[4] = { PB12, PB13, PB14, PB15 };   // outputs
const uint32_t colPins[4] = { PB8, PB9, PB10, PB11 };     // inputs with pull-up

const char keyMap[4][4] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' }
};

// ---------- Passcode settings ----------
const char PASSCODE[] = "0000";                  // change this
#define MAX_ENTRY 8                              // longest code that can be typed
#define MAX_ATTEMPTS 3                           // wrong tries before lockout

const unsigned long LOCKOUT_TIME  = 30000;       // keypad lockout (ms)
const unsigned long ENTRY_TIMEOUT = 10000;       // clear half-typed code after this
const unsigned long MESSAGE_TIME  = 2000;        // how long "Incorrect" is shown

// ---------- Allowed cards ----------
const byte ALLOWED_UIDS[][4] = {
  { 0x53, 0xA7, 0x43, 0x96 },
  { 0xA7, 0x94, 0x32, 0x07 }
};
const byte NUM_ALLOWED = sizeof(ALLOWED_UIDS) / sizeof(ALLOWED_UIDS[0]);

// ---------- Lock settings ----------
// Most relay modules are active-LOW (IN = LOW energizes the relay).
#define RELAY_ACTIVE_LOW    true

// Maglock wired through the relay's NC contact (powered at idle):
// false = the lock is OPEN when the relay is OFF
// true  = the lock is OPEN when the relay is ON  (your current wiring)
#define RELAY_ON_OPENS_LOCK true

const unsigned long OPEN_TIME        = 4000;   // lock stays open for 4 seconds
const unsigned long UID_SCREEN_TIME  = 1500;   // how long the first screen is shown
const unsigned long LCD_REFRESH_TIME = 2000;   // redraw interval

bool doorOpen = false;
unsigned long openedAt = 0;

bool pendingScreen = false;
unsigned long screenSwitchAt = 0;

unsigned long lastRefresh = 0;

bool buttonHeld = false;   // true while the exit button is being held down

// Keypad state
char entry[MAX_ENTRY + 1] = "";
byte entryLen = 0;
char lastKey = 0;
unsigned long lastKeyTime = 0;

byte failedAttempts = 0;
bool lockedOut = false;
unsigned long lockedUntil = 0;

bool restorePending = false;
unsigned long restoreAt = 0;

// Current LCD text (kept so the screen can be redrawn)
char line0[17] = "";
char line1[17] = "";

// ---------- RC522 registers ----------
#define CommandReg     0x01
#define ComIrqReg      0x04
#define DivIrqReg      0x05
#define ErrorReg       0x06
#define FIFODataReg    0x09
#define FIFOLevelReg   0x0A
#define BitFramingReg  0x0D
#define ModeReg        0x11
#define TxControlReg   0x14
#define TxASKReg       0x15
#define CRCResultRegH  0x21
#define CRCResultRegL  0x22
#define TModeReg       0x2A
#define TPrescalerReg  0x2B
#define TReloadRegH    0x2C
#define TReloadRegL    0x2D
#define VersionReg     0x37

#define PCD_IDLE       0x00
#define PCD_CALCCRC    0x03
#define PCD_TRANSCEIVE 0x0C
#define PCD_SOFTRESET  0x0F

// SPI register write
void writeReg(byte reg, byte value)
{
  digitalWrite(SS_PIN, LOW);
  SPI.transfer((reg << 1) & 0x7E);
  SPI.transfer(value);
  digitalWrite(SS_PIN, HIGH);
}

// SPI register read
byte readReg(byte reg)
{
  digitalWrite(SS_PIN, LOW);
  SPI.transfer(((reg << 1) & 0x7E) | 0x80);
  byte value = SPI.transfer(0x00);
  digitalWrite(SS_PIN, HIGH);
  return value;
}

void setMask(byte reg, byte mask)
{
  writeReg(reg, readReg(reg) | mask);
}

void clearMask(byte reg, byte mask)
{
  writeReg(reg, readReg(reg) & (byte)~mask);
}

// Calculate CRC_A
bool calculateCRC(byte *data, byte length, byte *result)
{
  writeReg(CommandReg, PCD_IDLE);
  writeReg(DivIrqReg, 0x04);
  writeReg(FIFOLevelReg, 0x80);

  for (byte i = 0; i < length; i++)
    writeReg(FIFODataReg, data[i]);

  writeReg(CommandReg, PCD_CALCCRC);

  unsigned long start = millis();

  while (!(readReg(DivIrqReg) & 0x04))
  {
    if (millis() - start > 100)
      return false;
  }

  result[0] = readReg(CRCResultRegL);
  result[1] = readReg(CRCResultRegH);

  return true;
}

// Send command and receive response
bool transceive(byte *sendData, byte sendLen,
                byte *backData, byte *backLen,
                byte validBits = 0)
{
  writeReg(CommandReg, PCD_IDLE);
  writeReg(ComIrqReg, 0x7F);
  writeReg(FIFOLevelReg, 0x80);
  writeReg(BitFramingReg, validBits);

  for (byte i = 0; i < sendLen; i++)
    writeReg(FIFODataReg, sendData[i]);

  writeReg(CommandReg, PCD_TRANSCEIVE);
  setMask(BitFramingReg, 0x80);

  unsigned long start = millis();
  byte irq = 0;

  while (millis() - start < 100)
  {
    irq = readReg(ComIrqReg);

    if (irq & 0x30)
      break;

    if (irq & 0x01)
    {
      clearMask(BitFramingReg, 0x80);
      return false;
    }
  }

  clearMask(BitFramingReg, 0x80);

  if (!(irq & 0x30))
    return false;

  if (readReg(ErrorReg) & 0x1B)
    return false;

  byte count = readReg(FIFOLevelReg);

  if (count == 0 || count > *backLen)
    return false;

  *backLen = count;

  for (byte i = 0; i < count; i++)
    backData[i] = readReg(FIFODataReg);

  return true;
}

// Request an ISO14443A card
bool requestCard()
{
  byte command = 0x26;
  byte answer[2];
  byte answerLen = sizeof(answer);

  if (!transceive(&command, 1, answer, &answerLen, 7))
    return false;

  return answerLen == 2;
}

// Read a common 4-byte card UID
bool readCardUID(byte *uid)
{
  byte command[2] = {0x93, 0x20};
  byte response[5];
  byte responseLen = sizeof(response);

  if (!transceive(command, 2, response, &responseLen))
    return false;

  if (responseLen != 5)
    return false;

  byte bcc = response[0] ^ response[1] ^
             response[2] ^ response[3];

  if (bcc != response[4])
    return false;

  // Cascade tag indicates a longer UID.
  if (response[0] == 0x88)
    return false;

  byte selectCmd[9] = {
    0x93, 0x70,
    response[0], response[1],
    response[2], response[3],
    response[4], 0x00, 0x00
  };

  byte crc[2];

  if (!calculateCRC(selectCmd, 7, crc))
    return false;

  selectCmd[7] = crc[0];
  selectCmd[8] = crc[1];

  byte sak[3];
  byte sakLen = sizeof(sak);

  if (!transceive(selectCmd, 9, sak, &sakLen))
    return false;

  for (byte i = 0; i < 4; i++)
    uid[i] = response[i];

  return true;
}

// ========================================
// RC522 INIT (can be called any time)
// Returns the chip version register
// ========================================
byte initReader()
{
  SPI.beginTransaction(
    SPISettings(1000000, MSBFIRST, SPI_MODE0)
  );

  digitalWrite(RST_PIN, LOW);
  delay(10);
  digitalWrite(RST_PIN, HIGH);
  delay(50);

  writeReg(CommandReg, PCD_SOFTRESET);
  delay(50);

  writeReg(TModeReg, 0x8D);
  writeReg(TPrescalerReg, 0x3E);
  writeReg(TReloadRegH, 0x00);
  writeReg(TReloadRegL, 0x1E);

  writeReg(TxASKReg, 0x40);
  writeReg(ModeReg, 0x3D);

  setMask(TxControlReg, 0x03);

  byte version = readReg(VersionReg);

  SPI.endTransaction();

  return version;
}

// True if the antenna is still on (i.e. the RC522 was not reset)
bool readerHealthy()
{
  SPI.beginTransaction(
    SPISettings(1000000, MSBFIRST, SPI_MODE0)
  );

  byte tx = readReg(TxControlReg);

  SPI.endTransaction();

  return (tx & 0x03) == 0x03;
}

// ========================================
// LCD HELPERS
// ========================================

// Print text and pad with spaces to 16 characters
void printPadded(const char *s)
{
  byte n = 0;

  while (s[n] && n < 16)
  {
    lcd.print(s[n]);
    n++;
  }

  while (n < 16)
  {
    lcd.print(' ');
    n++;
  }
}

// Redraw the stored text without clearing the screen
void lcdRedraw()
{
  lcd.setCursor(0, 0);
  printPadded(line0);

  lcd.setCursor(0, 1);
  printPadded(line1);
}

// Store new text and draw it
void lcdShow(const char *a, const char *b)
{
  strncpy(line0, a, 16);
  line0[16] = 0;

  strncpy(line1, b, 16);
  line1[16] = 0;

  lcdRedraw();
}

// Re-initialize the LCD (recovers from noise) and redraw
void lcdRecover()
{
  lcd.init();
  lcd.backlight();
  lcdRedraw();
}

// Idle / current-state screen
void showStateScreen()
{
  if (doorOpen)
    lcdShow("Door: OPEN", "Closing soon");
  else
    lcdShow("Door: CLOSED", "Card or Passcode");
}

// ========================================
// LOCK CONTROL
// The caller sets the LCD text first; this
// switches the relay, then recovers the
// reader and LCD from any switching noise.
// ========================================
void setDoor(bool open)
{
  doorOpen = open;

  // Should the relay be energized to reach this door state?
  bool relayEnergized = RELAY_ON_OPENS_LOCK ? open : !open;

  digitalWrite(RELAY_PIN, (relayEnergized == RELAY_ACTIVE_LOW) ? LOW : HIGH);

  Serial.print("Door lock: ");
  Serial.println(open ? "OPEN" : "CLOSED");

  // Let the supply settle, then make sure the reader survived the switching
  delay(100);

  if (!readerHealthy())
  {
    Serial.println("RC522 reset detected after relay switch - reinitializing");
  }

  // Always reinitialize after a relay change (cheap and safe)
  initReader();

  // Same for the LCD: re-init it and redraw the text
  lcdRecover();
}

void clearEntry()
{
  entryLen = 0;
  entry[0] = 0;
}

// Open the door for OPEN_TIME with a first screen, then the "open" screen
void grantAccess(const char *l0, const char *l1)
{
  clearEntry();
  restorePending = false;

  lcdShow(l0, l1);

  setDoor(true);
  openedAt = millis();

  pendingScreen = true;
  screenSwitchAt = millis() + UID_SCREEN_TIME;
}

void uidToString(byte *uid, char *buf)
{
  snprintf(buf, 17, "%02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3]);
}

void printUID(byte *uid)
{
  char buf[17];
  uidToString(uid, buf);

  Serial.print("CARD UID: ");
  Serial.println(buf);
}

// True if this UID is in the allowed list
bool isAllowedUID(byte *uid)
{
  for (byte i = 0; i < NUM_ALLOWED; i++)
  {
    if (memcmp(uid, ALLOWED_UIDS[i], 4) == 0)
      return true;
  }
  return false;
}

// ========================================
// EXIT BUTTON
// ========================================
void handleButton()
{
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  if (!pressed)
  {
    buttonHeld = false;
    return;
  }

  // Already handled this press, wait for release
  if (buttonHeld)
    return;

  // Debounce: make sure the press is real
  delay(20);

  if (digitalRead(BUTTON_PIN) != LOW)
    return;

  buttonHeld = true;

  Serial.println("Exit button pressed");

  if (!doorOpen)
  {
    Serial.println("Exit - opening for 4 seconds");
    grantAccess("Exit Button", "Door opening");
  }
  else
  {
    // Door already open: restart the timer so the person can get out
    Serial.println("Door already open - timer restarted");
    openedAt = millis();
    pendingScreen = false;
    showStateScreen();
  }
}

// ========================================
// KEYPAD
// ========================================

// Returns the pressed key, or 0 if none
char scanKeypad()
{
  for (byte r = 0; r < 4; r++)
  {
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(10);

    for (byte c = 0; c < 4; c++)
    {
      if (digitalRead(colPins[c]) == LOW)
      {
        digitalWrite(rowPins[r], HIGH);
        return keyMap[r][c];
      }
    }

    digitalWrite(rowPins[r], HIGH);
  }

  return 0;
}

// Show what has been typed so far (masked)
void showEntryScreen()
{
  char masked[MAX_ENTRY + 1];

  for (byte i = 0; i < entryLen; i++)
    masked[i] = '*';

  masked[entryLen] = 0;

  lcdShow("Enter Passcode", masked);
}

void submitEntry()
{
  if (entryLen == 0)
    return;

  if (strcmp(entry, PASSCODE) == 0)
  {
    Serial.println("Passcode correct - opening for 4 seconds");
    failedAttempts = 0;
    grantAccess("Passcode OK", "Door opening");
    return;
  }

  // Wrong passcode
  failedAttempts++;
  clearEntry();

  Serial.print("Incorrect passcode (attempt ");
  Serial.print(failedAttempts);
  Serial.print(" of ");
  Serial.print(MAX_ATTEMPTS);
  Serial.println(")");

  if (failedAttempts >= MAX_ATTEMPTS)
  {
    lockedOut = true;
    lockedUntil = millis() + LOCKOUT_TIME;
    restorePending = false;

    Serial.println("Too many wrong attempts - keypad locked for 30 seconds");
    lcdShow("Too many tries", "Keypad locked");
  }
  else
  {
    lcdShow("Incorrect", "Passcode");
    restorePending = true;
    restoreAt = millis() + MESSAGE_TIME;
  }
}

void processKey(char k)
{
  // Keypad is ignored while locked out or while the door is already open
  if (lockedOut || doorOpen)
    return;

  lastKeyTime = millis();
  restorePending = false;

  if (k == '*')
  {
    clearEntry();
    Serial.println("Passcode entry cleared");
    showStateScreen();
  }
  else if (k == '#')
  {
    submitEntry();
  }
  else
  {
    if (entryLen < MAX_ENTRY)
    {
      entry[entryLen++] = k;
      entry[entryLen] = 0;
    }

    Serial.print("Passcode entry: ");
    for (byte i = 0; i < entryLen; i++)
      Serial.print('*');
    Serial.println();

    showEntryScreen();
  }
}

void handleKeypad()
{
  char k = scanKeypad();

  if (k == 0)
  {
    lastKey = 0;
    return;
  }

  // Same key still held down: already handled
  if (k == lastKey)
    return;

  // Debounce: make sure the press is real
  delay(20);

  if (scanKeypad() != k)
    return;

  lastKey = k;
  processKey(k);
}

// ========================================
// SETUP
// ========================================
void setup()
{
  // Relay first: release the line (open-drain, pulled up to 5V) so the relay stays OFF
  digitalWrite(RELAY_PIN, HIGH);
  pinMode(RELAY_PIN, OUTPUT_OPEN_DRAIN);

  Serial.begin(115200);
  delay(1000);

  // Exit and doorbell buttons; RF transmitter
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BELL_BUTTON_PIN, INPUT_PULLUP);
  pinMode(RF_TX_PIN, OUTPUT);
  digitalWrite(RF_TX_PIN, LOW);

  // Keypad
  for (byte i = 0; i < 4; i++)
  {
    pinMode(rowPins[i], OUTPUT);
    digitalWrite(rowPins[i], HIGH);
    pinMode(colPins[i], INPUT_PULLUP);
  }

  // LCD
  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdShow("RFID DOOR LOCK", "Starting...");

  Serial.println();
  Serial.println("STM32 RC522 RFID DOOR LOCK");

  // RC522 pins
  pinMode(SS_PIN, OUTPUT);
  digitalWrite(SS_PIN, HIGH);

  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, HIGH);

  SPI.begin();

  byte version = initReader();

  Serial.print("RC522 version: 0x");
  Serial.println(version, HEX);

  if (version == 0x00 || version == 0xFF)
  {
    Serial.println("ERROR: Reader not detected!");
    lcdShow("RC522 ERROR", "Not detected");
    delay(2000);
  }
  else
  {
    Serial.println("Reader detected.");

    char buf[17];
    snprintf(buf, sizeof(buf), "Version 0x%02X", version);
    lcdShow("RC522 OK", buf);
    delay(1000);
  }

  // Relay (pin already set up at the start of setup)
  lcdShow("Door: CLOSED", "Card or Passcode");
  setDoor(false);   // start closed

  Serial.println("Scan your card, enter the passcode, or press the exit button.");
}

// ========================================
// LOOP
// ========================================
void loop()
{
  // Close the lock after OPEN_TIME
  if (doorOpen && (millis() - openedAt >= OPEN_TIME))
  {
    pendingScreen = false;
    lcdShow("Door: CLOSED", "Card or Passcode");
    setDoor(false);
    Serial.println("Ready for the next card or passcode.");
  }

  // After the first screen, show the door-open screen
  if (pendingScreen && (millis() >= screenSwitchAt))
  {
    pendingScreen = false;
    lcdShow("Door: OPEN", "Closing soon");
  }

  // End of keypad lockout
  if (lockedOut && (millis() >= lockedUntil))
  {
    lockedOut = false;
    failedAttempts = 0;
    Serial.println("Keypad lockout ended");
    showStateScreen();
  }

  // Go back to the idle screen after the "Incorrect" message
  if (restorePending && (millis() >= restoreAt))
  {
    restorePending = false;
    showStateScreen();
  }

  // Clear a half-typed passcode after a pause
  if (entryLen > 0 && (millis() - lastKeyTime >= ENTRY_TIMEOUT))
  {
    clearEntry();
    Serial.println("Passcode entry timed out");
    showStateScreen();
  }

  // Safety net: if the RC522 got reset by a power glitch, set it up again
  if (!readerHealthy())
  {
    Serial.println("RC522 reset detected - reinitializing");
    lcdShow("RC522 reset", "Reinitializing");
    initReader();
    showStateScreen();
  }

  // Exit button and independent wireless doorbell
  handleButton();
  handleBellButton();

  // Keypad
  handleKeypad();

  // RFID card
  SPI.beginTransaction(
    SPISettings(1000000, MSBFIRST, SPI_MODE0)
  );

  byte uid[4];
  bool found = false;

  if (requestCard())
    found = readCardUID(uid);

  SPI.endTransaction();

  if (found)
  {
    printUID(uid);

    if (!isAllowedUID(uid))
    {
      Serial.println("Access denied - card not allowed");
      pendingScreen = false;
      lcdShow("Access Denied", "Unknown card");
      restorePending = true;
      restoreAt = millis() + MESSAGE_TIME;
    }
    else if (!doorOpen)
    {
      Serial.println("Access granted - opening for 4 seconds");

      char buf[17];
      uidToString(uid, buf);
      grantAccess("Access Granted", buf);
    }
    else
    {
      Serial.println("Door already open - scan ignored");
      pendingScreen = false;
      lcdShow("Door is open", "Scan ignored");
    }
  }

  // Periodic redraw heals small LCD glitches
  if (millis() - lastRefresh >= LCD_REFRESH_TIME)
  {
    lastRefresh = millis();
    lcdRedraw();
  }

  delay(50);
}