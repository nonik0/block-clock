// TODO: move display driver code into PIO?

#include <ArduinoOTA.h>
#include <EEPROM.h>
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

#define EEPROM_WIFI_REBOOT_ADDR 0
#define MAX_WIFI_RETRY_ATTEMPTS 3
#define WIFI_CONNECT_TIMEOUT_MS 30 * 1000
#define WIFI_STATUS_PERIOD_MS 60 * 1000
#define WIFI_REBOOT_FLAG 0xAA

// ASCII character mapping constants
#define ASCII_SPACE_OFFSET 32    // ASCII space character
#define MAX_CHAR_INDEX 92        // Maximum character index in bitmap array
#define MIN_CHAR_INDEX 0         // Minimum valid character index
#define ASCII_PRINTABLE_MIN 32   // First printable ASCII character (space)
#define ASCII_PRINTABLE_MAX 126  // Last printable ASCII character (~)

// Display timing constants
#define DEFAULT_BRIGHTNESS 110   // Default microseconds per column
#define MIN_BRIGHTNESS 130       // Minimum stable brightness value
#define MAX_BRIGHTNESS 270       // Maximum brightness before flashy
#define WATCHDOG_TIMEOUT_MS 8000 // Watchdog timeout in milliseconds
#define WATCHDOG_MAX_MS 8333     // Maximum possible watchdog timeout
#define DISPLAY_BLANK_DELAY_US 25 // Microseconds for blanking pulse

// String buffer sizes
#define STATUS_STRING_RESERVE 200 // Pre-allocation size for status messages
#define SPEED_MSG_BUFFER_SIZE 100 // Buffer size for speed message formatting
#define ERROR_MSG_BUFFER_SIZE 50  // Buffer size for error message formatting

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
bool displayEnabled = false;
Mode mode = SCROLLING_TEXT;
int displayOffset = 0;                                           // position of whatever is on the display, so this is the compass heading or text scroll
unsigned char displayBuffer[SCROLLING_TEXT_SIZE * CHAR_SPACING]; // this is where the actual bitmap is loaded to be shown on the display

// blinkenlights stuff
uint8_t pixelsActive[IVG116_DISPLAY_WIDTH];         // bit-packed bitmap for blinkenlights (1 bit per pixel)
unsigned long pixelsDelay[IVG116_DISPLAY_WIDTH][IVG116_DISPLAY_HEIGHT]; // this is the delay for the blinkenlights

// scrolling text stuff
bool scrollingEnabled = true;
char scrollText[SCROLLING_TEXT_SIZE] = {"This is block clock, or not?\0 "}; // this is where you put text to display (from wifi or serial or whatever) terminated with \0
int scrollOffset = IVG116_DISPLAY_WIDTH + 1;                                // used to increment scroll for text scrolling (this value here is the start point)
int scrollTextSize = sizeof(scrollText) / sizeof(scrollText[0]);
int scrollSpeedPercent = 52;
int scrollSpeedMs = 15;
unsigned long lastUpdateMillis = 0;

// Performance optimization: cache text buffer and length
bool textBufferDirty = true;  // Flag to rebuild buffer only when text changes
int cachedTextLength = 0;     // Cached length calculation

// display driver stuff
int scanLocation = -1; // keeps track of where we are in the cathode scanning sequence (-1 keeps it from scanning the first cycle)
int brightness = DEFAULT_BRIGHTNESS;  // microseconds per column (range: MIN_BRIGHTNESS - MAX_BRIGHTNESS)

// wifi stuff
WebServer server(80);
int wifiReconnectCount = 0;
int wifiRetryCount = 0;
bool wifiCausedReboot = false;
unsigned long wifiLastStatusCheckMs = 0;

// Connection display timing
enum ConnectionDisplayState { SHOW_MDNS, SHOW_IP, SHOW_ERROR, NORMAL_MODE };
ConnectionDisplayState connectionDisplayState = NORMAL_MODE;
unsigned long connectionDisplayStartMs = 0;
const unsigned long CONNECTION_DISPLAY_DURATION_MS = 3000;
const unsigned long ERROR_DISPLAY_DURATION_MS = 5000;

