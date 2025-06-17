// TODO: move display driver code into PIO?

#include <ArduinoOTA.h>
#include <string.h>
#include <SimpleMDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include "bitmaps.h"
#include "secrets.h"

#define IVG116_DISPLAY_HEIGHT 7
#define IVG116_DISPLAY_WIDTH 111
#define BLANK_COL 0b11111111
#define BLANK 2
#define ROW0 6
#define ROW1 7
#define ROW2 8
#define ROW3 9
#define ROW4 10
#define ROW5 11
#define ROW6 12
#define SCAN1 13
#define SCAN2 14
#define SCAN3 15

#define SCROLLING_TEXT_SIZE 500
#define BLINKENLIGHTS_BASE_DELAY 100

enum DeviceType
{
  IGV1_16,
  GIPS_16_1
};

enum Mode
{
  BLINKENLIGHTS,
  SCROLLING_TEXT
};

#ifdef IVG1_16_DEVICE
DeviceType deviceType = IGV1_16;
bool mirrorHorizontal = true;
bool mirrorVertical = true;
#else // default, also #ifdef GIPS_16_1
DeviceType deviceType = GIPS_16_1;
bool mirrorHorizontal = false;
bool mirrorVertical = false;
#endif
bool displayEnabled = true;
Mode mode = SCROLLING_TEXT;
int displayOffset = 0;                                           // position of whatever is on the display, so this is the compass heading or text scroll
unsigned char displayBuffer[SCROLLING_TEXT_SIZE * CHAR_SPACING]; // this is where the actual bitmap is loaded to be shown on the display
// TODO: optimized buf size
// unsigned char displayBuffer[IVG116_DISPLAY_WIDTH];

// blinkenlights stuff
bool pixelsActive[IVG116_DISPLAY_WIDTH][IVG116_DISPLAY_HEIGHT];         // this is the bitmap for the blinkenlights
unsigned long pixelsDelay[IVG116_DISPLAY_WIDTH][IVG116_DISPLAY_HEIGHT]; // this is the delay for the blinkenlights

// scrolling text stuff
bool scrollingEnabled = true;
char scrollText[SCROLLING_TEXT_SIZE] = {"This is block clock, or not?\0 "}; // this is where you put text to display (from wifi or serial or whatever) terminated with \0
int scrollOffset = IVG116_DISPLAY_WIDTH + 1;                                // used to increment scroll for text scrolling (this value here is the start point)
int scrollTextSize = sizeof(scrollText) / sizeof(scrollText[0]);
char receivedChars[SCROLLING_TEXT_SIZE];
int scrollSpeedPercent = 52;
int scrollSpeedMs = 15;
unsigned long lastUpdateMillis = 0;

// display driver stuff
int scanLocation = -1; // keeps track of where we are in the cathode scanning sequence (-1 keeps it from scanning the first cycle)
int brightness = 110;  // number of microseconds to hold on each column of the display (works from like 130 - 270, above that it gets kinda flashy)

WebServer server(80);
int disconnectCount = 0;
bool watchdogRebooted = false;
unsigned long lastStatusCheckMs = 0;

