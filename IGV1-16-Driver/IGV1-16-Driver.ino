#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <LEAmDNS.h>
#include <string.h>

#include "bitmaps.h"
#include "secrets.h"
#include "hardware/pio.h"
#include "quadrature.pio.h"

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
#define QUADRATURE_A_PIN 21
#define QUADRATURE_B_PIN 20
#define ENC_A 21      //ky-040 clk pin, add 100nF/0.1uF capacitors between pin & ground!!!
#define ENC_B 20      //ky-040 dt  pin, add 100nF/0.1uF capacitors between pin & ground!!!
#define BUTTONENC 22  //ky-040 sw  pin, add 100nF/0.1uF capacitors between pin & ground!!!

#define IVG116_DISPLAY_HEIGHT 7
#define IVG116_DISPLAY_WIDTH 111
#define BLANK_COL 0b11111111

PIO pio = pio0;
uint offset, sm;
int scroll = 0;     // position of whatever is on the display, so this is the compass heading or text scroll
int scroll2 = 112;  // used to increment scroll for text scrolling (this value here is the start point)

enum Mode  // this is for the menu system
{
  COMPASS,
  MENU,
  SCROLLING_TEXT
};
Mode mode = SCROLLING_TEXT;

const int DISPLAY_TEXT_SIZE = 500;
char displayText[DISPLAY_TEXT_SIZE] = { "pIGV1-16\0 " };  // this is where you put text to display (from wifi or serial or whatever) terminated with \0
bool invertedChars[DISPLAY_TEXT_SIZE];                    // put 1 in locations where you want text to be knockout
unsigned char displayBuffer[3000];          // this is where the actual bitmap is loaded to be shown on the display

// scrolling text stuff
int charsInDisplay = 0;
const int numChars = 600;
char receivedChars[numChars];  // an array to store the received data
int scrollSpeed = 30;          // number of millisecods to wait before moving 1 pixel
unsigned long timeLast = 0;
int scrollCharPosition;

unsigned long previousMillis = 0;
const long interval = 100;

unsigned long previousMillisScroll;
int scanLocation = -1;  //keeps track of where we are in the cathode scanning sequence (-1 keeps it from scanning the first cycle)
int brightness = 110;   //number of microseconds to hold on each column of the display (works from like 130 - 270, above that it gets kinda flashy)
bool toggleButton = 1;
volatile bool pressed = 0;
int lastPosition = 0;
int oldPos = 0;

volatile int encoder_value = 0;
int encoderOffset = 0;
int encoderRaw = 0;

bool scrollFlag = true;

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

WebServer server(80);

const int led = LED_BUILTIN;

const String postForms = "<html>\
  <head>\
<title>Pico-W Web Server POST handling</title>\
   <style>\
    body { background-color: #795973; font-family: Arial, Helvetica, Sans-Serif; Color: #000088; font-size: 15}\
    </style>\
  </head>\
  <body>\
    <h1>pIGV1-16 web entry! Type some stuff below to have it show up on the display.</h1><br>\
    <form method=\"post\" enctype=\"application/x-www-form-urlencoded\" action=\"/postform/\">\
      <textarea name=\"hello\" style=\"width:1000px;height:150px;background-color:#CB1334;color:##FFBC46;font-size:150%;\"></textarea><br>\
      <input type=\"submit\" value=\"Submit\">\
    </form>\
  </body>\
</html>";

void handleRoot() {
  //digitalWrite(led, 1);
  server.send(200, "text/html", postForms);
  //digitalWrite(led, 0);
}

void handlePlain() {
  if (server.method() != HTTP_POST) {
    //digitalWrite(led, 1);
    server.send(405, "text/plain", "Method Not Allowed");
    //digitalWrite(led, 0);
  } else {
    //digitalWrite(led, 1);
    server.send(200, "text/plain", "POST body was:\n" + server.arg("plain"));
    //digitalWrite(led, 0);
  }
}

void handleForm() {
  if (server.method() != HTTP_POST) {
    //digitalWrite(led, 1);
    server.send(405, "text/plain", "Method Not Allowed");
    // digitalWrite(led, 0);
  } else {
    //digitalWrite(led, 1);
    String message = "POST form was:\n";
    for (uint8_t i = 0; i < server.args(); i++) {
      message += " " + server.argName(1) + ": " + server.arg(i) + "\n";
    }
    server.send(200, "text/plain", message);
    // digitalWrite(led, 0);
  }
}

void handleNotFound() {
  //digitalWrite(led, 1);
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i = 0; i < server.args(); i++) {
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
  server.send(404, "text/plain", message);
  //digitalWrite(led, 0);
}

void setup() {
  Serial.begin(115200);
  Serial.println("ready");

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(BUTTONENC, INPUT_PULLUP);
  offset = pio_add_program(pio, &quadrature_program);
  sm = pio_claim_unused_sm(pio, true);
  quadrature_program_init(pio, sm, offset, QUADRATURE_A_PIN, QUADRATURE_B_PIN);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected to ");
  Serial.println(ssid);
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (MDNS.begin("picow")) {
    Serial.println("MDNS responder started");
  }

  server.on("/", handleRoot);
  server.on("/postplain/", handlePlain);
  server.on("/postform/", handleForm);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
  recvWithEndMarker();

  // Serial.println(scrollSpeed);
  pio_sm_exec_wait_blocking(pio, sm, pio_encode_in(pio_x, 32));

  encoderRaw = pio_sm_get_blocking(pio, sm);
  encoder_value = encoderRaw - encoderOffset;

  scrollSpeed = encoder_value;

  if (digitalRead(BUTTONENC) == 0) {
    scrollFlag = !scrollFlag;
    encoderOffset = encoderRaw;
    encoder_value = 0;
    delay(300);
  }
}