// Display mode function pointers
typedef void (*DisplayModeFunction)();
DisplayModeFunction currentDisplayMode = NULL;

// Forward declarations
void loadBlinkenLights();
void loadScrollingText();
void updateDisplayMode();
void filterASCIIText(const char* input, char* output, int maxLength);

// Bit manipulation helpers for pixelsActive
inline bool getPixel(int col, int row) {
  return (pixelsActive[col] >> row) & 1;
}

inline void setPixel(int col, int row, bool value) {
  if (value) {
    pixelsActive[col] |= (1 << row);
  } else {
    pixelsActive[col] &= ~(1 << row);
  }
}

// Helper to filter out non-ASCII characters from text
void filterASCIIText(const char* input, char* output, int maxLength) {
  int inputPos = 0;
  int outputPos = 0;
  
  while (input[inputPos] != '\0' && outputPos < maxLength - 1) {
    unsigned char c = (unsigned char)input[inputPos];
    
    // Only keep printable ASCII characters
    if (c >= ASCII_PRINTABLE_MIN && c <= ASCII_PRINTABLE_MAX) {
      output[outputPos] = input[inputPos];
      outputPos++;
    }
    // Skip non-ASCII and non-printable characters
    
    inputPos++;
  }
  
  output[outputPos] = '\0'; // Null terminate
}

// Helper to update scroll text and invalidate caches
void updateScrollText(const char* newText) {
  filterASCIIText(newText, scrollText, SCROLLING_TEXT_SIZE);
  textBufferDirty = true;  // Mark buffer for rebuild
  cachedTextLength = 0;    // Invalidate cached length
}


String parseInput()
{
  String status;
  status.reserve(STATUS_STRING_RESERVE); // Pre-allocate to reduce heap fragmentation

  if (server.hasArg("message"))
  {
    String inputText = server.arg("message");
    updateScrollText(inputText.c_str());
    status += "Message updated: '";
    status += inputText;
    status += "'\n"; // update the scroll text with the new message
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
      updateDisplayMode();
      status += "Mode set to blinkenlights\n";
    }
    else if (value == "scroll" || value == "scrolling" || value == "scroll_text" || value == "scrolling_text")
    {
      mode = Mode::SCROLLING_TEXT;
      updateDisplayMode();
      status += "Mode set to scrolling text\n";
    }
    else
    {
      status += "Mode value '";
      status += value;
      status += "' is invalid\n";
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
      char speedMsg[SPEED_MSG_BUFFER_SIZE];
      snprintf(speedMsg, SPEED_MSG_BUFFER_SIZE, "Scroll speed updated to %d ms (%d%%)\n", scrollSpeedMs, percent);
      status += speedMsg; // this maps 0-100% to 200ms (slow) to 10ms (fast)
    }
    else
    {
      char errorMsg[ERROR_MSG_BUFFER_SIZE];
      snprintf(errorMsg, ERROR_MSG_BUFFER_SIZE, "Invalid scroll speed percent: %d\n", percent);
      status += errorMsg;
    }
  }

  if (server.hasArg("vmirror"))
  {
    String value = server.arg("vmirror");
    value.toLowerCase();

    mirrorVertical = value == "true" || value == "yes" || value == "on";
    status += "Vertical mirror ";
    status += mirrorVertical ? "enabled" : "disabled";
    status += "\n";
  }

  if (server.hasArg("hmirror"))
  {
    String value = server.arg("hmirror");
    value.toLowerCase();

    mirrorHorizontal = value == "true" || value == "yes" || value == "on";
    status += "Horizontal mirror ";
    status += mirrorHorizontal ? "enabled" : "disabled";
    status += "\n";
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
  // Log all requests
  String queryString = server.uri();
  if (server.args() > 0)
  {
    queryString += "?";
    for (int i = 0; i < server.args(); i++)
    {
      if (i > 0) queryString += "&";
      queryString += server.argName(i) + "=" + server.arg(i);
    }
  }
  Serial.println("REST request: " + queryString);
  
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
    status += " - WiFi Caused Reboot: " + String(wifiCausedReboot ? "yes" : "no") + "\n";
    status += " - Wifi Reconnects: " + String(wifiReconnectCount) + "\n";
    if (wifiRetryCount > 0)
    {
      status += " - Wifi Retries: " + String(wifiRetryCount) + "\n";
    }

    Serial.printf("Handled status request\n");
  }
  else
  {
    Serial.println("Handled REST request: " + status);
  }

  server.send(200, "text/plain", status);
}

