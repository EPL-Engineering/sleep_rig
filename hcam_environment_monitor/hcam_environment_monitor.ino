// TODO

#include <FastLED.h>
#include <RTClib.h>
#include <Wire.h>
#include <DHT.h>
#include <stdio.h>
#include <string.h>

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
#define NUM_LEDS 26
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define UPDATES_PER_SECOND 100

RTC_PCF8523 rtc;                 // define the RTC object
CRGB leds[NUM_BOXES][NUM_LEDS];  // LED manipulation for booths
uint8_t buff[BUFF_SIZE];         // message read buffer
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

// struct to send sensor data packet over serial 128 bytes
typedef struct {
  char date[11];
  char time[9];
  uint8_t r1, g1, b1;  // RGB values for LED 1
  uint8_t r2, g2, b2;  // RGB values for LED 2
  float temp1;
  float temp2;
  float hum1;
  float hum2;
  char cmdName[86]; // LED_RGB 0 255 255 255 uses 23 bytes max (largest command size)
} SensorData;



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

  // initialize the RTC
  if (!rtc.begin()) while (true);
  // rtc.begin();

  // check if the RTC lost power
  if (!rtc.initialized() || rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  // ensure rtc is running by clearing the stop bit (stayed connected to battery but stopped)
  rtc.start();

  write_log("SER_CON");
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
            return;
          }
          // store if it's provided
          argv[j] = token;
        }
        // try to get another argument (should be done already)
        token = strtok(NULL, " ");
        // check if there is too many arguments
        if (token != NULL) {
          return;
        }
        // execute the command and pass any arguments
        cmdTable[i].commandFunction(argc, argv);
        // convert the command and arguments to string
        char cmdName[86];
        strcpy(cmdName, echo_command_args(argc, argv));
        // write log to serial line
        write_log(cmdName);
        // ok get out of here now
        return;
      }
    }
  }
}

// update day and night cycle
void update_day_night(int box) {

  // get the current time
  DateTime now = rtc.now();
  int currentMinute = now.hour() * 60 + now.minute();

  // determine if it's day or night
  bool isDay;

  // check if cycle is reversed (stop comes before start)
  if (dayStop[box] < dayStart[box]) {
    isDay = !(currentMinute >= dayStop[box] && currentMinute < dayStart[box]);
  }
  // check if cycle is normal (start comes before stop time)
  else {
    isDay = (currentMinute >= dayStart[box] && currentMinute < dayStop[box]);
  }

  // only prompt user if we changed from the last day check
  if (firstCheck[box] || isDay != prevIsDay[box]) {
    // store the previous cycle day time check
    prevIsDay[box] = isDay;
    // set the LED color to white during the day
    if (isDay) {
      // manually set the RGB
      int argc = 5;
      char* boxStr = (box == 0) ? strdup("0") : strdup("1");
      char* argv[argc] = { "LED_RGB", boxStr, "255", "255", "255" };
      Command_LED_RGB(argc, argv);
    }
    // set the LED color to red during the night
    else {
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

// echos the command back to the user
char* echo_command_args(int argc, char* argv[]) {

  // allocate memory for the echoed string
  static char cmdName[86] = "";

  // prepare the string to be passed to cmdName in SensorData
  strcpy(cmdName, argv[0]);  // copy command name first

  // concatenate arguments
  for (int i = 1; i < argc && strlen(cmdName) + strlen(argv[i]) + 2 < sizeof(cmdName); ++i) {
    strcat(cmdName, " ");      // append space
    strcat(cmdName, argv[i]);  // append argument
  }

  // ensure null termination
  cmdName[sizeof(cmdName) - 1] = '\0';

  // return the command name and arguments
  return cmdName;
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

void write_log(char* cmdArgsString) {

  // packet to send over serial
  SensorData pkt;

  // date and time string buffer
  DateTime now = rtc.now();
  snprintf(pkt.date, sizeof(pkt.date), "%02d/%02d/%04d", now.month(), now.day(), now.year());
  snprintf(pkt.time, sizeof(pkt.time), "%02d:%02d:%02d", now.hour(), now.minute(), now.second());

  // get the LED information
  CRGB color1 = leds[0][0];
  pkt.r1 = color1.r;
  pkt.g1 = color1.g;
  pkt.b1 = color1.b;
  CRGB color2 = leds[1][0];
  pkt.r2 = color2.r;
  pkt.g2 = color2.g;
  pkt.b2 = color2.b;

  // get the climate information
  pkt.temp1 = dhtSensors[0].readTemperature();
  pkt.temp2 = dhtSensors[1].readTemperature();
  pkt.hum1 = dhtSensors[0].readHumidity();
  pkt.hum2 = dhtSensors[1].readHumidity();

  // send 0.0 float instead of nan
  if (isnan(pkt.temp1)) pkt.temp1 = 0.0;
  if (isnan(pkt.temp2)) pkt.temp2 = 0.0;

  // echo the command we executed
  strncpy(pkt.cmdName, cmdArgsString, sizeof(pkt.cmdName));

  // send the log data packet over serial
  Serial.write(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));

  // print the log data to the serial monitor
  // Serial.print("Date: ");
  // Serial.print(pkt.date);
  // Serial.print(" | Time: ");
  // Serial.print(pkt.time);
  // Serial.print(" | LED 1 (R,G,B): (");
  // Serial.print(pkt.r1);
  // Serial.print(",");
  // Serial.print(pkt.g1);
  // Serial.print(",");
  // Serial.print(pkt.b1);
  // Serial.print(") | LED 2 (R,G,B): (");
  // Serial.print(pkt.r2);
  // Serial.print(",");
  // Serial.print(pkt.g2);
  // Serial.print(",");
  // Serial.print(pkt.b2);
  // Serial.print(") | Temp 1: ");
  // Serial.print(pkt.temp1);
  // Serial.print(" | Temp 2: ");
  // Serial.print(pkt.temp2);
  // Serial.print(" | Humidity 1: ");
  // Serial.print(pkt.hum1);
  // Serial.print(" | Humidity 2: ");
  // Serial.print(pkt.hum2);
  // Serial.print(" | Command: ");
  // Serial.println(pkt.cmdName);
}



/******************************************************************************************
 *********************************   Command Functions   **********************************
 ******************************************************************************************/



// send log entry to serial when requested
void Command_LOG_REQ(int argc, char* argv[]) {
  // just needs to be called successfully to send unprompted log entry
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

  
  // convert hours and minutes to total minutes integer
  int newDayStart = atoi(startHours) * 60 + atoi(startMinutes);
  int newDayStop = atoi(stopHours) * 60 + atoi(stopMinutes);

  // set the globals if good
  dayStart[box] = newDayStart;
  dayStop[box] = newDayStop;

  // tell the main loop to start checking the time
  startCheck[box] = true;
}

// turn off the auto day and night cycle checking
void Command_RTC_END(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // signal the void loop to stop checking
  startCheck[box] = false;

  // reset our first iteration flag for next time
  firstCheck[box] = true;

  // set LED to black
  for (int i = 0; i < NUM_LEDS; ++i) {
    leds[box][i].setRGB(0, 0, 0);
  }

  // update all of the changes we just made
  FastLED.show();
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
}

// command function to set brightness for a specific LED strip
void Command_LED_INT(int argc, char* argv[]) {

  // parse the arguments
  brightnessVal = constrain(atoi(argv[1]), 0, 100);
  FastLED.setBrightness(brightnessVal);

  // update all of the changes we just made
  FastLED.show();
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
}