String parseInput()
{
  String status = "";

  if (server.hasArg("message"))
  {
    String inputText = server.arg("message");
    strncpy(scrollText, inputText.c_str(), SCROLLING_TEXT_SIZE);
    status += "Message updated: '" + inputText + "'\n"; // update the scroll text with the new message
  }

  if (server.hasArg("display"))
  {
    String value = server.arg("display");
    value.toLowerCase();

    if (value == "off" || value == "false")
    {
      displayEnabled = false;
      status += "Display disabled\n";
    }
    else if (value == "on" || value == "true")
    {
      displayEnabled = true;
      status += "Display enabled\n";
    }
  }

  if (server.hasArg("mode"))
  {
    String value = server.arg("mode");
    value.toLowerCase();

    if (value == "blinkenlights")
    {
      mode = Mode::BLINKENLIGHTS;
      status += "Mode set to blinkenlights\n";
    }
    else if (value == "scroll" || value == "scrolling" || value == "scroll_text" || value == "scrolling_text")
    {
      mode = Mode::SCROLLING_TEXT;
      status += "Mode set to scrolling text\n";
    }
    else
    {
      status += "Mode value '" + value + "' is invalid\n";
    }
  }

  if (server.hasArg("scrollSpeed"))
  {
    String value = server.arg("scrollSpeed");
    int percent = value.toInt();

    if (percent >= 0 && percent <= 100)
    {
      scrollSpeedPercent = percent;

      const float SlowestScrollMs = 150.0f;
      const float FastestScrollMs = 2.0f;
      float expBase = pow(FastestScrollMs / SlowestScrollMs, 1.0f / 100.0f);
      scrollSpeedMs = constrain((int)SlowestScrollMs * pow(expBase, percent), (int)FastestScrollMs, (int)SlowestScrollMs);
      status += "Scroll speed updated to " + String(scrollSpeedMs) + " ms (" + String(percent) + "%)\n"; // this maps 0-100% to 200ms (slow) to 10ms (fast)
    }
    else
    {
      status += "Invalid scroll speed percent: " + String(percent) + "\n";
    }
  }

  if (server.hasArg("vmirror"))
  {
    String value = server.arg("vmirror");
    value.toLowerCase();

    mirrorVertical = value == "true" || value == "yes" || value == "on";
    status += "Vertical mirror " + String(mirrorVertical ? "enabled" : "disabled") + "\n";
  }

  if (server.hasArg("hmirror"))
  {
    String value = server.arg("hmirror");
    value.toLowerCase();

    mirrorHorizontal = value == "true" || value == "yes" || value == "on";
    status += "Horizontal mirror " + String(mirrorHorizontal ? "enabled" : "disabled") + "\n";
  }

  if (server.hasArg("restart"))
  {
    Serial.write("Restarting...\n");
    rp2040.restart();
  }

  return status;
}

void handleRestRequest()
{
  String status = parseInput();

  if (status.isEmpty())
  {
    status = "Settings:\n";
    status += " - Device Type: " + String(deviceType == IGV1_16 ? "IGV1-16" : "GIPS-16-1") + "\n";
    status += " - Display: " + String(displayEnabled ? "enabled" : "disabled") + "\n";
    status += " - Mode: " + String(mode == Mode::BLINKENLIGHTS ? "blinkenlights" : "scrolling text") + "\n";
    status += " - Scroll Speed: " + String(scrollSpeedPercent) + "% (" + String(scrollSpeedMs) + "ms)\n";
    status += " - Scroll Text: '" + String(scrollText) + "'\n";
    status += " - Horizontal Mirror: " + String(mirrorHorizontal ? "enabled" : "disabled") + "\n";
    status += " - Vertical Mirror: " + String(mirrorVertical ? "enabled" : "disabled") + "\n";
    status += " - Watchdog Caused Reboot: " + String(watchdog_caused_reboot() ? "yes" : "no") + ", Enable: " + String(watchdog_enable_caused_reboot() ? "yes" : "no") + "\n";
    status += " - Wifi Disconnects: " + String(disconnectCount) + "\n";

    Serial.printf("Handled status request\n");
  }
  else
  {
    Serial.println("Handled REST request: " + status);
  }

  server.send(200, "text/plain", status);
}

void checkWifiStatus()
{
  if (millis() - lastStatusCheckMs > 60 * 1000)
  {
    lastStatusCheckMs = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
      disconnectCount++;

      Serial.println("Wifi disconnecting, attempting to reconnect");
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      while (WiFi.status() != WL_CONNECTED)
      {
        delay(500);
        Serial.print(".");
      }
      Serial.println("Reconnected to WiFi");
    }
  }
}