void showConnectionInfo()
{
  scrollingEnabled = false;
  connectionDisplayState = SHOW_MDNS;
  connectionDisplayStartMs = millis();
  
  // Use char arrays to avoid String concatenation
  char mdnsMessage[50];
  snprintf(mdnsMessage, sizeof(mdnsMessage), "%s.local    ", DEVICE_NAME);
  updateScrollText(mdnsMessage);
  scrollOffset = 0; // Don't scroll
}

void checkWifiStatus()
{
  // Handle connection display sequence
  if (!scrollingEnabled)
  {
    unsigned long timeElapsed = millis() - connectionDisplayStartMs;
    unsigned long timeoutDuration = (connectionDisplayState == SHOW_ERROR) ? ERROR_DISPLAY_DURATION_MS : CONNECTION_DISPLAY_DURATION_MS;
    
    if (timeElapsed > timeoutDuration)
    {
      if (connectionDisplayState == SHOW_MDNS)
      {
        // Transition from mDNS to IP address
        connectionDisplayState = SHOW_IP;
        connectionDisplayStartMs = millis();
        
        // Use char arrays to avoid String concatenation
        char ipMessage[30];
        snprintf(ipMessage, sizeof(ipMessage), "IP: %s    ", WiFi.localIP().toString().c_str());
        updateScrollText(ipMessage);
        scrollOffset = 0; // Don't scroll
      }
      else if (connectionDisplayState == SHOW_IP)
      {
        // Transition to normal scrolling mode
        connectionDisplayState = NORMAL_MODE;
        scrollingEnabled = true;
        updateScrollText("This is block clock, or not?");
        scrollOffset = IVG116_DISPLAY_WIDTH + 1; // Reset scroll position
      }
      else if (connectionDisplayState == SHOW_ERROR)
      {
        // Turn off display after showing error
        connectionDisplayState = NORMAL_MODE;
        displayEnabled = false;
      }
    }
  }
  
  if (millis() - wifiLastStatusCheckMs > WIFI_STATUS_PERIOD_MS)
  {
    wifiLastStatusCheckMs = millis();

    if (WiFi.status() != WL_CONNECTED)
    {
      wifiRetryCount++;

      Serial.printf("WiFi disconnected, attempting to reconnect (attempt %d/%d)\n", wifiRetryCount, MAX_WIFI_RETRY_ATTEMPTS);

      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);

      unsigned long reconnectStartMs = millis();
      while (WiFi.status() != WL_CONNECTED)
      {
        delay(500);
        Serial.print(".");

        if (millis() - reconnectStartMs > WIFI_CONNECT_TIMEOUT_MS)
        {
          WiFi.disconnect();
          Serial.printf("\nWiFi reconnection attempt %d failed after 30 seconds\n", wifiRetryCount);
          
          if (wifiRetryCount >= MAX_WIFI_RETRY_ATTEMPTS)
          {
            Serial.println("Maximum WiFi retry attempts exceeded, rebooting...");
            
            EEPROM.write(EEPROM_WIFI_REBOOT_ADDR, WIFI_REBOOT_FLAG);
            EEPROM.commit();
            rp2040.restart();
          }
          
          updateScrollText("WiFi failing to reconnect");
          return;
        }
      }

      Serial.println("\nReconnected to WiFi");
      
      // Show connection info when reconnected
      showConnectionInfo();
      wifiReconnectCount++;
    }

    wifiRetryCount = 0;
  }
}

