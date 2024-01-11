const uint8_t pinLED = LED_BUILTIN;

// constant definitions
#define BAUD_RATE 115200        // (bits/second)        serial buffer baud rate
#define BUFF_SIZE 64            // (bytes)              maximum message size
#define MSG_TIMEOUT 1000        // (milliseconds)       timeout from last character received
#define NUM_CMDS 2              // (positive integer)   number of commands in the command table struct

// buffer to hold received messages
uint8_t buff[BUFF_SIZE];

// global variables
bool debugMode = false;         // enables useful info for debugging code
unsigned long lastCharTime;     // used to timeout a message that is cut off or too slow

// custom data type that is a pointer to a command function
typedef void (*CmdFunc)();

// command structure
typedef struct {
  char *commandString;          // command string (e.g. "LED_ON", "LED_OFF"; use caps for alpha characters)
  CmdFunc commandFunction;      // pointer to the function that will execute if this command matches
} CmdStruct;

// prototypes of command functions to be used in command table below
void Command_LED_ON(void);
void Command_LED_OFF(void);

// command table
CmdStruct cmdTable[NUM_CMDS] = {

  {
  .commandString = "LED_ON",            // capitalized command for turning the LED on
  .commandFunction = &Command_LED_ON    // if "LED_ON" is received, run Command_LED_ON
  },

  {
  .commandString = "LED_OFF",           // capitalized command for turning the LED off
  .commandFunction = &Command_LED_OFF   // if "LED_OFF" is received, run Command_LED_OFF
  }

};



/******************************************************************************************
 ************************************   Setup & Loop   ************************************
 ******************************************************************************************/



void setup(void) {

  Serial.begin(BAUD_RATE);
  pinMode(pinLED, OUTPUT);
  digitalWrite(pinLED, LOW);

}

void loop(void) {

  receive_message();

}



/******************************************************************************************
 **********************************   Helper Functions   **********************************
 ******************************************************************************************/



void receive_message(void) {

  // control variables
  uint8_t rc;                 // stores byte from serial
  static uint8_t idx = 0;     // keeps track of which byte we are on

  // check for a timeout if we've received at least one character
  if (idx > 0) {
      // ignore message and reset index if we exceed timeout
      if ((millis() - lastCharTime) > MSG_TIMEOUT) {
        idx = 0;
        Serial.println("Message timeout...");
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

void process_message(void) {

  // walk through command table to search for message match
  for (int i = 0; i < NUM_CMDS; ++i) {
    // if received message matches the null-terminated command string
    if (strcmp(buff, cmdTable[i].commandString) == 0) {
        // execute that command function and leave
        cmdTable[i].commandFunction();
        return;
    }
  }

  // send user response if we did not find a match in the command table
  Serial.print("Unrecognized command: "); Serial.println((char *)buff);

}



/******************************************************************************************
 *********************************   Command Functions   **********************************
 ******************************************************************************************/



// turn on the LED and send a message
void Command_LED_ON(void) {

    Serial.println("LED on");
    digitalWrite(pinLED, HIGH);
        
}

// turn off the LED and send a message
void Command_LED_OFF(void) {

    Serial.println("LED off");
    digitalWrite(pinLED, LOW);
    
}