void loadDisplayBuffer()
{
  switch (mode)
  {
  case Mode::BLINKENLIGHTS:
    for (int col = 0; col < IVG116_DISPLAY_WIDTH; col++)
    {
      unsigned char colBits = 0b00000000;
      for (int row = 0; row < IVG116_DISPLAY_HEIGHT; row++)
      {
        if (pixelsDelay[col][row]-- <= 0)
        {
          pixelsActive[col][row] = !pixelsActive[col][row];
          pixelsDelay[col][row] = random(BLINKENLIGHTS_BASE_DELAY, BLINKENLIGHTS_BASE_DELAY * 2);
        }

        colBits |= pixelsActive[col][row] << row;
      }

      displayBuffer[col] = ~colBits;
    }
    break;

  case Mode::SCROLLING_TEXT:
    int bufIndex = 0;
    int textIndex = 0;
    bool endFlag = false;
    bool foundEndingFlag = false;

    // fills buffer with repeat copies of message, TODO: optimize
    // TODO: figure out how current impl avoids buffer skipping to avoid UTF-8 issues by skipping them
    while (bufIndex < sizeof(displayBuffer))
    {
      if (!foundEndingFlag && scrollText[bufIndex / CHAR_SPACING] == '\0')
      {
        foundEndingFlag = true;
        scrollTextSize = bufIndex / CHAR_SPACING + 1;
      }

      if (textIndex > scrollTextSize)
      {
        endFlag = true;
        textIndex = -1;
      }
      else
      {
        endFlag = false;
      }

      // write char bits
      int charBitmapIndex = scrollText[textIndex] - 32; //' ';
      if (charBitmapIndex > 92 || charBitmapIndex < 0)
      {
        charBitmapIndex = 0;
      }
      for (int charOffset = 0; charOffset < CHAR_WIDTH; charOffset++)
      {
        if (endFlag)
        {
          displayBuffer[bufIndex + charOffset] = BLANK_COL;
          continue;
        }

        displayBuffer[bufIndex + charOffset] = ~CHAR_BITMAPS[charBitmapIndex][charOffset];
      }

      // write space between characters
      for (int gapIndex = 0; gapIndex < CHAR_SPACING - CHAR_WIDTH; gapIndex++)
      {
        displayBuffer[bufIndex + CHAR_WIDTH + gapIndex] = BLANK_COL;
      }

      bufIndex += CHAR_SPACING;
      textIndex += 1;
    }
    break;
  }
}

