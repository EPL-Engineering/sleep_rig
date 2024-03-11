// TODO

#include <FastLED.h>
#include <RTClib.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>

// constant macros
#define BAUD_RATE 9600    // (bits/second)          serial buffer baud rate
#define BUFF_SIZE 64      // (bytes)                maximum message size
#define MSG_TIMEOUT 5000  // (milliseconds)         timeout from last character received
#define NUM_CMDS 6        // (positive integer)     number of commands in the command table struct

#define DHT_PIN_1 3
#define DHT_PIN_2 4
#define LED_PIN_1 5
#define LED_PIN_2 6

#define NUM_BOXES 2
#define NUM_LEDS 1
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define UPDATES_PER_SECOND 100

RTC_PCF8523 rtc;                 // define the RTC object
CRGB leds[NUM_BOXES][NUM_LEDS];  // LED manipulation for booths
uint8_t buff[BUFF_SIZE];         // message read buffer
bool debugMode = false;          // print serial output for debugging
unsigned long lastCharTime;      // used to timeout a message that is cut off or too slow
uint8_t brightnessVal = 1;       // default brightness for boxes (can be adjusted with LED_INT)
DHT dhtSensors[NUM_BOXES] = {
  DHT(DHT_PIN_1, DHT11),
  DHT(DHT_PIN_2, DHT11)
};

// RTC global variables
bool prevIsDay[NUM_BOXES] = { false, false };   // keep track of the last check for comparison to next check
bool startCheck[NUM_BOXES] = { false, false };  // user defined start variable for checking day and night cycles
bool firstCheck[NUM_BOXES] = { true, true };    // some things only need to be done on the first iteration
int dayStart[NUM_BOXES] = { 480, 480 };         // user defined day time start (white light)
int dayStop[NUM_BOXES] = { 1080, 1080 };        // uesr defined night time start (red light)

// custom data type that is a pointer to a command function
typedef void (*CmdFunc)(int argc, char* argv[]);

// command structure
typedef struct {
  int commandArgs;          // number of command line arguments including the command string
  char* commandString;      // command string (e.g. "LED_ON", "LED_OFF"; use caps for alpha characters)
  CmdFunc commandFunction;  // pointer to the function that will execute if this command matches
} CmdStruct;

void Command_LOG_REQ(int argc = 0, char* argv[] = { NULL });
void Command_RTC_BEG(int argc = 0, char* argv[] = { NULL });
void Command_RTC_END(int argc = 0, char* argv[] = { NULL });
void Command_LED_RGB(int argc = 0, char* argv[] = { NULL });
void Command_LED_INT(int argc = 0, char* argv[] = { NULL });
void Command_LED_OFF(int argc = 0, char* argv[] = { NULL });

// command table
CmdStruct cmdTable[NUM_CMDS] = {

  {
    .commandArgs = 1,                    // LOG_REQ
    .commandString = "LOG_REQ",          // capitalized command for getting environment datum
    .commandFunction = &Command_LOG_REQ  // run Command_LOG_REQ
  },

  {
    .commandArgs = 4,                    // RTC_BEG <boxNum> <dayStart> <dayStop>
    .commandString = "RTC_BEG",          // capitalized command for starting the RTC check
    .commandFunction = &Command_RTC_BEG  // run Command_RTC_BEG
  },

  {
    .commandArgs = 2,                    // RTC_END <boxNum>
    .commandString = "RTC_END",          // capitalized command for stopping the RTC check
    .commandFunction = &Command_RTC_END  // run Command_RTC_END
  },

  {
    .commandArgs = 5,                    // LED_RGB <boxNum> <R> <G> <B>
    .commandString = "LED_RGB",          // capitalized command for adjusting LED color
    .commandFunction = &Command_LED_RGB  // run Command_LED_RGB
  },

  {
    .commandArgs = 2,                    // LED_INT <INT>
    .commandString = "LED_INT",          // capitalized command for adjusting LED brightness
    .commandFunction = &Command_LED_INT  // run Command_LED_INT
  },

  {
    .commandArgs = 2,                    // LED_OFF <boxNum>
    .commandString = "LED_OFF",          // capitalized command for turning the LED off
    .commandFunction = &Command_LED_OFF  // run Command_LED_OFF
  }

};



/******************************************************************************************
 ************************************   Setup & Loop   ************************************
 ******************************************************************************************/