void recvWithEndMarker() {
  if (server.hasArg("hello") > 0) {
    String webBuffer = server.arg("hello");
    strncpy(displayText, webBuffer.c_str(), DISPLAY_TEXT_SIZE);
  }
}

void setup1() {
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
}

void loop1() {
  unsigned long timeNow = millis();

  if (scrollFlag) {
    if (timeNow - timeLast > scrollSpeed) {
      scroll2++;
      scrollCharPosition = scroll2 / 6;

      if (scroll2 >= (charsInDisplay * 6) + 18) {
        scroll2 = 6;
      }
      timeLast = timeNow;
    }
  } else {
    scroll2 = encoder_value;
  }
  loadDisplayBuffer();
  writeNeon();
}


void loadDisplayBuffer(void) {  //this fills displayBuffer[] with data depending on the menu
  switch (mode) {
    case Mode::COMPASS:  //compass
      break;

    case Mode::MENU:  //compass menu
      break;

    case Mode::SCROLLING_TEXT:  //scrolling text
      int bufferPosition = 0;
      bool endFlag = false;
      bool foundEndingFlag = false;

      for (int i = 0; i < sizeof(displayBuffer); i += 6) {
        if (displayText[(i / 6)] == '\0' && !foundEndingFlag) {
          foundEndingFlag = true;
          charsInDisplay = (i / 6) + 1;
        }

        if (bufferPosition / 6 > charsInDisplay)  //this makes it fill the entire buffer with repeated copies of displayText[]
        {                                         //if speed is an issue you can change this loop to not waste so many cycles
          endFlag = true;
          bufferPosition = -6;
        } else {
          endFlag = false;
        }

        for (int j = 0; j < 6; j++) {
          if (endFlag) {
            displayBuffer[i + j] = BLANK_COL;
            continue;
          }

          int letterInt = displayText[bufferPosition / 6] - 32;
          if (letterInt > 92 || letterInt < 0)
            letterInt = 0;

          if (invertedChars[i / 6]) {  // for inverted charachters
            if (j == 0) {
              displayBuffer[i + j - 1] = ~BLANK_COL;
            }
            if (j < 5) {
              displayBuffer[i + j] = CHAR_BITMAPS[letterInt][j];
            } else {
              displayBuffer[bufferPosition + j] = !BLANK_COL;
            }
          } else {  //non inverted characters
            displayBuffer[i + j] = j < 5
                                     ? ~CHAR_BITMAPS[letterInt][j]
                                     : BLANK_COL;
          }
        }

        bufferPosition += 6;
      }
      break;
  }
}

void writeNeon(void) {  //this function writes data to the display
  digitalWrite(BLANK, LOW);
  delayMicroseconds(10);

  if (Mode::SCROLLING_TEXT) {
    scroll = scroll2;
  } else {
    // TODO
  }

  for (int i = scroll; i < scroll + IVG116_DISPLAY_WIDTH; i++) {

    digitalWrite(ROW0, HIGH);  //pull display anodes low (~100V) while dealing with the scan cathodes
    digitalWrite(ROW1, HIGH);
    digitalWrite(ROW2, HIGH);
    digitalWrite(ROW3, HIGH);
    digitalWrite(ROW4, HIGH);
    digitalWrite(ROW5, HIGH);
    digitalWrite(ROW6, HIGH);

    scanLocation = (scanLocation + 1) % 3;

    if (scanLocation == 0) {

      digitalWrite(SCAN1, HIGH);
      digitalWrite(SCAN2, LOW);
      digitalWrite(SCAN3, LOW);

    } else if (scanLocation == 1) {

      digitalWrite(SCAN2, HIGH);  //pull the scan high first to give an infinitesimally small overlap
      digitalWrite(SCAN1, LOW);
      digitalWrite(SCAN3, LOW);

    } else if (scanLocation == 2) {

      digitalWrite(SCAN3, HIGH);
      digitalWrite(SCAN1, LOW);
      digitalWrite(SCAN2, LOW);
    }

    digitalWrite(ROW0, displayBuffer[i] & 0b01000000);  //displays one column of data on the scan anodes
    digitalWrite(ROW1, displayBuffer[i] & 0b00100000);
    digitalWrite(ROW2, displayBuffer[i] & 0b00010000);
    digitalWrite(ROW3, displayBuffer[i] & 0b00001000);
    digitalWrite(ROW4, displayBuffer[i] & 0b00000100);
    digitalWrite(ROW5, displayBuffer[i] & 0b00000010);
    digitalWrite(ROW6, displayBuffer[i] & 0b00000001);

    delayMicroseconds(brightness);  //give the plasma time to cook

    digitalWrite(ROW0, HIGH);  // pull all anodes low for the next cycle
    digitalWrite(ROW1, HIGH);  // I know this is basically running twice in a row but
    digitalWrite(ROW2, HIGH);  // for some reason the display is more stable
    digitalWrite(ROW3, HIGH);  // when I add this
    digitalWrite(ROW4, HIGH);
    digitalWrite(ROW5, HIGH);
    digitalWrite(ROW6, HIGH);
  }

  scanLocation = -1;  // -1 so it doesnt scan on the first time though the loop

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

  digitalWrite(BLANK, HIGH);  // BLANKing pulse (left on until the next time through this function)

  delayMicroseconds(25);
}