void writeDisplay()
{
  digitalWrite(BLANK, LOW);
  delayMicroseconds(10);

  if (mode == Mode::BLINKENLIGHTS)
  {
    displayOffset = 0;
  }
  else if (mode == Mode::SCROLLING_TEXT)
  {
    displayOffset = scrollOffset;
  }
  else
  {
    // TODO
  }

  scanLocation = -1; // -1 so it doesnt scan on the first time though the loop
  bool scanUp = (deviceType == IGV1_16) ^ mirrorHorizontal;
  int startIndex = scanUp ? displayOffset : displayOffset + IVG116_DISPLAY_WIDTH - 1;
  int endIndex = scanUp ? displayOffset + IVG116_DISPLAY_WIDTH + 1 : displayOffset - 1;
  for (int i = startIndex; i != endIndex; scanUp ? i++ : i--)
  {
    digitalWrite(ROW0, HIGH); // pull display anodes low (~100V) while dealing with the scan cathodes
    digitalWrite(ROW1, HIGH);
    digitalWrite(ROW2, HIGH);
    digitalWrite(ROW3, HIGH);
    digitalWrite(ROW4, HIGH);
    digitalWrite(ROW5, HIGH);
    digitalWrite(ROW6, HIGH);

    scanLocation = (scanLocation + 1) % 3;
    digitalWrite(SCAN1, scanLocation == 0);
    digitalWrite(SCAN2, scanLocation == 1);
    digitalWrite(SCAN3, scanLocation == 2);

    // displays one column of data on the scan anodes
    if (mirrorVertical)
    {
      digitalWrite(ROW6, displayBuffer[i] & 0b01000000);
      digitalWrite(ROW5, displayBuffer[i] & 0b00100000);
      digitalWrite(ROW4, displayBuffer[i] & 0b00010000);
      digitalWrite(ROW3, displayBuffer[i] & 0b00001000);
      digitalWrite(ROW2, displayBuffer[i] & 0b00000100);
      digitalWrite(ROW1, displayBuffer[i] & 0b00000010);
      digitalWrite(ROW0, displayBuffer[i] & 0b00000001);
    }
    else
    {
      digitalWrite(ROW0, displayBuffer[i] & 0b01000000);
      digitalWrite(ROW1, displayBuffer[i] & 0b00100000);
      digitalWrite(ROW2, displayBuffer[i] & 0b00010000);
      digitalWrite(ROW3, displayBuffer[i] & 0b00001000);
      digitalWrite(ROW4, displayBuffer[i] & 0b00000100);
      digitalWrite(ROW5, displayBuffer[i] & 0b00000010);
      digitalWrite(ROW6, displayBuffer[i] & 0b00000001);
    }

    delayMicroseconds(brightness); // give the plasma time to cook

    digitalWrite(ROW0, HIGH); // pull all anodes low for the next cycle
    digitalWrite(ROW1, HIGH); // I know this is basically running twice in a row but
    digitalWrite(ROW2, HIGH); // for some reason the display is more stable
    digitalWrite(ROW3, HIGH); // when I add this
    digitalWrite(ROW4, HIGH);
    digitalWrite(ROW5, HIGH);
    digitalWrite(ROW6, HIGH);
  }

  digitalWrite(ROW0, HIGH);
  digitalWrite(ROW1, HIGH);
  digitalWrite(ROW2, HIGH);
  digitalWrite(ROW3, HIGH);
  digitalWrite(ROW4, HIGH);
  digitalWrite(ROW5, HIGH);
  digitalWrite(ROW6, HIGH);

  digitalWrite(SCAN1, LOW);
  digitalWrite(SCAN2, LOW);
  digitalWrite(SCAN3, LOW);

  digitalWrite(BLANK, HIGH); // BLANKing pulse (left on until the next time through this function)

  delayMicroseconds(25);
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Setting up WiFi services");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\nConnected to %s\nIP address: %s\n", WIFI_SSID, WiFi.localIP().toString().c_str());

  if (MDNS.begin(DEVICE_NAME))
  {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRestRequest);
  server.begin();
  Serial.println("REST server started");

  ArduinoOTA.setHostname(DEVICE_NAME);
  //_ota.setPasswordHash(OTA_PASS_HASH); TODO: add password hash based off IP?

  static bool displayStateOta = false;
  ArduinoOTA.onStart([]()
                     {
                      displayStateOta = displayEnabled; 
                      displayEnabled = false;
                      Serial.printf("Start updating %s", ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem"); });
  ArduinoOTA.onEnd([]()
                   {
                    Serial.printf("\nEnd");
                    displayEnabled = displayStateOta; });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                        { Serial.printf("Progress: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error)
                     {
                Serial.printf("Error[%u]: ", error);
                if (error == OTA_AUTH_ERROR) {
                  Serial.println("Auth Failed");
                } else if (error == OTA_BEGIN_ERROR) {
                  Serial.println("Begin Failed");
                } else if (error == OTA_CONNECT_ERROR) {
                  Serial.println("Connect Failed");
                } else if (error == OTA_RECEIVE_ERROR) {
                  Serial.println("Receive Failed");
                } else if (error == OTA_END_ERROR) {
                  Serial.println("End Failed");
                } });
  ArduinoOTA.begin();

  Serial.println("WiFi services setup complete");
}

void loop()
{
  checkWifiStatus();
  ArduinoOTA.handle();
  server.handleClient();
  watchdog_update();
  yield();
}

void setup1()
{
  Serial.println("Setting up IVG1-16 display");

  pinMode(SCAN1, OUTPUT_8MA);
  pinMode(SCAN2, OUTPUT_8MA);
  pinMode(SCAN3, OUTPUT_8MA);

  pinMode(ROW0, OUTPUT_8MA);
  pinMode(ROW1, OUTPUT_8MA);
  pinMode(ROW2, OUTPUT_8MA);
  pinMode(ROW3, OUTPUT_8MA);
  pinMode(ROW4, OUTPUT_8MA);
  pinMode(ROW5, OUTPUT_8MA);
  pinMode(ROW6, OUTPUT_8MA);

  pinMode(BLANK, OUTPUT_8MA);

  // initialize watchdog -- we really want to reboot if the display stops updating
  watchdog_enable(8000, true); // max is 8333
  watchdog_update();

  // initialize blinkenlights
  for (int col = 0; col < IVG116_DISPLAY_WIDTH; col++)
  {
    for (int row = 0; row < IVG116_DISPLAY_HEIGHT; row++)
    {
      pixelsActive[col][row] = random(0, 2);
      pixelsDelay[col][row] = random(BLINKENLIGHTS_BASE_DELAY * 4, BLINKENLIGHTS_BASE_DELAY * 8);
    }
  }

  Serial.println("IVG1-16 display setup complete");
}

void loop1()
{
  // feed the dog -- will reboot if scan loop is not running
  watchdog_update();

  if (scrollingEnabled)
  {
    if (millis() - lastUpdateMillis > scrollSpeedMs)
    {
      scrollOffset++;

      // move back scroll position after writing out full display and the 3 char gap
      if (scrollOffset >= scrollTextSize * CHAR_SPACING + 3 * CHAR_SPACING)
      {
        scrollOffset = CHAR_SPACING;
      }
      lastUpdateMillis = millis();
    }
  }
  else
  {
    scrollOffset = 0;
  }

  loadDisplayBuffer();
  if (displayEnabled)
  {
    writeDisplay();
  }
}