void setup(void) {

  // power up safety delay
  delay(3000);

  // start up serial buffer and flush it
  Serial.begin(BAUD_RATE);
  while (Serial.available() > 0) { Serial.read(); }
  Serial.flush();
  if (debugMode) Serial.println("*********BEGIN SETUP*********");

  // setup DHT climate sensors
  dhtSensors[0].begin();  // box 1
  dhtSensors[1].begin();  // box 2

  // setup the WS2812B LED strips
  FastLED.addLeds<LED_TYPE, LED_PIN_1, COLOR_ORDER>(leds[0], NUM_LEDS);
  FastLED.addLeds<LED_TYPE, LED_PIN_2, COLOR_ORDER>(leds[1], NUM_LEDS);

  // set all of the LED in each strip to black
  for (int i = 0; i < NUM_BOXES; ++i) {
    for (int j = 0; j < NUM_LEDS; ++j) {
      leds[i][j].setRGB(0, 0, 0);
    }
  }

  // set their brightness to default
  FastLED.setBrightness(brightnessVal);

  // update all of those changes
  FastLED.show();
  if (debugMode) Serial.println("LED: initialized successfully");

  // initialize the RTC
  if (!rtc.begin()) {
    if (debugMode) Serial.println("RTC: couldn't initialize module... please restart!");
    while (true)
      ;
  } else {
    if (debugMode) Serial.println("RTC: initialized successfully");
  }

  // check if the RTC lost power
  if (!rtc.initialized() || rtc.lostPower()) {
    if (debugMode) Serial.println("RTC: uninitialized, setting the date and time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // when the RTC was stopped and stays connected to the battery, it has
  // to be restarted by clearing the STOP bit. Let's do this to ensure
  // the RTC is running
  rtc.start();

  // separate commands with new line
  if (debugMode) Serial.println("**********END SETUP**********");

  // first log entry on serial connect
  Command_LOG_REQ();
  Serial.println("CONNECT");
}

void loop(void) {

  // slow down LED color transitions (not necessary if static)
  // FastLED.delay(1000 / UPDATES_PER_SECOND);

  // receive the serial input and process the message
  receive_message();

  // start the day and night cycles for each box
  for (int box = 0; box < NUM_BOXES; ++box) {
    if (startCheck[box]) {
      update_day_night(box);
    }
  }
}



/******************************************************************************************
 **********************************   Helper Functions   **********************************
 ******************************************************************************************/



// reads in serial byte-by-byte and executes command
void receive_message(void) {

  // control variables
  uint8_t rc;              // stores byte from serial
  static uint8_t idx = 0;  // keeps track of which byte we are on

  // check for a timeout if we've received at least one character
  if (idx > 0) {
    // ignore message and reset index if we exceed timeout
    if ((millis() - lastCharTime) > MSG_TIMEOUT) {
      idx = 0;
      if (debugMode) Serial.println("SYS: message timeout");
    }
  }

  // if there's a character waiting
  if (Serial.available() > 0) {
    // update the last character timer
    lastCharTime = millis();
    // read the character
    rc = Serial.read();
    // if character is newline (serial monitor delimeter)
    if (rc == '\n') {
      // null-terminate the message
      buff[idx] = '\0';
      // and go process it
      process_message();
      // reset the buffer index to get ready for the next message
      idx = 0;
    } else {
      // store capitalized character and bump buffer pointer
      buff[idx++] = toupper(rc);
      // but not beyond the limits of the buffer
      if (idx == BUFF_SIZE) {
        --idx;
      }
    }
  }
}

// matches the message buffer to supported commands
void process_message(void) {

  // split the input message by a space delimeter (first token is the command name)
  char* token = strtok(buff, " ");

  // if we at least have a command name (first token)
  if (token != NULL) {
    // walk through command table to search for message match
    for (int i = 0; i < NUM_CMDS; ++i) {
      // start handling the arguments if the requested command is supported
      if (strcmp(token, cmdTable[i].commandString) == 0) {
        // get the number of required arguments
        int argc = cmdTable[i].commandArgs;
        // create an array to store arguments
        char* argv[argc];
        // store the command name in argv
        argv[0] = token;
        // parse the arguments required for the command
        for (int j = 1; j < argc; ++j) {
          // get the next argument
          token = strtok(NULL, " ");
          // check if there is too few arguments
          if (token == NULL) {
            if (debugMode) {
              Serial.print("SYS: not enough args for '");
              Serial.print(argv[0]);
              Serial.println("'");
            }
            return;
          }
          // store if it's provided
          argv[j] = token;
        }
        // try to get another argument (should be done already)
        token = strtok(NULL, " ");
        // check if there is too many arguments
        if (token != NULL) {
          if (debugMode) {
            Serial.print("SYS: too many args for '");
            Serial.print(argv[0]);
            Serial.println("'");
          }
          return;
        }
        // execute the command and pass any arguments
        cmdTable[i].commandFunction(argc, argv);
        // make sure we echo the command if manual log entry (this gives us a new line too)
        if (i == 0) echo_command_args(argc, argv);
        // ok get out of here now
        return;
      }
    }
  }

  // send user response if we did not find a match in the command table
  if (debugMode) {
    Serial.print("SYS: unrecognized command '");
    Serial.print(token);
    Serial.println("'");
  }
}

// update day and night cycle
void update_day_night(int box) {

  // get the current time
  DateTime now = rtc.now();
  int currentMinute = now.hour() * 60 + now.minute();

  // determine if it's day or night
  bool isDay = (currentMinute >= dayStart[box] && currentMinute < dayStop[box]);

  // only prompt user if we changed from the last day check
  if (firstCheck[box] || isDay != prevIsDay[box]) {
    // store the previous cycle day time check
    prevIsDay[box] = isDay;
    // set the LED color to white during the day
    if (isDay) {
      // prompt the user of date and time
      if (debugMode) Serial.println("RTC: It is now day time...");
      // manually set the RGB
      int argc = 5;
      char* boxStr = (box == 0) ? strdup("0") : strdup("1");
      char* argv[argc] = { "LED_RGB", boxStr, "255", "255", "255" };
      Command_LED_RGB(argc, argv);
    }
    // set the LED color to red during the night
    else {
      // prompt the user of date and time
      if (debugMode) Serial.println("RTC: It is now night time...");
      // manually set the RGB
      int argc = 5;
      char* boxStr = (box == 0) ? strdup("0") : strdup("1");
      char* argv[argc] = { "LED_RGB", boxStr, "255", "0", "0" };
      Command_LED_RGB(argc, argv);
    }
    // it's not the first check anymore
    firstCheck[box] = false;
  }
}

// echos the command back to the user via serial
void echo_command_args(int argc, char* argv[]) {

  for (int i = 0; i < argc; ++i) {
    Serial.print(argv[i]);
    if (i < argc - 1) {
      Serial.print(" ");
    }
  }
  Serial.println();
}

// splits the time format "HHMM" into "HH" and "MM"
void split_time(const char* inputTime, char* hours, char* minutes) {

  // validate the input format
  if (strlen(inputTime) != 4) {
    strcpy(hours, "00");
    strcpy(minutes, "00");
    return;
  }

  // extract hours and minutes
  strncpy(hours, inputTime, 2);
  hours[2] = '\0';  // null terminate
  strncpy(minutes, inputTime + 2, 2);
  minutes[2] = '\0';  // null terminate
}



/******************************************************************************************
 *********************************   Command Functions   **********************************
 ******************************************************************************************/



// send log entry to serial when requested
void Command_LOG_REQ(int argc, char* argv[]) {

  if (debugMode) Serial.println("Made it to Command_LOG_REQ 0");

  // write buffer for formatting strings
  char logStr[128];

  // date and time string buffer
  DateTime now = rtc.now();
  char date[11];
  char time[9];
  sprintf(date, "%02d/%02d/%04d", now.month(), now.day(), now.year());
  sprintf(time, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  if (debugMode) Serial.println("Made it to Command_LOG_REQ 1");

  // get the LED information
  CRGB color1 = leds[0][0];
  uint8_t r1 = color1.r;
  uint8_t g1 = color1.g;
  uint8_t b1 = color1.b;
  CRGB color2 = leds[1][0];
  uint8_t r2 = color2.r;
  uint8_t g2 = color2.g;
  uint8_t b2 = color2.b;

  if (debugMode) Serial.println("Made it to Command_LOG_REQ 2");

  // get the climate information
  float hum1 = dhtSensors[0].readHumidity();
  float hum2 = dhtSensors[1].readHumidity();
  float temp1 = dhtSensors[0].readTemperature();
  float temp2 = dhtSensors[1].readTemperature();

  if (debugMode) Serial.println("Made it to Command_LOG_REQ 3");

  // sprintf cannot handle floats in avr gcc compiler (Arduino compiler) so convert to string
  char hum1Str[7];
  char hum2Str[7];
  char temp1Str[7];
  char temp2Str[7];

  if (debugMode) Serial.println("Made it to Command_LOG_REQ 4");

  dtostrf(hum1, 6, 2, hum1Str);
  dtostrf(temp1, 6, 2, temp1Str);
  dtostrf(hum2, 6, 2, hum2Str);
  dtostrf(temp2, 6, 2, temp2Str);

  if (debugMode) Serial.println("Made it to Command_LOG_REQ 5");

  // format the information into a string using sprintf
  sprintf(logStr, "%s  | %s | (%03d, %03d, %03d) | (%03d, %03d, %03d) |%s%%     |%s%%     |%s\xB0""C       |%s\xB0""C       | ", date, time, r1, g1, b1, r2, g2, b2, hum1Str, hum2Str, temp1Str, temp2Str);
  Serial.print(logStr);
}

// turn on the auto day and night cycle checking
void Command_RTC_BEG(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // get the time arguments from the user (HH:MM)
  char* startStr = argv[2];
  char* stopStr = argv[3];

  // split the start time into hours and minutes
  char startHours[3];
  char startMinutes[3];
  split_time(startStr, startHours, startMinutes);

  // split the stop time into hours and minutes
  char stopHours[3];
  char stopMinutes[3];
  split_time(stopStr, stopHours, stopMinutes);

  // convert hours and minutes to total minutes integer
  dayStart[box] = atoi(startHours) * 60 + atoi(startMinutes);
  dayStop[box] = atoi(stopHours) * 60 + atoi(stopMinutes);

  // tell the main loop to start checking the time
  startCheck[box] = true;

  // add a log entry with the command we executed
  Command_LOG_REQ();
  echo_command_args(argc, argv);

  // ack the start and stop times
  if (debugMode) {
    Serial.print("RTC: setting day and night times for box ");
    Serial.println(box + 1);
    Serial.print("RTC: white LED (day) starts at ");
    Serial.print(startHours);
    Serial.print(":");
    Serial.println(startMinutes);
    Serial.print("RTC: red LED (night) starts at ");
    Serial.print(stopHours);
    Serial.print(":");
    Serial.println(stopMinutes);
  }
}

// turn off the auto day and night cycle checking
void Command_RTC_END(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // signal the void loop to stop checking
  startCheck[box] = false;

  // reset our first iteration flag for next time
  firstCheck[box] = true;

  // // turn off the LED
  // for (int i = 0; i < NUM_LEDS; ++i) {
  //   leds[box][i].setRGB(0, 0, 0);
  // }
  // FastLED.show();

  // turn off the LED
  int _argc = 5;
  char* boxStr = (box == 0) ? strdup("0") : strdup("1");
  char* _argv[_argc] = { "LED_RGB", boxStr, "0", "0", "0" };
  Command_LED_RGB(_argc, _argv);

  // add a log entry with the command we executed
  Command_LOG_REQ();
  echo_command_args(argc, argv);

  // prompt the user
  if (debugMode) {
    Serial.print("RTC: turned off day and night cycles for box ");
    Serial.println(box + 1);
  }
}

// change RGB values of LED
void Command_LED_RGB(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // ensure RGB arguments are byte sized
  uint8_t R = constrain(atoi(argv[2]), 0, 255);
  uint8_t G = constrain(atoi(argv[3]), 0, 255);
  uint8_t B = constrain(atoi(argv[4]), 0, 255);

  // change all of the LED colors
  for (int i = 0; i < NUM_LEDS; ++i) {
    leds[box][i].setRGB(R, G, B);
  }

  // update all of the changes we just made
  FastLED.show();

  // add a log entry with the command we executed
  Command_LOG_REQ();
  echo_command_args(argc, argv);

  // prompt the user
  if (debugMode) {
    Serial.print("LED: strip ");
    Serial.print(box + 1);
    Serial.print(" color set to RGB values (");
    Serial.print(R);
    Serial.print(", ");
    Serial.print(G);
    Serial.print(", ");
    Serial.print(B);
    Serial.println(")");
  }
}

// command function to set brightness for a specific LED strip
void Command_LED_INT(int argc, char* argv[]) {

  // parse the arguments
  brightnessVal = constrain(atoi(argv[1]), 0, 100);
  FastLED.setBrightness(brightnessVal);

  // update all of the changes we just made
  FastLED.show();

  // add a log entry with the command we executed
  Command_LOG_REQ();
  echo_command_args(argc, argv);

  // prompt the user
  if (debugMode) {
    Serial.print("LED: brightness set to ");
    Serial.println(brightnessVal);
  }
}

// turn off the LED
void Command_LED_OFF(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // set LED to black
  for (int i = 0; i < NUM_LEDS; ++i) {
    leds[box][i].setRGB(0, 0, 0);
  }

  // update all of the changes we just made
  FastLED.show();

  // add a log entry with the command we executed
  Command_LOG_REQ();
  echo_command_args(argc, argv);

  // prompt the user
  if (debugMode) {
    Serial.print("LED: turned off strip ");
    Serial.println(box + 1);
  }
}

// resets the RTC clock time to last flash
void Command_RTC_RST(int argc, char* argv[]) {
  
  // prompt the user, then reset the date and time
  if (debugMode) Serial.println("RTC: resetting RTC time");
  rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
}