void loadBlinkenLights()
{
  for (int col = 0; col < IVG116_DISPLAY_WIDTH; col++)
  {
    unsigned char colBits = 0b00000000;
    for (int row = 0; row < IVG116_DISPLAY_HEIGHT; row++)
    {
      if (pixelsDelay[col][row]-- <= 0)
      {
        bool currentPixel = getPixel(col, row);
        setPixel(col, row, !currentPixel);
        pixelsDelay[col][row] = random(BLINKENLIGHTS_BASE_DELAY, BLINKENLIGHTS_BASE_DELAY * 2);
      }

      colBits |= getPixel(col, row) << row;
    }

    displayBuffer[col] = ~colBits;
  }
}

void loadScrollingText()
{
  // Cache text length calculation 
  if (cachedTextLength == 0) {
    cachedTextLength = strlen(scrollText);
  }
  
  // Always rebuild buffer (original behavior) - the optimization was wrong
  // The display system expects this to run every frame
  int bufIndex = 0;
  int textIndex = 0;
  bool endFlag = false;
  bool foundEndingFlag = false;

  // fills buffer with repeat copies of message
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
    int charBitmapIndex = scrollText[textIndex] - ASCII_SPACE_OFFSET;
    if (charBitmapIndex > MAX_CHAR_INDEX || charBitmapIndex < MIN_CHAR_INDEX)
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
}

void updateDisplayMode()
{
  DisplayModeFunction newMode = NULL;
  
  switch (mode)
  {
    case Mode::BLINKENLIGHTS:
      newMode = loadBlinkenLights;
      break;
    case Mode::SCROLLING_TEXT:
      newMode = loadScrollingText;
      break;
  }
  
  currentDisplayMode = newMode;
}

void loadDisplayBuffer()
{
  if (currentDisplayMode == NULL)
  {
    updateDisplayMode();
  }
  
  if (currentDisplayMode != NULL)
  {
    currentDisplayMode();
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

  delayMicroseconds(DISPLAY_BLANK_DELAY_US);
}

void setup()
{
  Serial.begin(115200);

  EEPROM.begin(512);
  if (EEPROM.read(EEPROM_WIFI_REBOOT_ADDR) == WIFI_REBOOT_FLAG)
  {
    wifiCausedReboot = true;
    EEPROM.write(EEPROM_WIFI_REBOOT_ADDR, 0x00);
    EEPROM.commit();
    Serial.println("Detected WiFi-caused reboot");
  }

  Serial.println("Setting up WiFi services");

  // Enable display and show connecting message
  displayEnabled = true;
  scrollingEnabled = false;
  updateScrollText("WiFi connecting... ");
  scrollOffset = 0; // Don't scroll connecting message

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long wifiConnectStartMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiConnectStartMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("\nConnected to %s\nIP address: %s\n", WIFI_SSID, WiFi.localIP().toString().c_str());
    
    // Show connection info
    showConnectionInfo();
  }
  else
  {
    Serial.println("\nWiFi failed to connect at boot");
    
    // Show error message temporarily before turning off display
    connectionDisplayState = SHOW_ERROR;
    connectionDisplayStartMs = millis();
    scrollingEnabled = false;
    updateScrollText("WiFi connection failed    ");
    scrollOffset = 0; // Don't scroll error message
    // displayEnabled remains true to show error
  }

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

  // Initialize display mode function pointer
  updateDisplayMode();
  
  // Initialize text buffer (force initial build and ASCII filtering)
  updateScrollText(scrollText); // Filter the default text through ASCII filter

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
  watchdog_enable(WATCHDOG_TIMEOUT_MS, true); // max is WATCHDOG_MAX_MS
  watchdog_update();

  // initialize blinkenlights
  for (int col = 0; col < IVG116_DISPLAY_WIDTH; col++)
  {
    pixelsActive[col] = 0; // Initialize to all off
    for (int row = 0; row < IVG116_DISPLAY_HEIGHT; row++)
    {
      setPixel(col, row, random(0, 2));
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
