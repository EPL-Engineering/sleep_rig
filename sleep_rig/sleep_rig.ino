// TODO
// - Make sure the manual LED controls interface correctly with the automatic ones
// - Desolder RESET-EN
// - Create a GUI for easy use with LabView
// - Debug redirects outputs to text file
// - Control two different LED strips
// - Execute commands based on argc and argv not the structure

#include <FastLED.h>
#include <RTClib.h>
#include <Wire.h>
#include <EEPROM.h>

// constant macros
#define BAUD_RATE                   115200      // (bits/second)          serial buffer baud rate
#define BUFF_SIZE                   64          // (bytes)                maximum message size
#define MSG_TIMEOUT                 1000        // (milliseconds)         timeout from last character received
#define NUM_CMDS                    6           // (positive integer)     number of commands in the command table struct

#define NUM_BOXES                   2
#define LED_PIN_1                   5
#define LED_PIN_2                   6
#define NUM_LEDS                    1
#define LED_TYPE                    WS2812B
#define COLOR_ORDER                 GRB
#define UPDATES_PER_SECOND          100

// constant variables
const char* DAYS_OF_THE_WEEK[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

// memory block storage locations
uint8_t buff[BUFF_SIZE];          // message buffer
CRGB leds[NUM_BOXES][NUM_LEDS];   // LED manipulation for booths
RTC_PCF8523 rtc;                  // define the RTC object

// general global variables
bool debugMode = false;           // print serial output to text file (defective)
unsigned long lastCharTime;       // used to timeout a message that is cut off or too slow

// LED global variables
uint8_t brightnessVal = 1;        // default brightness for boxes (can be adjusted with LED_RGB)

// RTC box 1 global variables
bool prevIsDay[NUM_BOXES] = {false, false};     // keep track of the last check for comparison to next check
bool startCheck[NUM_BOXES] = {false, false};    // user defined start variable for checking day and night cycles
bool firstCheck[NUM_BOXES] = {true, true};      // some things only need to be done on the first iteration
uint8_t dayStart[NUM_BOXES] = {8, 8};           // user defined day time start (white light)
uint8_t dayStop[NUM_BOXES] = {18, 18};          // uesr defined night time start (red light)

// custom data type that is a pointer to a command function
typedef void (*CmdFunc)(int argc, char* argv[]);

// command structure
typedef struct {
  int commandArgs;                // number of command line arguments including the command string
  char* commandString;            // command string (e.g. "LED_ON", "LED_OFF"; use caps for alpha characters)
  CmdFunc commandFunction;        // pointer to the function that will execute if this command matches
} CmdStruct;

void Command_RTC_DT(int argc = 0, char* argv[] = {NULL});
void Command_RTC_ON(int argc = 0, char* argv[] = {NULL});
void Command_RTC_OFF(int argc = 0, char* argv[] = {NULL});
void Command_LED_RGB(int argc = 0, char* argv[] = {NULL});
void Command_LED_INT(int argc = 0, char* argv[] = {NULL});
void Command_LED_OFF(int argc = 0, char* argv[] = {NULL});

// command table
CmdStruct cmdTable[NUM_CMDS] = {

  {
    .commandArgs = 1,                         // RTC_DT
    .commandString = "RTC_DT",                // capitalized command for getting global date and time
    .commandFunction = &Command_RTC_DT        // run Command_RTC_DT
  },

  {
    .commandArgs = 4,                         // RTC_ON <boxNum> <dayStart> <dayStop>
    .commandString = "RTC_ON",                // capitalized command for starting the RTC check
    .commandFunction = &Command_RTC_ON        // run Command_RTC_ON
  },

  {
    .commandArgs = 2,                         // RTC_OFF <boxNum>
    .commandString = "RTC_OFF",               // capitalized command for stopping the RTC check
    .commandFunction = &Command_RTC_OFF       // run Command_RTC_OFF
  },

  {
    .commandArgs = 5,                         // LED_RGB <boxNum> <R> <G> <B>
    .commandString = "LED_RGB",               // capitalized command for adjusting LED color
    .commandFunction = &Command_LED_RGB       // run Command_LED_RGB
  },

  {
    .commandArgs = 2,                         // LED_INT <INT>
    .commandString = "LED_INT",               // capitalized command for adjusting LED brightness
    .commandFunction = &Command_LED_INT       // run Command_LED_INT
  },

  {
    .commandArgs = 2,                         // LED_OFF <boxNum>
    .commandString = "LED_OFF",               // capitalized command for turning the LED off
    .commandFunction = &Command_LED_OFF       // run Command_LED_OFF
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
  while (Serial.available() > 0) {Serial.read();}
  Serial.println("*********BEGIN SETUP*********");
  
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
  Serial.println("LED: initialized successfully");

  // initialize the RTC
  if (!rtc.begin()) {
    Serial.println("RTC: couldn't initialize module... please restart!");
    while (true);
  }
  else {
    Serial.println("RTC: initialized successfully");
  }

  // check if the RTC lost power
  if (rtc.lostPower()) {
    Serial.println("RTC: lost power, setting the date and time...");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  // let the user know the date and time on startup
  Command_RTC_DT();

  // separate commands with new line
  Serial.println("**********END SETUP**********");

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
  uint8_t rc;                 // stores byte from serial
  static uint8_t idx = 0;     // keeps track of which byte we are on

  // check for a timeout if we've received at least one character
  if (idx > 0) {
      // ignore message and reset index if we exceed timeout
      if ((millis() - lastCharTime) > MSG_TIMEOUT) {
        idx = 0;
        Serial.println("SYS: Message timeout...");
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
    }
    else {
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
            Serial.print("SYS: not enough args for '");
            Serial.print(argv[0]);
            Serial.println("'");
            return;
          }
          // store if it's provided
          argv[j] = token;
        }
        // try to get another argument (should be done already)
        token = strtok(NULL, " ");
        // check if there is too many arguments
        if (token != NULL) {
            Serial.print("SYS: too many args for '");
            Serial.print(argv[0]);
            Serial.println("'");
            return;
        }
        // echo what were about to execute
        echo_command_args(argc, argv);
        // execute the command and pass any arguments
        cmdTable[i].commandFunction(argc, argv);
        return;
      }
    }
  }

  // send user response if we did not find a match in the command table
  Serial.print("SYS: unrecognized command '");
  Serial.print(token);
  Serial.println("'");

}

