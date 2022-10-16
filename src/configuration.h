const char* SKETCH_VERSION = "v221016.0";

#define TELNET_DEBUG                               // Stream debug statements to UDP Telnet if defined

const char* default_ssid = "<Default SSID>";       // SSID
const char* default_password = "<Default PWD>";    // PSK
const char* mqtt_server = "<MQTT Broker IP>";      // MQTT Broker IP address
const char* mqtt_pwd = "<MQTT PWD>";               // MQTT Broker password

// Pins
// (default GPIO 21)                    // - Sensor SDA (SDI) -> ESP32.SDA  
// (default GPIO 22)                    // - Sensor SCL (SCK) -> ESP32.SCL  
const int pin_RPWM = 25;                // PWM output pin 25  -> connect to IBT-2 pin 1 (RPWM)
const int pin_LPWM = 26;                // PWM output pin 26  -> connect to IBT-2 pin 2 (LPWM)
const int pin_REN = 14;                 // DO output pin 14   -> connect to IBT-2 pin 3 (R_EN)
const int pin_LEN = 27;                 // DO output pin 27   -> connect to IBT-2 pin 4 (L_EN)
const int pin_iSense = 32;              // ADC input pin 23   -> connect to IBT-2 pins 5 & 6 (R_IS + L_IS) + 10k to ground.
const int pin_MotorRotations = 13;      // Motor Rotation Pulse counter (Hall sensor or other switch pulled down per count)
const int pin_BtnOpen = 18;             // DI input pin 18  -> Button for manual blinds OPEN (up)
const int pin_BtnClose = 19;            // DI input pin 19  -> Button for manual blinds CLOSE (down)
const int pin_StopOpen = 16;            // DI input pin 16. -> Limit Switch OPEN (top reached)
const int pin_StopClosed = 17;          // DI input pin 17  -> Limit Switch CLOSED (bottom reached)
//const int pin_StopOpenMid = 4?;       // DI input pin ?.  -> Limit Switch 3. (middle)             Currently not used.
const int pin_Buzzer = 5;               // DO output pin 5. -> Active Buzzer.

const int pwmResolution = 8;            // PWM frequency resolution
const int pwmFrequency = 20000;         // PWM frequency (in Hz)
const int pwmChannel_Open = 0;          // Channel for OPEN (RIGHT) PWM timer
const int pwmChannel_Close = 1;         // Channel for DOWN (LEFT) PWM timer
const int wifiMaxRetry = 10;            // Number of times to try and reconnect WiFi, per call.
const int mqttMaxRetry = 2;             // Number of times to try and reconnect MQTT.
const int currentSenseInterval = 200;   // Interval between current sense checks to prevent overcurrent. (milliseconds) 

const int BleepTimeOn = 80;             // Buzzer "on" duration
const int BleepTimeOff = 110;           // Buzzer "off" duration

const int luxLowLevelThreshold = 25;    // Report Lux level with each time interval when it starts to get dark.

enum blindsAction {actUNDEF, actBlindsOpen, actBlindsClose, actBlindsStop};
enum actionOwner {ownUNDEF, ownMQTT, ownButton, ownLimit};

/* Naming Convention
 *  btn  -> Button
 *  mtr  -> Motor
 *  lmt  -> Limit switch
 *  tmr  -> Timer (interrupt)
 *  sem  -> Semaphore
 *  tsk  -> Task (handle)
 *  lux  -> Lux sensor
 *  tmp  -> Temperature sensor
*/

#define MQTT_PUB_BLINDSSTATE    "livingroom/blinds/state"           // PUBLISH: current Blinds state                    (open/closed + %)
#define MQTT_PUB_CONFIG         "livingroom/blinds/config"          // PUBLISH: configuration settings                  (JSON settings)
#define MQTT_PUB_APPSTATE       "livingroom/blinds/app_state"       // PUBLISH: telemetry metrics                       (JSON parameters)
#define MQTT_PUB_LUX            "livingroom/lightlevel/state"       // PUBLISH: current Lux reading                     (value)
#define MQTT_PUB_TEMP           "livingroom/temperature/state"      // PUBLISH: current temperate reading               (value)
#define MQTT_PUB_HUMIDITY       "livingroom/humidity/state"         // PUBLISH: current humidity reading                (value)

#define MQTT_SUB_BLINDSACTION   "livingroom/blinds/action"          // SUBSCRIBE: blinds action (open/close/stop)
#define MQTT_SUB_APPCMD         "livingroom/blinds/appcmd"          // SUBSCRIBE: app configuration and action commands
#define MQTT_SUB_NOTIFY         "all/notify/bleep"                  // SUBSCRIBE: string pattern to beep the buzzer

struct BlindsAction {
  volatile bool NewAction;                        // New/unprocessed action flag. E.g. from MQTT
  volatile blindsAction Action;                   // Requested action to perform.
};

struct Button {
  volatile bool Changed;                          // Button status changed (pressed/released).
  volatile unsigned long lastDebounceTime;        // Timestamp when button status changed. Used for debouncing.
  volatile unsigned long lastStopTime;            // Timestamp of last time that button stopped motor.
};

struct Switch {
  volatile bool Set;                              // The Limit Switch is set (don't use open/close naming to avoid confusion)
  volatile unsigned long lastDebounceTime;        // Timestamp when limit switch was closed. Used for debouncing.
};

struct Motor {
  volatile bool AllowToRun;                       // Allow motor to start if not running. Stop motor if currently running.
  volatile bool IsRunning;                        // Indication if motor is currently running, or stopped.
  volatile int targetPosition;                    // Position to where blinds must go.
  volatile int currentPosition;                   // Position where blinds currently are. Based on motor axis rotations.
  volatile blindsAction Action;                   // Action to take when motor is started (open or close).
  volatile actionOwner Owner;                     // Who or What initiated the action.
};

struct Config {
  bool AllowRemoteControl;                        // Allow remote control (MQTT) of the blinds motor. (true/false)
  bool AllowRemoteBleep;                          // Allow buzzer bleep via MQTT. (true/false)
  int Lux_Interval;                               // Interval between LUX reporting. (minutes)
  int Lux_MinReportDelta;                         // Minimum change from previous upload to report Lux levels
  int Temp_Interval;                              // Interval between Temperature feedback (minutes)
  int State_Interval;                             // Interval between State feedback (minutes) 
  int DebounceDurSwitches;                        // Debounce time for Button and Limit switches
  int DebounceDurMotor;                           // Debounce time for motor rotation switch
  bool RotationLimits;                            // Blinds considered open/closed based on rotation count. Else open/closed at limit switches.
  int Open_Duration;                              // How long to allow motor to run when opening blinds (seconds)
  int Open_MaxRotations;                          // How many motor axis rotations before blinds are fully open
  int MaxCurrentLimit;                            // Maximum current motor can draw before stopped (raw analog reading)
  int MaxRunDuration;                             // Maximum time that motor can run in any direction (seconds). (prevents running forever when e.g. the blinds cord snaps)
  char* SSID;                         			      // WLAN SSID
  char* Password;                     			      // WLAN password
};