// update day and night cycle
void update_day_night(int box) {

  // get the current time
  DateTime now = rtc.now();
  int currentHour = now.hour();

  // determine if it's day or night
  bool isDay = (currentHour >= dayStart[box] && currentHour < dayStop[box]);

  // only prompt user if we changed from the last day check
  if (firstCheck[box] || isDay != prevIsDay[box]) {
    // store the previous cycle day time check
    prevIsDay[box] = isDay;
    // set the LED color to white during the day
    if (isDay) {
      // prompt the user of date and time
      Serial.println("RTC: It is now day time...");
      Command_RTC_DT();
      // manually send an RGB command
      int argc = 5;
      char* boxStr = (box == 0) ? strdup("0") : strdup("1");
      char* argv[argc] = {"LED_RGB", boxStr, "255", "255", "255"};
      Command_LED_RGB(argc, argv);
    }
    // set the LED color to red during the night
    else {
      // prompt the user of date and time
      Serial.println("RTC: It is now night time...");
      Command_RTC_DT();
      // manually send an RGB command
      int argc = 5;
      char* boxStr = (box == 0) ? strdup("0") : strdup("1");
      char* argv[argc] = {"LED_RGB", boxStr, "255", "0", "0"};
      Command_LED_RGB(argc, argv);
    }
    // it's not the first check anymore
    firstCheck[box] = false;
  }

}

// echos the command back to the user via serial
void echo_command_args(int argc, char* argv[]) {

  Serial.print("SYS: ");
  for (int i = 0; i < argc; ++i) {
    Serial.print(argv[i]);
    if (i < argc - 1) {
      Serial.print(" ");
    }
  }
  Serial.println();

}



/******************************************************************************************
 *********************************   Command Functions   **********************************
 ******************************************************************************************/



// print the date and time to serial monitor
void Command_RTC_DT(int argc, char* argv[]) {

  DateTime now = rtc.now();
  Serial.print("RTC: ");
  Serial.print(now.month(), DEC);
  Serial.print('/');
  Serial.print(now.day(), DEC);
  Serial.print('/');
  Serial.print(now.year(), DEC);
  Serial.print(" (");
  Serial.print(DAYS_OF_THE_WEEK[now.dayOfTheWeek()]);
  Serial.print(") ");
  Serial.print(now.hour(), DEC);
  Serial.print(':');
  Serial.print(now.minute(), DEC);
  Serial.print(':');
  Serial.print(now.second(), DEC);
  Serial.println();

}

// turn on the auto day and night cycle checking
void Command_RTC_ON(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // get the day time arguments from the user
  dayStart[box] = atoi(argv[2]);
  dayStop[box] = atoi(argv[3]);

  // ack the start and stop times
  Serial.print("RTC: setting day and night times for box ");
  Serial.println(box);
  Serial.print("RTC: white LED (day) starts at hour ");
  Serial.println(dayStart[box]);
  Serial.print("RTC: red LED (night) starts at hour ");
  Serial.println(dayStop[box]);

  // tell the main loop to start checking the time
  startCheck[box] = true;

}

// turn off the auto day and night cycle checking
void Command_RTC_OFF(int argc, char* argv[]) {

  // select which box
  int box = atoi(argv[1]);

  // signal the void loop to stop checking
  startCheck[box] = false;

  // reset our first iteration flag for next time
  firstCheck[box] = true;

  // turn off the LED
  int _argc = 2;
  char* _argv[_argc] = {"LED_OFF", argv[1]};
  Command_LED_OFF(_argc, _argv);

  // prompt the user
  Serial.print("RTC: turned off day and night cycles for box ");
  Serial.println(box);

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

  // prompt the user
  Serial.print("LED: strip ");
  Serial.print(box);
  Serial.print(" color set to RGB values (");
  Serial.print(R);
  Serial.print(", ");
  Serial.print(G);
  Serial.print(", ");
  Serial.print(B);
  Serial.println(")");

}

// command function to set brightness for a specific LED strip
void Command_LED_INT(int argc, char* argv[]) {

    // parse the arguments
    brightnessVal = constrain(atoi(argv[1]), 0, 255);
    FastLED.setBrightness(brightnessVal);

    // update all of the changes we just made
    FastLED.show();

    // prompt the user
    Serial.print("LED: brightness set to ");
    Serial.println(brightnessVal);

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

  // prompt the user
  Serial.print("LED: turned off strip ");
  Serial.println(box);

}