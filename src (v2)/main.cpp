/************************************************************************************************************ 
 *  ESP32_BlindsControl
 *  - Board Type:  Espressif DevKit C
 *  - Fritzing:    ESP32_DevKitC-BlindsControl.fzz 
 * 
 *  Functionality
 *  - when "Up" button pressed OR "Open" MQTT cmd received, motor runs clockwise/right.
 *  - when "Down" button pressed OR "Close" MQTT cmd received, motor runs anticlockwise/left.
 *  - when "Up/Down" button released OR "Open/Closed" limit switch triggered, motor stops.
 *  - run motor based on MQTT messages received (e.g. from Home Assistant).
 *  - motor stops if
 *    - a limit switch is triggered.
 *    - a limit is exceeded (e.g. max current, max open/close duration - configurable).
 *  - at regular intervals, get the light level and publish on MQTT.
 *  - at regular intervals, get the temperature and publish on MQTT.
 *  - update any status change via MQTT (to Home Assistant).
 *  - configuration settings received via MQTT, and stored in SPIFS.
 *  
 *  Concepts
 *  - Interrupts (Butttons on Pins 16 & 17, limit switches on 18, 19)
 *  - PWM (driving IBT-2 on Pins 25, 26)  
 *  - Multitasking - running a dedicated loop task for motor actions (also for OTA)
 *  - Lux sensor (TSL2561)
 *  - Temperature sensor (AM2320)
 *  - Supports OTA updates                  : (laptop network must be set to private)
 *  - UDP Telnet debugging                  : e.g. "telnet 192.168.2.208"
 * 
 *  Wiper Motor Connection:
 *  - Negative:         31                  : Also wiper motor body!
 *  - Positive (1):     53                  : 36rpm,   1.3A (no load) 
 *  - Positive (2):     53b-                : 54rpm,   2.2A (no load) 
 *  - Rotation Switch   ?                   : Connection 1 of wiper motor rotor slip switch
 *  - Rotation Switch   ?                   : Connection 2 of wiper motor rotor slip switch
 *  
 * ------------------------------------
 * MQTT Messages
 * - Subscribed:
 *   - "livingroom/blinds/action"
 *      -> open:<value>                     : open the Blinds to the indicated percentage.
 *      -> close                            : close the Blinds if they are not closed already.
 *      -> stop                             : stop the Blinds if the motor is currently running.
 *   - "livingroom/blinds/appcmd" 
 *      -> restart                          : restart ESP32
 *      -> getstate                         : report the current state and telemetry values (RSSI, Memory, ..)
 *      -> getconfig                        : report the current application configuration
 *      -> StateInterval:<minutes>          : set the interval between state updates (0 = disabled)
 *      -> LuxInterval:<minutes>            : set the interval between Lux updates (0 = disabled)
 *      -> TempInterval:<minutes>           : set the interval between Temperature updates (0 = disabled)
 *      -> RotationLimits:<true/false>      : set if blinds is considered open/closed on rotations (true) or at limit switches (false) 
 *      -> ClosedRotationOffset:<count>     : set additional rotations motor will do if CLOSE rotation count is reached (count) 
 *      -> DebounceDurSwitches:<mseconds>   : set the debounce time for Buttons and Limit switches (milliseconds)
 *      -> DebounceDurMotor:<mseconds>      : set the debounce time for the motor rotation switch (milliseconds)
 *      -> OpenDuration:<seconds>           : set max duration the motor will run when OPENING the blinds (0 = check and timer disabled)
 *      -> MaxRunDuration:<seconds>         : set max duration the motor may run in ANY direction (0 = check and timer disabled)
 *      -> MaxOpenRotations:<count>         : set max number of axis rotations that blinds can open (0 = disabled)
 *      -> MinLuxReportDelta:<lux>          : set the minimum difference in Lux level before publishing MQTT (0 = no threshold, interval only)
 *      -> MaxCurrentLimit:<value>          : set max load current motor is allowed to draw (raw analog value) (0 = disabled)
 *      -> AllowRemoteControl:<true/false>  : set control Blinds using MQTT (true), else (false)
 *      -> AllowRemoteBleep:<true/false>    : set if Bleep notifications must be processed (true) or ignored (false)
 *      -> WiFiSetup:SSID/password          : set the SSID and password to be used ("default" for defaults)
 *   - "all/notify/bleep"                   : if enabled, sound buzzer based on provided value
 * 
 * - Published:
 *   - "livingroom/blinds/state"            : publish current Blinds state                    (open/closed + %)
 *   - "livingroom/blinds/config"           : publish configuration settings                  (JSON settings)
 *   - "livingroom/blinds/app_state"        : publish telemetry metrics                       (JSON parameters)
 *   - "livingroom/lux/state"               : publish current Lux reading                     (value)
 *   - "livingroom/temperature/state"       : publish current temperate reading               (value)
 *   - "livingroom/humidity/state"          : publish current humidity reading                (value)
 * ------------------------------------
 *
 *  TODO
 * 
 ***********************************************************************************************************/ 
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <AM2320.h>
#include <BH1750.h>
#include <ArduinoJson.h>
#include <soc/rtc_cntl_reg.h>     // disable brownout problems
#include <rom/rtc.h>
#include <TelnetStream.h>
#include "OTA.h"
#include "configuration.h"

Preferences preferences;
WiFiClient espClient;
PubSubClient clientMQTT(espClient);
AM2320 th(&Wire);
BH1750 luxSensor;

TaskHandle_t taskLoopMotorActions;     // Task handle for the loop task that will do all the motor handling.
SemaphoreHandle_t semBlindsCheck;      // Semaphore for syncing tasks, to prevent reading/writing global variables at the same time.


Config appConfig;                                             // Config object for app configuration settings
Button btnBlindsOpen = {false, 0};                            // Button object for "Blinds OPEN"
Button btnBlindsClose = {false, 0};                           // Button object for "Blinds CLOSE"
Switch swcBlindsOpen = {false, 0};                            // LimitSwitch object for "Blinds OPENED"
Switch swcBlindsClosed = {false, 0};                          // LimitSwitch object for "Blinds CLOSED"
Motor mtrBlinds = {false, false, -1, -1, actUNDEF, ownUNDEF}; // Motor object
BlindsAction mqttBlindsAction = {false, actUNDEF};            // MQTT requested action
volatile bool actionStopMotor = false;                        // Stop motor flag. Set by e.g. limit switches, MQTT, button release, ..
volatile bool actionProcessMotorRotation = false;             // Motor rotation completion trigger flag.
bool mqttPublishBlindsState = false;                          // Flag for main loop to publish MQTT Open msg
unsigned long timeLastRotationDebounce = 0;                   // Timestamp when last axis rotation was triggered.
unsigned long timeClosePosReached = 0;                        // Timestamp when motor reached close position (based on rotation count)
int DoBleepTimes = 0;                                         // Let loop do bleep, initiated from e.g. interrupts. 

hw_timer_t * tmrBlindsOpen = NULL;
hw_timer_t * tmrBlindsMaster = NULL;
//hw_timer_t * tmrBlindsClosedDelay = NULL;
portMUX_TYPE muxTimer = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE muxButton = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE muxLimit = portMUX_INITIALIZER_UNLOCKED;

// Function forward declarations
void MotorStart();
void MotorStop();
void loop_MotorActions (void * parameter);
void Bleep (const String& BleepMsg);
void MyBleep(int NrBleeps);

/**************************************************************************
*  Timer Interrupt routine to stop motor after running for a maximum period.
*  - Safety measure to stop motor from running indefinately should something go wrong (e.g. cord breaks)
***************************************************************************/
void IRAM_ATTR isrTimerBlindsMaster() {
  Serial.println(" >>> Blinds Master Timer Interrupt: stop motor!");
  portENTER_CRITICAL_ISR(&muxTimer);
  actionStopMotor = true;                        // Set flag to stop the motor. Will be processed in motor loop.
  portEXIT_CRITICAL_ISR(&muxTimer);
}

/**************************************************************************
*  Timer Interrupt routine to stop motor after opening blinds for a certain period.
*  - Safety measure in case "fully open" limit switch does not work
***************************************************************************/
void IRAM_ATTR isrTimerBlindsOpen() {
  Serial.println(" >> Blinds Open Timer Interrupt: stop motor");
  if (mtrBlinds.Action == actBlindsOpen) {
    portENTER_CRITICAL_ISR(&muxTimer);
    actionStopMotor = true;                        // Set flag to stop the motor. Will be processed in motor loop.
    portEXIT_CRITICAL_ISR(&muxTimer);
  }
}

/**************************************************************************
*  Timer Interrupt routine to delay stopping motor after the close (rotation) position was reached.
*  - If delay is defined, motor must run a short period after the rotation count reached zero.
*  - The motor should then be stopped by the close limit switch, this is a safety measure to ensure motor stops anyway. 
***************************************************************************/
//void IRAM_ATTR isrTimerClosedPosDelay() {
//  Serial.println(" >> Blinds Close Position Delay Timer Interrupt: stop motor");
//  if (mtrBlinds.Action == actBlindsClose) {
//    portENTER_CRITICAL_ISR(&muxTimer);
//    actionStopMotor = true;                        // Set flag to stop the motor. Will be processed in motor loop.
//    portEXIT_CRITICAL_ISR(&muxTimer);
//  }
//}

/**************************************************************************
*  Interrupt routine for "OPEN" button change (triggered on both press and release).
***************************************************************************/
void IRAM_ATTR isrButtonBlindsOpen() {
  portENTER_CRITICAL_ISR(&muxButton);
  if ( (millis() - btnBlindsOpen.lastDebounceTime) > appConfig.DebounceDurSwitches) {
    // This is the first OPEN button press in some time, process the change. Else ignore.
    btnBlindsOpen.lastDebounceTime = millis();
    btnBlindsOpen.Changed = true;
  }
  portEXIT_CRITICAL_ISR(&muxButton);
}

/**************************************************************************
*  Interrupt routine for "CLOSE" button (triggered on both press and release).
**************************************************************************/
void IRAM_ATTR isrButtonBlindsClose() {
  portENTER_CRITICAL_ISR(&muxButton);
  if ( (millis() - btnBlindsClose.lastDebounceTime) > appConfig.DebounceDurSwitches) {
    // This is the first CLOSE button press in some time, process the change. Else ignore.
    btnBlindsClose.lastDebounceTime = millis();
    btnBlindsClose.Changed = true;
  }
  portEXIT_CRITICAL_ISR(&muxButton);
}  

/**************************************************************************
 *  Interrupt routine to count the motor axis rotations to determine blinds open position/percentage.
 *  (Interrupt is declared as "falling" i.e. when pulled down)
 *  This routine also works for a Hall sensor, with no need to debounce.
 *  For a wiper motor, the "internal" slip contacts can be used, but they must be debounced (give two triggers within a second).
 **************************************************************************/
void IRAM_ATTR isrMotorRotations() {

  if (appConfig.Open_MaxRotations > 0) {
    // Only care about rotation count if the max rotations is set.
    if ( (millis() - timeLastRotationDebounce) > appConfig.DebounceDurMotor) {
      // This is the first motor rotation trigger in some time. Process it, ignore any subsequent triggers for the debounce duration.

      actionProcessMotorRotation = true;

      timeLastRotationDebounce = millis();
    }
  }
}

/**************************************************************************
 *  reportTemperature
 *  - Read the temperature and humidity from AM2320
 *  - Report back using MQTT
 **************************************************************************/
void reportTemperature() {
  const int maxRetries = 10;
  float temperature = 0.0;
  float humidity = 0.0;
  int sensorStatus = 0;
  int readCount = 0;
  
  do {
    sensorStatus = th.Read();
    if (sensorStatus == 0) {

      temperature = th.cTemp;
      humidity = th.Humidity;
      Serial.printf(" - Temperature: (%f), Humidity (%f)\n", temperature, humidity ); 

      clientMQTT.publish(MQTT_PUB_TEMP, String(temperature).c_str() );
      clientMQTT.publish(MQTT_PUB_HUMIDITY, String(humidity).c_str());
      
    } else {
      //Serial.printf("\t - AM2320 error: %d (%d)\n", sensorStatus, readCount ); 
#ifdef TELNET_DEBUG
      TelnetStream.println(" ReportTemperature: - AM2320 error!");
#endif
      delay(100);
    }
    readCount++;
  } while (sensorStatus != 0 && readCount < maxRetries); // Loop until successful or max times tried
  //Serial.printf("reportTemp: status=%d retry=%d temp=%f\n", sensorStatus, readCount, temperature); 
}

/**************************************************************************
 *  reportLux
 *  - Read the light intensity from TSL2561
 *  - Publish value using MQTT
 **************************************************************************/
void reportLux() {
  static float luxLastReportedValue = 0;              // Last reported LUX reading (remember between calls)

  float luxValue = luxSensor.readLightLevel();
  if (luxValue > 0) { 
    if ( luxLastReportedValue == 0 || 
          (luxValue != luxLastReportedValue && luxValue <= luxLowLevelThreshold ) ||  
          abs(luxValue - luxLastReportedValue) >= appConfig.Lux_MinReportDelta ) {
      // First time since start, or 
      // Lux value changed since the previous read, and it is rather low (nearly dark), or
      // The light changed significantly since the previous report. Publish the new value through MQTT.
      Serial.print(" - Light Level report: ");  Serial.print( String(luxValue).c_str() ); Serial.println(" lux");      
#ifdef TELNET_DEBUG
    TelnetStream.print(" ReportLux: - Lux level="); TelnetStream.println(luxValue);
#endif
      luxLastReportedValue = luxValue;
      clientMQTT.publish(MQTT_PUB_LUX, String(luxValue).c_str());
    } 
//    else { Serial.printf(" - Lux: not reporting. Prev = %d, Cur = %d\n", luxLastReportedValue, luxValue );  }
  } else {
    Serial.println(" - Lux sensor reading error!");
#ifdef TELNET_DEBUG
    TelnetStream.print(" ReportLux: - Lux sensor reading error! lux="); TelnetStream.println(luxValue);
#endif
    // This typically happens when the lux value is below a level that the TSL2561 can read, so report 0 instead.
    // Only publish the 0 state once, until the problem is corrected.
    if ( luxLastReportedValue > 0 ) {
      luxLastReportedValue = 0;
      clientMQTT.publish(MQTT_PUB_LUX, String(luxLastReportedValue).c_str());
    }
  }
}

/**************************************************************************
 * getRestartReason
 * - Get last ESP32 reset reason.
 **************************************************************************/
void getRestartReason(char* Reason, int rsnLength) {

  int rreason = (int) rtc_get_reset_reason( (RESET_REASON) 0);

  switch (rreason)
  {
    case 1 : strncpy(Reason, "POWERON_RESET", rsnLength);break;             // - 1,  Vbat power on reset
    case 3 : strncpy(Reason, "SW_RESET", rsnLength);break;                  // - 3,  Software reset digital core
    case 4 : strncpy(Reason, "OWDT_RESET", rsnLength);break;                // - 4,  Legacy watch dog reset digital core
    case 5 : strncpy(Reason, "DEEPSLEEP_RESET", rsnLength);break;           // - 5,  Deep Sleep reset digital core
    case 6 : strncpy(Reason, "SDIO_RESET", rsnLength);break;                // - 6,  Reset by SLC module, reset digital core
    case 7 : strncpy(Reason, "TG0WDT_SYS_RESET", rsnLength);break;          // - 7,  Timer Group0 Watch dog reset digital core
    case 8 : strncpy(Reason, "TG1WDT_SYS_RESET", rsnLength);break;          // - 8,  Timer Group1 Watch dog reset digital core
    case 9 : strncpy(Reason, "RTCWDT_SYS_RESET", rsnLength);break;          // - 9,  RTC Watch dog Reset digital core
    case 10 : strncpy(Reason, "INTRUSION_RESET", rsnLength);break;          // - 10, Instrusion tested to reset CPU
    case 11 : strncpy(Reason, "TGWDT_CPU_RESET", rsnLength);break;          // - 11, Time Group reset CPU
    case 12 : strncpy(Reason, "SW_CPU_RESET", rsnLength);break;             // - 12, Software reset CPU
    case 13 : strncpy(Reason, "RTCWDT_CPU_RESET", rsnLength);break;         // - 13, RTC Watch dog Reset CPU
    case 14 : strncpy(Reason, "EXT_CPU_RESET", rsnLength);break;            // - 14, for APP CPU, reseted by PRO CPU
    case 15 : strncpy(Reason, "RTCWDT_BROWN_OUT_RESET", rsnLength);break;   // - 15, Reset when the vdd voltage is not stable
    case 16 : strncpy(Reason, "RTCWDT_RTC_RESET", rsnLength);break;         // - 16, RTC Watch dog reset digital core and rtc module
    default : strncpy(Reason, "NO_MEAN", rsnLength);                        // - undefined (not covered in this function)
  }
}

/**************************************************************************
 * RSSItoPrecentage
 * - Convert the RSSI dBi value to equavalent percentage.
 **************************************************************************/
int RSSItoPrecentage(int valRSSI) {
  int result = 0;

  result = (WiFi.RSSI()+100)*2;
  if (result>100) {
    result = 99;
  } else if (result < 0) {
    result = 0;
  }
  return result;
}

/**************************************************************************
 * reportState
 * - Feedback the current app state and telemetry values.
 **************************************************************************/
void reportState() {
  const int LEN = 30;
  char startReason[LEN];
  char UpTime[LEN];
  unsigned int espTemperature = temperatureRead();
  unsigned long UptimeSeconds = esp_timer_get_time()/1000/1000;
  String ipAddress = WiFi.localIP().toString();

  getRestartReason(startReason, LEN);
  sprintf(UpTime, "%01.0fd%01.0f:%02.0f:%02.0f", floor(UptimeSeconds/86400.0), floor(fmod((UptimeSeconds/3600.0),24.0)), floor(fmod(UptimeSeconds,3600.0)/60.0), fmod(UptimeSeconds,60.0));

  StaticJsonDocument<640> doc;
  // Set the values in the document
  doc["Version"] = SKETCH_VERSION;                                // software version of this sketch
  doc["Device"] = WiFi.getHostname();                             // device (network) name
  doc["IP Address"] = ipAddress;                                  // device IP address
  doc["SSID"] = WiFi.SSID();                                      // WLAN name 
  doc["RSSI (dBm)"] = WiFi.RSSI();                                // dBm value (negative)
  doc["wifi (%)"] = RSSItoPrecentage( WiFi.RSSI() );              // dBm value converted to signal strength percentage
  doc["Core Temperature (°C)"] = espTemperature;                  // ESP core temperature
  doc["Uptime"] = UpTime;                                         // day.hours:minutes:seconds since last boot
  doc["Start Reason"] = startReason;                              // reason for last restart
  doc["Free Heap Memory"] = esp_get_free_heap_size();
  //doc["Min Free Heap"] = esp_get_minimum_free_heap_size();  

  char buffer[640];
  size_t n = serializeJson(doc, buffer);
  clientMQTT.publish(MQTT_PUB_APPSTATE, buffer);
  Serial.print("> State: (size="); Serial.print(n); Serial.println(") ");  Serial.println(buffer);
}

/**************************************************************************
 * reportConfig
 * - Feedback the general settings (that is currently in memory).
 **************************************************************************/
void reportConfig() {

  StaticJsonDocument<512> doc;
  // Set the values in the document
  doc["AllowRemoteControl"] = appConfig.AllowRemoteControl;
  doc["AllowRemoteBleep"] = appConfig.AllowRemoteBleep;
  doc["MinLuxReportDelta"] = appConfig.Lux_MinReportDelta;
  doc["LuxInterval"] = appConfig.Lux_Interval;
  doc["TempInterval"] = appConfig.Temp_Interval;
  doc["StateInterval"] = appConfig.State_Interval;
  doc["DebounceDurSwitches"] = appConfig.DebounceDurSwitches;
  doc["DebounceDurMotor"] = appConfig.DebounceDurMotor;
  doc["RotationLimits"] = appConfig.RotationLimits;
  doc["ClosedRotationOffset"] = appConfig.ClosedOffset;
  doc["OpenDuration"] = appConfig.Open_Duration;
  doc["MaxOpenRotations"] = appConfig.Open_MaxRotations;
  doc["MaxCurrentLimit"] = appConfig.MaxCurrentLimit;
  doc["MaxRunDuration"] = appConfig.MaxRunDuration;
  doc["SSID"] = appConfig.SSID;
  //doc["Password"] = appConfig.Password;   // Perhaps better to not show Pwd in surrounding applications

  char buffer[512];
  size_t n = serializeJson(doc, buffer);
  if ( clientMQTT.setBufferSize(512) ) {                      // Increase buffer size, config exceeds default 256 bytes 
    clientMQTT.publish(MQTT_PUB_CONFIG, buffer, true);        // Publish configuration, retain state
    Serial.print("> Configuration: (size="); Serial.print(n); Serial.println(") ");  Serial.println(buffer);
  } else {
    // Failed to use increased MQTT buffer size (default is 256)
    Bleep("1x1.1.1");
  }
}

/**************************************************************************
 * loadConfig
 * - Get the settings on initialisation.
 **************************************************************************/
void loadConfig() {

  preferences.begin("app", true);    // opens "app" namespace in read-only mode

  appConfig.AllowRemoteControl = preferences.getBool("AllowRemoteCtl", true);       // Enable remote control of the blinds motor. (default: true)
  appConfig.AllowRemoteBleep = preferences.getBool("AllowRemoteBlp", true);         // Enable Bleep notifications through MQTT. (default: true)
  appConfig.Lux_Interval = preferences.getInt("LuxInterval", 0);                    // Interval between LUX reporting (minutes. 0 = disabled).
  appConfig.Lux_MinReportDelta = preferences.getInt("LuxMinDelta", 10);             // Minimum change since previous report.
  appConfig.Temp_Interval = preferences.getInt("TempInterval", 0);                  // Interval between Temperature reporting (minutes. 0 = disabled). 
  appConfig.State_Interval = preferences.getInt("StateInterval", 10);               // Interval between state values reporting (minutes. 0 = disabled).
  appConfig.DebounceDurSwitches = preferences.getInt("DebounceButton", 150);        // Debounce time for buttons and limit switches.
  appConfig.DebounceDurMotor = preferences.getInt("DebounceRotate", 500);           // Debounce time for motor rotation count switch.
  appConfig.RotationLimits = preferences.getBool("RotationLimits", true);           // Blinds considered open/closed based on rotation count. Else closed at limit switch.
  appConfig.Open_Duration = preferences.getInt("OpenDuration", 20);                 // How long the motor is allowed to run when opening the blinds (seconds. 0 = disabled).
  appConfig.ClosedOffset = preferences.getInt("ClosedOffset", 0);                   // Additional rotations motor will do when closing and "zero" rotation count reached (count).  
  appConfig.Open_MaxRotations = preferences.getInt("MaxOpenRotate", 20);            // How many rotations the motor can make before blinds are fully open (0 = disabled).
  appConfig.MaxCurrentLimit = preferences.getInt("MaxCurrentLmt", 0);               // Max load current before motor is stopped (raw analog reading. 0 = disabled).
  appConfig.MaxRunDuration = preferences.getInt("MaxRunDuration", 60);              // Max time motor can run in any direction (seconds).

  String ssid = preferences.getString("SSID", default_ssid);                        // Get SSID from preferences. If not defined use default SSID hardcoded in configuration.h
  String password = preferences.getString("Password", default_password);            // Get Password from preferences. If not defined use default password hardcoded in configuration.h

  appConfig.SSID = (char*) malloc(strlen(ssid.c_str() )+1);
  if (appConfig.SSID != NULL) {
    strcpy( appConfig.SSID, ssid.c_str() );
    appConfig.SSID[strlen(ssid.c_str())+1] = '\0';
  }
  appConfig.Password = (char*) malloc(strlen(password.c_str())+1);
  if (appConfig.Password != NULL) {
    strcpy( appConfig.Password, password.c_str() );
    appConfig.SSID[strlen(password.c_str())+1] = '\0';
  }
  #ifdef TELNET_DEBUG
    TelnetStream.println("LoadConfig done");
  #endif
  
  preferences.end();
}

/**************************************************************************
 * updatePreferences
 * - Update/set the provided setting in NVM.
 **************************************************************************/
void updatePreferences(const char* confKey, const char* newValue, const char* confType ) {

  preferences.begin("app", false);    // opens "app" namespace preferences in read-write mode
  
  // Set a INTEGER setting
  if ( strcmp(confType, "int") == 0) {
    char* p;
    int   iValue = 0;
    iValue = strtol(newValue, &p, 10);  
    if (iValue >= 0) {                        // only support positive nr's. also:   if (*p == 0) {
      Serial.printf("- updatePreferences: int=%d\n", iValue);
      preferences.putInt(confKey,  iValue);
      #ifdef TELNET_DEBUG
        TelnetStream.printf("UpdatePreferences: Key=%s, Value=%d\n", confKey, iValue);
      #endif
    } else {
      Serial.printf("- updatePreferences: NOT VALID int [%s]\n", newValue);
    }
  }
  else if ( strcmp(confType, "float") == 0) {
    char* p;
  // Set a FLOAT setting
    float   iValue = 0.0;
    iValue = strtof(newValue, &p);
    if (iValue >= 0) {                        // only support positive nr's. also:   if (*p == 0) {
      Serial.printf("- updatePreferences: float=%f\n", iValue);
      preferences.putFloat(confKey,  iValue);
      #ifdef TELNET_DEBUG
        TelnetStream.printf("UpdatePreferences: Key=%s, Value=%f\n", confKey, iValue);
      #endif
    } else {
      Serial.printf("- updatePreferences: NOT VALID long [%s]\n", newValue);
    }
  } 
  // Set a STRING setting
  else if (strcmp(confType, "string") == 0) {
    Serial.println("- updatePreferences: char");
    preferences.putString(confKey, newValue); 
    #ifdef TELNET_DEBUG
      TelnetStream.printf("UpdatePreferences: Key=%s, Value=%s\n", confKey, newValue);
    #endif
  } 
  else if (strcmp(confType, "bool") == 0) {
    Serial.printf("- updatePreferences: bool [%s]\n", newValue);
    preferences.putBool(confKey, strcasecmp(newValue, "true") == 0 || strcmp(newValue, "1") == 0 ); 
    #ifdef TELNET_DEBUG
      TelnetStream.printf("UpdatePreferences: Key=%s, Value=%s\n", confKey, newValue);
    #endif
  } else {
    Serial.printf("- updatePreferences: type unknown [%s]\n", confType);
  }
  preferences.end();                  // closes the namespace
}

/**************************************************************************
 * SaveCurrentPosition
 * - Store the current Blnds position in NVS, for use after restart..
 **************************************************************************/
void SaveCurrentPosition(int curBlindsPos)
{
  preferences.begin("run", false);    // opens "run" namespace in read-write mode
  preferences.putInt("BlindsPosition", curBlindsPos);
  #ifdef TELNET_DEBUG
    TelnetStream.print("Store current Blinds pos = "); TelnetStream.println(curBlindsPos);
  #endif
  preferences.end();                  // closes the namespace
}

/**************************************************************************
 * ReadLastPosition
 * - Read the last Blinds position from NVS.
 **************************************************************************/
int ReadLastPosition()
{
  int lastBlindPos;
  preferences.begin("run", true);    // opens "run" namespace in read mode
  lastBlindPos = preferences.getInt("BlindsPosition", 0);
  #ifdef TELNET_DEBUG
    TelnetStream.print("Read current Blinds pos = "); TelnetStream.println(lastBlindPos);
  #endif
  preferences.end();                  // closes the namespace
  return lastBlindPos;
}

/**************************************************************************
 *  remoteBlindsAction
 *  - Process the received MQTT Blinds action
 **************************************************************************/
void remoteBlindsAction(String msgAction) {

  //  "LIVINGROOM/BLINDS/ACTION" 
  //    -> open                         : open the Blinds fully (if currently closed).
  //    -> open:<%>                     : open the Blinds to certain percentage.
  //    -> close                        : close the Blinds if they are not closed already.
  //    -> stop                         : stop the Blinds if the motor is currently running.
  //
  if (msgAction.length() > 0) {
    // ACTION:  "OPEN"
    if (msgAction.substring(0,4) == "open" ) {
      bool okToProceed = true;
      // Get the target blinds position (if provided).
      mtrBlinds.targetPosition = -1;
      if (msgAction.indexOf(":") > 0 && appConfig.Open_MaxRotations > 0) {
        // A target percentage is provided. Determine the rotations based on the max rotations defined to open the blinds.
        int valSplit = msgAction.indexOf(":"); 
        if (valSplit > 0 && valSplit < msgAction.length() ) {
          mtrBlinds.targetPosition = round( (msgAction.substring(valSplit+1).toFloat() / 100) * (float)appConfig.Open_MaxRotations );
        }
      } else {
        // Open is received without a position. Open to max position.
        mtrBlinds.targetPosition = appConfig.Open_MaxRotations;
      }  
      // Do some validations.
      if (appConfig.Open_MaxRotations > 0) {
        // The max open position (nr of axis rotations) is defined. Do additional checks.
        if (!swcBlindsClosed.Set && mtrBlinds.currentPosition < 0 && mtrBlinds.targetPosition > 0) {
          // Blinds are open, but current position is unknown (e.g. after restart when blinds are open = -1). Must full close to sync position again.
          okToProceed = false;
          Serial.println(" - Not opening: current position unknown");
          TelnetStream.println(" - Not opening: current position unknown");
        } else if (!swcBlindsClosed.Set && appConfig.Open_MaxRotations == 0 && appConfig.Open_Duration > 0) {
          // Blinds are open, no full open position defined, and open timer is defined. 
          // Unknown current position, so timer has no meaning. Ignore the OPEN command (safety feature).
          okToProceed = false;
          Serial.println(" - Not opening: Blinds already open and only using timer ");
          TelnetStream.println(" - Not opening: Blinds already open and only using timer ");
        } else if (mtrBlinds.targetPosition < 0 || mtrBlinds.targetPosition > appConfig.Open_MaxRotations ) {
          // Blinds already at or past max open position. Ignore the OPEN command (safety feature).
          okToProceed = false;
          Serial.printf(" - Not opening: invalid target below 0 or beyond max open position (%d)\n", mtrBlinds.targetPosition);
          TelnetStream.println(" - Not opening: invalid target below 0 or beyond max open position\n");
        } else if (mtrBlinds.targetPosition == mtrBlinds.currentPosition) {
          // Target and current positions the same. Ignore OPEN command.
          okToProceed = false;
          Serial.println(" - Not opening: current and target positions the same");
          TelnetStream.println(" - Not opening: current and target positions the same");
        } else if (mtrBlinds.targetPosition > mtrBlinds.currentPosition && swcBlindsOpen.Set ) {
          // Blinds already fully open. Ignore the OPEN command (safety feature).
          okToProceed = false;
          Serial.println(" - Not opening: Blinds already fully opened (limit)");
          TelnetStream.println(" - Not opening: Blinds already fully opened (limit)");
        }
      }
      if (okToProceed) {
        if (appConfig.Open_MaxRotations > 0 && mtrBlinds.targetPosition >= 0) {
          // The number of full open rotations is defined, and a target position is provided.
          // Rotation is based on current position: see if blinds must be opened or closed to reach target.
          if (mtrBlinds.targetPosition > mtrBlinds.currentPosition) {
            Serial.print(" - Opening blinds to position: "); Serial.println(mtrBlinds.targetPosition);
            mqttBlindsAction.Action = actBlindsOpen;
          } else {
            Serial.print(" - Closing blinds to position: "); Serial.println(mtrBlinds.targetPosition);
            mqttBlindsAction.Action = actBlindsClose;
          }
          mqttBlindsAction.NewAction = true;
        } else {
          // No target position provided, or no full open position defined. Just fully open blinds (if not already fully open).
          if (!swcBlindsOpen.Set ) {
            mtrBlinds.targetPosition = appConfig.Open_MaxRotations;
            mqttBlindsAction.Action = actBlindsOpen;
            mqttBlindsAction.NewAction = true;
          } else {
            // Can't open blinds further if open limit switch is already set.
            Serial.print(" - Not opening: Blinds already fully opened (limit set)"); Serial.println(mtrBlinds.targetPosition);
            TelnetStream.println(" - Not opening: Blinds already fully opened (limit set)"); 
            Bleep("1x1.1");
          }
        }
      } else {
        Bleep("1x1.1");
      }
    }

    // ACTION:  "CLOSE"
    else if (msgAction == "close") {
      if ( swcBlindsClosed.Set || (appConfig.RotationLimits && mtrBlinds.currentPosition == 0) ) {
        Serial.println(" - Not closing, Blinds already closed");
        TelnetStream.println(" - Not closing, Blinds already closed");
        Bleep("1x1.1");                                               // raise audible error.
      } else {
        mtrBlinds.targetPosition = 0;
        mqttBlindsAction.Action = actBlindsClose;
        mqttBlindsAction.NewAction = true;
      }
    }

    // ACTION:  "STOP"
    else if (msgAction == "stop") {
      mtrBlinds.AllowToRun = false;
      mtrBlinds.Action = actBlindsStop;
      mtrBlinds.Owner = ownMQTT;
      mtrBlinds.targetPosition = -1; 
      xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
        actionStopMotor = true;
      xSemaphoreGive(semBlindsCheck);
    }

    else {
      Serial.printf(" >>> UNKNOWN blinds action (%s)\n", msgAction.c_str() ); 
      TelnetStream.print(" >>> UNKNOWN blinds action: "); TelnetStream.println( msgAction.c_str() );
      Bleep("1x1.1.1");                                               // raise audible error.
    }
  }
}

/**************************************************************************
 *  remoteAppAction
 *  - Process the received MQTT application-related action
 **************************************************************************/
void remoteAppAction(String msgAction) {

  // LIVINGROOM/BLINDS/APPCMD 
  //    -> restart                          : restart ESP32
  //    -> getstate                         : report the current state and telemetry values (RSSI, Memory, ..)
  //    -> getconfig                        : report the current application configuration
  //    -> StateInterval:<minutes>          : set the interval between state updates (0 = disabled)
  //    -> LuxInterval:<minutes>            : set the interval between Lux updates (0 = disabled)
  //    -> TempInterval:<minutes>           : set the interval between Temperature updates (0 = disabled)
  //    -> RotationLimits:<true/false>      : set if blinds is considered open/closed based on rotations (true) or only at limit switch (false) 
  //    -> ClosedRotationOffset:<count>     : set additional rotations motor will do if CLOSE rotation count is reached (count) 
  //    -> DebounceDurSwitches:<mseconds>   : set the debounce time for Buttons and Limit switches (milliseconds)
  //    -> DebounceDurMotor:<mseconds>      : set the debounce time for the motor rotation switch (milliseconds)
  //    -> OpenDuration:<seconds>           : set max duration the motor will run when OPENING the blinds (0 = check and timer disabled)
  //    -> MaxRunDuration:<seconds>         : set max duration the motor may run in ANY direction (0 = check and timer disabled)
  //    -> MaxOpenRotations:<count>         : set max number of axis rotations that blinds can open (0 = disabled)
  //    -> MinLuxReportDelta:<lux>          : set the minimum difference in Lux level before publishing MQTT (0 = no threshold, interval only)
  //    -> MaxCurrentLimit:<value>          : set max load current motor is allowed to draw (raw analog value) (0 = disabled)
  //    -> AllowRemoteControl:<true/false>  : set control Blinds using MQTT (true), else (false)
  //    -> AllowRemoteBleep:<true/false>    : set if Bleep notifications must be processed (true) or ignored (false)
  //    -> WiFiSetup:SSID/password          : set the SSID and password to be used ("default" for hardcoded defaults)
  //  
  if (msgAction.length() > 0) {
    //
    // :: restart  ->>  restart ESP32
    if (msgAction == "restart") {
      Serial.println("\t- MQTT -- RESTART ESP32");
      TelnetStream.println("\t- MQTT -- RESTART ESP32");
      Bleep("2x1.1.0");                                                   // Audio indication 
      delay(100);
      esp_restart();                                                      // RESTART ESP32 !!!!!
    }
    //
    // ::   getstate  ->>  report the current state and telemetry values (RSSI, Memory, ..)
    else if (msgAction == "getstate") {
      Serial.println("\t- MQTT request State and Telemetry values");
      reportState();                                                      // Feedback current telemetry values (once)
    }
    //
    // ::   getconfig  ->>  report the current application configuration
    else if (msgAction == "getconfig") {
      Serial.println("\t- MQTT request Configuration values");
      reportConfig();                                                     // Feedback current configuration (once)
    }
    //
    // :: StateInterval:<minutes>  ->>  set the interval between state updates (0=disabled)
    else if (msgAction.substring(0,13) == "StateInterval") {
      Serial.print("\t- MQTT set State Interval ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.State_Interval = msgAction.substring(valSplit+1).toInt();           // Set State feedback interval (in seconds)
        updatePreferences("StateInterval", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.State_Interval);
      } else {
        Serial.println(" >>> INVALID INTERVAL!!");
      }
    }  
    //
    // :: LuxInterval:<minutes>  ->>  set the interval between Lux updates (0=disabled)
    else if (msgAction.substring(0,11) == "LuxInterval") {
      Serial.print("\t- MQTT set Lux Interval ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.Lux_Interval = msgAction.substring(valSplit+1).toInt();            // Set interval between Lux feedback (in seconds)
        updatePreferences("LuxInterval", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.Lux_Interval);
      } else {
        Serial.println(" >>> INVALID INTERVAL!!");
      }
    }  
    //
    // :: TempInterval:<minutes>  ->>  set the interval between Temperature updates (0=disabled)
    else if (msgAction.substring(0,12) == "TempInterval") {
      Serial.print("\t- MQTT set Temp Interval ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.Temp_Interval = msgAction.substring(valSplit+1).toInt();            // Set interval between temperature feedback (in seconds)
        updatePreferences("TempInterval", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.Temp_Interval);
      } else {
        Serial.println(" >>> INVALID INTERVAL!!");
      }
    }  
    //
    // :: OpenDuration:<seconds>  ->>  set max duration the motor will run when opening the blinds (0 = check and timer disabled)
    else if (msgAction.substring(0,12) == "OpenDuration") {
      Serial.print("\t- MQTT set Max Open Run Duration ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.Open_Duration = msgAction.substring(valSplit+1).toInt();            // Set max open run duration (in seconds)
        updatePreferences("OpenDuration", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.Open_Duration);
      } else {
        Serial.println(" >>> INVALID DURATION!!");
      }
    }
    //
    // :: MaxRunDuration:<seconds>  ->>  set max duration the motor will run (any direction) (0 = check and timer disabled)
    else if (msgAction.substring(0,14) == "MaxRunDuration") {
      Serial.print("\t- MQTT set Max Run Duration ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.MaxRunDuration = msgAction.substring(valSplit+1).toInt();           // Set max run duration (in seconds)
        updatePreferences("MaxRunDuration", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.MaxRunDuration);
      } else {
        Serial.println(" >>> INVALID DURATION!!");
      }
    }
    //
    // :: MaxOpenRotations:<number>  ->>  set max number of axis rotations before blinds are fully open (0 = check disabled)
    else if (msgAction.substring(0,16) == "MaxOpenRotations") {
      Serial.print("\t- MQTT set Max Open Axis Rotations ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.Open_MaxRotations = msgAction.substring(valSplit+1).toInt();        // Set max axis rotations before blinds are fully open
        updatePreferences("MaxOpenRotate", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.Open_MaxRotations);
      } else {
        Serial.println(" >>> INVALID OPEN COUNT!!");
      }
    }  
    //
    // ::   RotationLimits:<true/false>  ->>  set if blinds are open/closed based on rotation count (true), else closed at limit switches (false)
    else if (msgAction.substring(0,14) == "RotationLimits") {
      Serial.print("\t- MQTT set blinds are opened/closed based on rotation count ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        if (msgAction.substring(valSplit+1) == "true") {
          appConfig.RotationLimits = true;                    // Open blinds using (max) rotation count
          updatePreferences("RotationLimits", "true", "bool" );
        } else {
          appConfig.RotationLimits = false;                   // Open blinds ignoring rotation count
          updatePreferences("RotationLimits", "false", "bool" );
        }
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.RotationLimits);
      } else {
        Serial.println(" >>> INVALID BOOLEAN!!");
      }
    }
    //
    // :: ClosedRotationOffset:<count>    ->> set additional rotations motor will do if CLOSE rotation count is reached (count)
    else if (msgAction.substring(0,20) == "ClosedRotationOffset") {
      Serial.print("\t- MQTT set Close Rotation count Offset ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.ClosedOffset = msgAction.substring(valSplit+1).toInt();
        updatePreferences("ClosedOffset", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.ClosedOffset);
      } else {
        Serial.println(" >>> INVALID ROTATION OFFSET!!");
      }
    }
    //
    // :: DebounceDurSwitches:<duration>  ->>  set debounce delay used for buttons and limit switches
    else if (msgAction.substring(0,19) == "DebounceDurSwitches") {
      Serial.print("\t- MQTT set Limit and Button debounce time ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.DebounceDurSwitches = msgAction.substring(valSplit+1).toInt();
        updatePreferences("DebounceButton", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.DebounceDurSwitches);
      } else {
        Serial.println(" >>> INVALID DEBOUNCE TIME!!");
      }
    }  
    //
    // :: DebounceDurMotor:<duration>  ->>  set debounce delay used for the motor axis rotation switch
    else if (msgAction.substring(0,16) == "DebounceDurMotor") {
      Serial.print("\t- MQTT set Motor Rotation switch debounce time ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.DebounceDurMotor = msgAction.substring(valSplit+1).toInt();         // Set the rotation switch debounce timeout
        updatePreferences("DebounceRotate", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.DebounceDurMotor);
      } else {
        Serial.println(" >>> INVALID DEBOUNCE TIME!!");
      }
    }  
    //
    // :: MinLuxReportDelta:<lux>  ->>  set the minimum difference in Lux level before publishing MQTT (0=no threshold, interval only)
    else if (msgAction.substring(0,17) == "MinLuxReportDelta") {
      Serial.print("\t- MQTT set Min Lux Report Delta ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.Lux_MinReportDelta = msgAction.substring(valSplit+1).toInt();       // Set min Lux report delta
        updatePreferences("LuxMinDelta", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.Lux_MinReportDelta);
      } else {
        Serial.println(" >>> INVALID DURATION!!");
      }
    }  
    //
    // :: MaxCurrentLimit:<val>  ->>  set the maximum current the motor can draw before it is stopped  (0 = no limit)
    else if (msgAction.substring(0,15) == "MaxCurrentLimit") {
      Serial.print("\t- MQTT set Max load current");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        appConfig.MaxCurrentLimit = msgAction.substring(valSplit+1).toInt();          // Set max load current allowed
        updatePreferences("MaxCurrentLmt", msgAction.substring(valSplit+1).c_str(), "int");
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.MaxCurrentLimit);
      } else {
        Serial.println(" >>> INVALID MAX CURRENT!!");
      }
    }
    //
    // ::   AllowRemoteControl:<true/false>  ->>  set control Blinds using MQTT (true), else (false)
    else if (msgAction.substring(0,18) == "AllowRemoteControl") {
      Serial.print("\t- MQTT set Allow Remote Control ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        if (msgAction.substring(valSplit+1) == "true") {
          appConfig.AllowRemoteControl = true;                    // Allow blinds to be controlled remotely
          updatePreferences("AllowRemoteCtl", "true", "bool" );
        } else {
          appConfig.AllowRemoteControl = false;                   // Ignore remote (MQTT) commands
          updatePreferences("AllowRemoteCtl", "false", "bool" );
        }
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.AllowRemoteControl);
      } else {
        Serial.println(" >>> INVALID BOOLEAN!!");
      }
    }
    //
    // ::   AllowRemoteBleep:<true/false>  ->>  set if Bleep notifications must be processed (true) or ignored (false)
    else if (msgAction.substring(0,16) == "AllowRemoteBleep") {
      Serial.print("\t- MQTT set Allow Bleep Notifications ");
      int valSplit = msgAction.indexOf(":"); 
      if (valSplit>0 && valSplit < msgAction.length() ) {
        // Seems like a valid parameter
        if (msgAction.substring(valSplit+1) == "true") {
          appConfig.AllowRemoteBleep = true;                      // Support global bleep notifications
          updatePreferences("AllowRemoteBlp", "true", "bool" );
        } else {
          appConfig.AllowRemoteBleep = false;                     // Ignore global (MQTT) bleep messages
          updatePreferences("AllowRemoteBlp", "false", "bool" );
        }
        reportConfig();                                                               // feedback new configuration settings
        //Serial.print(" NewVal="); Serial.println(appConfig.AllowRemoteBleep);
      } else {
        Serial.println(" >>> INVALID BOOLEAN!!");
      }
    }  
    //
    // ::   WiFiSetup:SSID/password  ->>  set the SSID and password to be used ("default" for default).
    else if (msgAction.substring(0,9) == "WiFiSetup") {
      int valSplit = msgAction.indexOf("/"); 
      if (valSplit>10 && valSplit < msgAction.length() ) {
        // New SSID and Pwd. Set the new values in config.
        appConfig.SSID = (char*) malloc((valSplit-10)+1);
        if (appConfig.SSID != NULL) {
          strncpy( appConfig.SSID, msgAction.substring(10).c_str(), valSplit-10 );
          appConfig.SSID[valSplit-10] = '\0';
        }
        appConfig.Password = (char*) malloc((msgAction.length()-valSplit)+1);
        if (appConfig.Password != NULL) {
          strncpy( appConfig.Password, msgAction.substring(valSplit+1).c_str(), msgAction.length()-valSplit );
          appConfig.Password[msgAction.length()-valSplit] = '\0';
        }
        updatePreferences("SSID", appConfig.SSID, "string");
        updatePreferences("Password", appConfig.Password, "string");
        reportConfig();
      } else if (msgAction.substring(10,17) == "default") {
        // "default". Set the default SSID and Password (reset to defaults).  
        appConfig.SSID = (char*) malloc (strlen(default_ssid)+1);
        if (appConfig.SSID != NULL) {
          strncpy( appConfig.SSID, default_ssid, strlen(default_ssid) );
          appConfig.SSID[strlen(default_ssid)] = '\0';
        }
        appConfig.Password = (char*) malloc (strlen(default_password)+1);
        if (appConfig.Password != NULL) {
          strncpy( appConfig.Password, default_password, strlen(default_password) );
          appConfig.Password[strlen(default_password)] = '\0';
        }
        updatePreferences("SSID", appConfig.SSID, "string");
        updatePreferences("Password", appConfig.Password, "string");
        reportConfig();
      } else {
        Serial.println(" >>> INVALID WiFi config!!");
      }
    }

    else {
      Serial.printf(" >>> UNKNOWN APP ACTION (%s)\n", msgAction.c_str() ); 
      TelnetStream.print(" >>> UNKNOWN APP action: "); TelnetStream.println( msgAction.c_str() );
      Bleep("1x1.1.1");                                               // raise audible error.
    }
  }  
}

/**************************************************************************
 *  MQTT_callback
 *  - Process received MQTT messages, subscribed in setup_MQTT().
 **************************************************************************/
void MQTT_callback (char* topic, byte* message, unsigned int length) {
  String msgAction;

  for (int i = 0; i < length; i++) {
    msgAction += (char)message[i];
  }
  Serial.printf("MQTT Message.  Topic: %s - Action: %s\n", topic, msgAction.c_str() );  

  // TOPIC: LIVINGROOM/BLINDS/ACTION
  if (String(topic) == MQTT_SUB_BLINDSACTION) {
    // If Blinds control through MQTT is enabled in the configuration..
    if (appConfig.AllowRemoteControl) {
      remoteBlindsAction(msgAction);
    }
  }  

  // TOPIC: LIVINGROOM/BLINDS/APPCMD 
  else if (String(topic) == MQTT_SUB_APPCMD ) { 
    remoteAppAction(msgAction);
  }

  // TOPIC:  "ALL/NOTIFY/BLEEP" 
  else if (String(topic) == MQTT_SUB_NOTIFY) {
    if (appConfig.AllowRemoteBleep) {
      // Process the received Buzzer Bleep
      Serial.printf("MQTT notify/bleep: %s", msgAction.c_str() );
      Bleep(msgAction);
    }
  }

  else {
    Serial.printf(" >>> UNKNOWN MQTT TOPIC (%s)\n", topic ); 
    TelnetStream.print(" >>> UNKNOWN APP action: "); TelnetStream.println( topic );
  }

}

/**************************************************************************
 *  setup_WIFI
 *  - Connect to specified WLAN.
 **************************************************************************/
bool setup_WIFI(bool UseDefault) {
  int i = 0;
  
  if ( !WiFi.isConnected() ) {
    WiFi.mode(WIFI_STA);
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.setHostname(DEVICE_NAME);
    if ( !UseDefault && strlen(appConfig.SSID)>0 && strlen(appConfig.Password)>0 ) {
      Serial.print("WiFi(cfg): Connecting to "); Serial.print(appConfig.SSID); Serial.print("/"); Serial.println(appConfig.Password);
      WiFi.begin(appConfig.SSID, appConfig.Password);
    } else {
      Serial.print("WiFi(def): Connecting to "); Serial.print(default_ssid); 
      WiFi.begin(default_ssid, default_password);
    }

    while (WiFi.status() != WL_CONNECTED && i<wifiMaxRetry) {
      Serial.print(".");
      delay(1000);
      i++;
    }
    Serial.println("");
    Serial.print("WiFi connection: Status="); Serial.print(WiFi.status()); 
    Serial.print("- IP="); Serial.print(WiFi.localIP()); 
    Serial.print("- Device="); Serial.print(WiFi.getHostname()); 
    Serial.print("- RSSI="); Serial.println(WiFi.RSSI()); 
    //Serial.print(" MAC=");  Serial.println(WiFi.macAddress());
  }
  
  return WiFi.isConnected();
}

/**************************************************************************
 *  setup_MQTT
 *  - If WiFi is not connected, reconnect WiFi. Reboot if this fails.
 *  - Connect/reconnect to the MQTT server.
 **************************************************************************/
bool setup_MQTT() {
  int i = 0;

  if ( !WiFi.isConnected() ) { 
    if ( !setup_WIFI(false) ) {
      // First reconnection using preference values failed.
      if ( strcmp(appConfig.SSID, default_ssid) != 0) {
        // Try again with the default SSID and Password.
        setup_WIFI(true);
      }
    }
  }
  if ( !WiFi.isConnected() && !mtrBlinds.IsRunning ) {
    // Failed to connect to WiFi, and motor is not running. Reboot.
    Serial.println("WiFi reconnect failed. Rebooting..");
    Bleep("2x2.1.0");
    delay(1000);
    ESP.restart();
  }

  if ( !clientMQTT.connected() ) {
    Serial.print("MQTT - connect to server. "); Serial.print(" Signal Level: ");  Serial.println(WiFi.RSSI());
    // Loop until we're reconnected to MQTT server
    while ( !clientMQTT.connected() && i<mqttMaxRetry ) {
      if ( clientMQTT.connect("ESP32Client", "MQTT", mqtt_pwd) ) {
        Serial.print("- MQTT connected. "); Serial.print(" WiFi="); Serial.println(WiFi.RSSI());
        // Subscribe to the relevant topics
        clientMQTT.subscribe(MQTT_SUB_BLINDSACTION);
        clientMQTT.subscribe(MQTT_SUB_NOTIFY);
        clientMQTT.subscribe(MQTT_SUB_APPCMD);

      } else {
        Serial.print("- MQTT connect failed! rc="); Serial.print(clientMQTT.state());
        Serial.print(" RSSI="); Serial.println(WiFi.RSSI()); 
        // Wait before retrying
        delay(1000);
      }
      i++;
    }    
  }
  return clientMQTT.connected();
}

/**************************************************************************
 *  setup
 *  - Define pins.
 *  - Define interrupts.
 *  - Set up PWM channels and pins.
 *  - Initialize WiFi and MQTT.
 *  - Initiate the Lux sensor.
 *  - Create task for running inifinate loop in seperate thread.
 *  - Start OTA (over the air updates)
 **************************************************************************/
void setup() {
  Serial.begin(115200);
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);    // brown-out
  Serial.println("");

  // Read configuration from preferences stored in NVS.
  preferences.begin("app", false);
  loadConfig();
  Serial.println("Setup: Reading config file done!");

  // Configure the pins.
  pinMode(pin_RPWM, OUTPUT);                          // RIGHT pulse width modulation
  pinMode(pin_LPWM, OUTPUT);                          // LEFT pulse width modulation
  pinMode(pin_REN, OUTPUT);                           // Enable RIGHT rotation
  pinMode(pin_LEN, OUTPUT);                           // Enable LEFT rotation
  pinMode(pin_Buzzer, OUTPUT);                        // Active Buzzer 
  pinMode(pin_BtnOpen, INPUT_PULLUP);                 // OPEN button
  pinMode(pin_BtnClose, INPUT_PULLUP);                // CLOSE button
  pinMode(pin_StopClosed, INPUT_PULLUP);              // CLOSED limit switch
  pinMode(pin_StopOpen, INPUT_PULLUP);                // OPEN limit switch
  pinMode(pin_MotorRotations, INPUT_PULLUP);          // Pin used to count motor rotations (wiper motor slip ring)

  // Set up WiFi and MQTT.
  if ( !setup_WIFI(false) ) {
    // First connection using config values failed.
    if ( strcmp(appConfig.SSID, default_ssid) != 0) {
      // Try again with the default SSID and Password.
      setup_WIFI(true);
    }
  } 
  
  if (WiFi.isConnected()) {
    // WiFi setup and connection succeeded.
    delay(500);
    clientMQTT.setServer(mqtt_server, 1883); 
    clientMQTT.setCallback(MQTT_callback);                               // local function to call when MQTT msg received.
    setup_MQTT();
  } else {
    // Reboot and try WiFi connection again.
    Serial.println("\nWiFi NOT CONNECTED!\n");
    Bleep("1x1.1.1");
    delay(5000);
    ESP.restart();
  }

  #ifdef TELNET_DEBUG
    TelnetStream.begin();
  #endif

  // Initialize I2c for AM2320 and TSL2561
  Wire.begin();

  // Initiate and set up the Lux light sensor.
  luxSensor.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);
  Serial.println("Lux sensor (BH1750) configured.");
  
  // Configure the PWM detail.
  ledcSetup(pwmChannel_Open, pwmFrequency, pwmResolution);
  ledcSetup(pwmChannel_Close, pwmFrequency, pwmResolution);
  // Attach the channels to the PWM pins.
  ledcAttachPin(pin_RPWM, pwmChannel_Open);
  ledcAttachPin(pin_LPWM, pwmChannel_Close);

  semBlindsCheck = xSemaphoreCreateMutex();                                     // ??

  // Set up timer to automatically limit motor run duration when opening.
  tmrBlindsOpen = timerBegin (0, 80, true);                                     // use ESP32 Timer 0, pre-scale 80 (of 80MHz), count up.
  timerAttachInterrupt (tmrBlindsOpen, &isrTimerBlindsOpen, true);              // attach the function to call when timer interrupt fires. Edge.

  // Set up master timer to automatically stop motor after running a max duration.
  tmrBlindsMaster = timerBegin (1, 80, true);                                   // use ESP32 Timer 0, pre-scale 80 (of 80MHz), count up.
  timerAttachInterrupt (tmrBlindsMaster, &isrTimerBlindsMaster, true);          // attach the function to call when timer interrupt fires. Edge.

  // Set up timer to automatically limit motor run duration when opening.
//  tmrBlindsClosedDelay = timerBegin (0, 80, true);                              // use ESP32 Timer 0, pre-scale 80 (of 80MHz), count up.
//  timerAttachInterrupt (tmrBlindsClosedDelay, &isrTimerClosedPosDelay, true);   // attach the function to call when timer interrupt fires. Edge.

  // Create the task that will run in a seperate thread on Core 1.
  // NOTE: the task starts to run immediately after its creation below.
  xTaskCreatePinnedToCore (
      loop_MotorActions,        // Function to be executed by the task 
      "loop_MotorActions",      // Name of the task 
      2000,                     // Stack size in words 
      NULL,                     // Task input parameter 
      1,                        // Priority of the task 
      &taskLoopMotorActions,    // Task handle 
      1);                       // Core where the task should run (Core 1 in this case) 

  // Configure the interrupts.
  attachInterrupt(pin_BtnOpen, isrButtonBlindsOpen, FALLING);                    // Blinds go up button pressed/released.
  attachInterrupt(pin_BtnClose, isrButtonBlindsClose, FALLING);                  // Blinds go down button pressed/released.
  attachInterrupt(pin_MotorRotations, isrMotorRotations, FALLING);              // Count axis rotation pulses.

  // Show board detail
  esp_chip_info_t espInfo;
  esp_chip_info(&espInfo);
  Serial.println("ESP32 detail: -----------------");
  Serial.print("\t- Nr of cores: \t"); Serial.println(espInfo.cores);
  Serial.print("\t- ESP Model: \t"); Serial.println( espInfo.model );
  Serial.print("\t- Revision: \t"); Serial.println( espInfo.revision );
  Serial.print("\t- IDF version: \t"); Serial.println( esp_get_idf_version() );
  Serial.print("\t- PSRAM: \t"); if (psramFound()) { Serial.println("Yes"); } else { Serial.println("No"); };

  // On startup, see if the blinds are (fully) open or closed.
  swcBlindsClosed.Set = (digitalRead(pin_StopClosed) == LOW);         // CLOSED (Normal high switch will be pulled low when closed.) 
  swcBlindsOpen.Set = (digitalRead(pin_StopOpen) == LOW);             // OPEN   (Normal high switch will be pulled low when closed.) 
  if (swcBlindsClosed.Set) {
    mtrBlinds.currentPosition = 0;             // If closed then set the initial position to 0.
  } else {
  // Read last blinds position from NVS.
    mtrBlinds.currentPosition = ReadLastPosition();
  }
  
  // Publish initial state to ensure HA is in sync.
  mqttPublishBlindsState = true;

  setupOTA("BlindsControl");

  Serial.println("Blinds Control setup done.\n");
  Bleep("1x3");
  delay(1000);

  #ifdef TELNET_DEBUG
    TelnetStream.println("BlindsController setup:");
    TelnetStream.print("- SSID: "); TelnetStream.println( WiFi.SSID() );
    TelnetStream.print("- IP: "); TelnetStream.println( WiFi.localIP() );
    TelnetStream.print("- Device: "); TelnetStream.println( WiFi.getHostname() );
  #endif

}

/**************************************************************************
 *  loop (default/standard)
 *  - Check WiFi and reconnect if necessary.
 *  - Publish any Blinds status changes.
 *  - Measure and publish the light level.
 *  - Measure and publish the temperature.
 **************************************************************************/
void loop() {
  static unsigned long lastLuxReport = 0;             // Last LUX reported time (in seconds)
  static unsigned long lastTempReport = 0;            // Last Temperature status report (in seconds)
  static unsigned long lastStateReport = 0;           // Last app/wifi status report (in seconds)
  static unsigned long lastCurrentSense = 0;

  if (DoBleepTimes>0) {
    MyBleep(DoBleepTimes);
    DoBleepTimes = 0;
  }

  // Check load current if enabled (>0) and motor is running. Stop motor if limit is exceeded. 
  if ( mtrBlinds.IsRunning ) {
    if  ( millis() - lastCurrentSense > currentSenseInterval ) {
      int motorCurrent = 0;
      motorCurrent = analogRead(pin_iSense);
      Serial.print("Motor Current: "); Serial.println(motorCurrent);
//    #ifdef TELNET_DEBUG
//      TelnetStream.print(" - loop: Motor Current = " ); TelnetStream.println(motorCurrent);
//    #endif
      if ( appConfig.MaxCurrentLimit > 0 && motorCurrent > appConfig.MaxCurrentLimit ) {
        // Max load current exceeded. Stop motor.
        xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
        actionStopMotor = true;
        xSemaphoreGive(semBlindsCheck);
        Serial.print(">>> Max current load exceeded! - "); Serial.println(motorCurrent);
        Bleep("2x1.1.1");           // Audible alarm
      }
      lastCurrentSense = millis();
    }
  }

  // Publish Blinds status if it changed since last check.
  if (mqttPublishBlindsState) {
    StaticJsonDocument<50> configDoc;
    if (swcBlindsClosed.Set) { configDoc["state"] = "closed"; } else { configDoc["state"] = "open"; }
    if (appConfig.Open_MaxRotations > 0 ) {
      if (swcBlindsClosed.Set) { mtrBlinds.currentPosition=0; } 
      configDoc["percentage"] = round( ( (float)mtrBlinds.currentPosition / (float)appConfig.Open_MaxRotations) * 100 );      
    } else {
      configDoc["percentage"] = "-";
    }
    char buffer[50];
    serializeJson(configDoc, buffer);
    //#clientMQTT.publish(MQTT_PUB_BLINDSSTATE, buffer, true);          // publish retain state ??
    clientMQTT.publish(MQTT_PUB_BLINDSSTATE, buffer);
    Serial.println(" - MQTT publish Blinds State: ");  Serial.println(buffer);
    mqttPublishBlindsState = false;
  }

  // Measure the Temperature if enabled (>0), and it is the first time or if the reporting interval has expired. 
  if ( appConfig.Temp_Interval > 0 ) {
    if  ( lastTempReport == 0 || ( (millis()/1000 - lastTempReport)/60 > appConfig.Temp_Interval ) ) {
      reportTemperature();
      lastTempReport = millis()/1000;
    }
  }
 
  // Measure the light if enabled (>0), and it is the first time or if the reporting interval has expired. 
  if ( appConfig.Lux_Interval > 0 ) {
    if  ( lastLuxReport == 0 || ( (millis()/1000 - lastLuxReport)/60 > appConfig.Lux_Interval ) ) {
      reportLux();
      lastLuxReport = millis()/1000;
    }
  }

  // Feedback ESP32 State and/or WiFi parameters if  enabled (interval>0) and interval has expired.
  if ( appConfig.State_Interval > 0 ) {
    if  ( lastStateReport == 0 || (millis()/1000-lastStateReport)/60 > appConfig.State_Interval ) {
      reportState();
      lastStateReport = millis()/1000;
    }
  }

  // Confirm if enough memory allocated to Task to prevent overflowing the stack.
  // uxTaskGetStackHighWaterMark2(TaskHandle_t &taskLoopMotorActions) 

  // Check MQTT and reconnect if necessary.
  if ( !clientMQTT.connected() ) {
    setup_MQTT();
  } else {
    clientMQTT.loop();
  }
}

/**************************************************************************
 *  CheckLimitSwitch
 *  Return true if the specified limit switch is set. 
 * Actions:
 *  - If switch is pulled low, set low bit to "0" and shift register for each function call. True if value = "F000". 
 *  - If switch is released (high), set low bit to "1" for each call.
 * https://www.best-microcontroller-projects.com/easy_switch_debounce.html
 * 
 **************************************************************************/
bool CheckLimitSwitch(uint8_t pinLimitSwitch) {
static uint16_t swc_dbc = 0;
//static uint16_t lastb = 0;

   swc_dbc=(swc_dbc<<1) | digitalRead(pinLimitSwitch) | 0xe000;
//   if (swc_dbc!=lastb) TelnetStream.println(swc_dbc,HEX); 
//   lastb = swc_dbc;
   if (swc_dbc==0xf000) {
     swc_dbc = 0;
     return true;
   } else {return false;}
}

/**************************************************************************
 *  loop_MotorActions
 *  This loop task runs in a seperate thread, on Core "1" (if run on core 0 then WiFi/MQTT freezes).
 *  This task will process the motor actions, based on flags set in interrupt events.
 *  WiFi and MQTT-related actions are done from the standard main loop. 
 * Actions:
 *  - Run motor up/down based on Up/Down button changes. 
 *  - Run motor up/down based on MQTT requests.
 *  - Stop motor when Open/Close limit switches are triggered, or button released. 
 **************************************************************************/
void loop_MotorActions (void * parameter) {

  bool actionClosedStopOffset = false;              // Blinds closed position reached, but only stop after (rotation) offset 
  int countClosedOffset = 0;                        // Number of offset rotations after closed position reached.

  for (;;) {

    // --- LIMIT SWITCHES ---
    // Check limit switch states (only) if motor is running. 
    if ( mtrBlinds.IsRunning ) {
      if (mtrBlinds.Action == actBlindsClose) {
        // CLOSING. Stop if CLOSED switch is set.
        swcBlindsClosed.Set = CheckLimitSwitch(pin_StopClosed);
        if (swcBlindsClosed.Set) {
          // Blinds are closed. Stop the motor.
          #ifdef TELNET_DEBUG
            TelnetStream.println(" - loop: CLOSE switch set. Motor STOP");
          #endif
          mtrBlinds.currentPosition = 0;  // Consider blinds fully closed if bottom limit switch is set.
          actionStopMotor = true;
          swcBlindsOpen.Set = false;      // If the CLOSED limit is hit then the blinds can't be open.
        }
      }
      else if (mtrBlinds.Action == actBlindsOpen) {
        // OPENING. Stop if OPEN switch is set
        swcBlindsOpen.Set = CheckLimitSwitch(pin_StopOpen);
        if (swcBlindsOpen.Set) {
          // Blinds are fully open. Stop the motor.
          #ifdef TELNET_DEBUG
            TelnetStream.println(" - loop: OPEN switch set. Motor STOP");
          #endif
          //mtrBlinds.currentPosition = 100;  // Consider blinds fully opened if top limit switch is set.
          actionStopMotor = true;
          swcBlindsClosed.Set = false;      // If the OPEN limit is hit then the blinds can't be closed.
        }
      }
    }

    // --- MOTOR ROTATION SWITCH ---
    if ( actionProcessMotorRotation && mtrBlinds.IsRunning) {
      // The motor completed a rotation. Do checks and take action if necessary.

      if (mtrBlinds.Action == actBlindsClose) {
        // Blinds are CLOSING. Decrease rotation count.
        if (mtrBlinds.currentPosition > 0) {
          mtrBlinds.currentPosition--;             // Blinds are closing. Decrease count (only down to zero).
          //Serial.print(" >> Motor Rotation: Count Rotations (d) - "); Serial.println(mtrBlinds.currentPosition);
        }

//TelnetStream.print(" - loopMA: Closing.. Pos="); TelnetStream.print(mtrBlinds.currentPosition); 
//TelnetStream.print(" Offset=" ); TelnetStream.println(appConfig.ClosedOffset);

        if (mtrBlinds.currentPosition == 0 && appConfig.RotationLimits && mtrBlinds.Owner == ownMQTT) {
          // The rotation count decreased to zero, we're stopping based on nr of rotations, and MQTT is closing the blinds.

          if ( appConfig.ClosedOffset < 0 || appConfig.ClosedOffset == 999 ) {
            // Ignore the rotation count when closing. Motor should be stopped by either bottom limit switch or else run timer.
            #ifdef TELNET_DEBUG
              TelnetStream.print(" - loopMA: ClosedRotationOffset<0||999, currentPos = " ); TelnetStream.println(mtrBlinds.currentPosition);
            #endif

          } else {
            if ( appConfig.ClosedOffset > 0 ) {
              // The ROTATION offset allows additional rotations for the Close limit switch to be reached. 
              // Stop the motor after the offset in case it is still running. (only if run by MQTT) 
              if (!actionClosedStopOffset) {
                // This is the first rotation when the closed position is reached. Initiate the offset counter, and continue.
                countClosedOffset = 1;
                actionClosedStopOffset = true; 
                #ifdef TELNET_DEBUG
                  TelnetStream.print(" - loopMA: ClosedOffset>0, actionClosedOffset=false, currentPos = " ); TelnetStream.println(mtrBlinds.currentPosition);
                #endif

              } else {
                if ( countClosedOffset < appConfig.ClosedOffset ) {
                  // The additional number of offset rotations are still below the offset. Continue..
                  countClosedOffset++;
                  #ifdef TELNET_DEBUG
                    TelnetStream.print(" - loopMA: ClosedRotationOffset>0, actionClosedOffset=true, cntOffset<offset, cntRotationOffset = " ); TelnetStream.println(countClosedOffset);
                  #endif
                } else {
                  // The additional number of ofsett rotations exceeds the offset. Stop the motor.
                  #ifdef TELNET_DEBUG
                    TelnetStream.print(" - loopMA: ClosedRotationOffset>0, actionClosedOffset=true, cntOffset >= offset, STOP, cntRotationOffset = " ); TelnetStream.println(countClosedOffset);
                  #endif
                  xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
                  actionStopMotor = true;
                  mtrBlinds.AllowToRun = false;
                  xSemaphoreGive(semBlindsCheck);
                }
              }

            } else {
              // Close position reached. No offset defined, so stop motor.
              #ifdef TELNET_DEBUG
                TelnetStream.print(" - loopMA: close, pos=0, NO OFFSET, STOP, curPos = " ); TelnetStream.println(mtrBlinds.currentPosition);
              #endif
              xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
              mtrBlinds.AllowToRun = false;
              actionStopMotor = true;
              xSemaphoreGive(semBlindsCheck);
            }
          }
        }

      } else if (mtrBlinds.Action == actBlindsOpen) {
        // Blinds are OPENING. Increase rotation count.
        mtrBlinds.currentPosition++;               // Blinds are opening. Increase count.
        //Serial.print(" >> ISR Motor: Count Rotations (u) - "); Serial.println(mtrBlinds.currentPosition);

        if (mtrBlinds.currentPosition >= appConfig.Open_MaxRotations && appConfig.RotationLimits && mtrBlinds.Owner == ownMQTT) {
          // Blinds are opened by MQTT. Blinds rotation reached full open position. Stop motor. (Button open can exceed count limit)
          //Serial.print(" >> ISR Motor: Stop motor. MAX Open rotations reached. "); Serial.print(mtrBlinds.currentPosition);
          xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
          mtrBlinds.AllowToRun = false;
          actionStopMotor = true;
          xSemaphoreGive(semBlindsCheck);
        }
      }

      if (mtrBlinds.Owner == ownMQTT && mtrBlinds.targetPosition > 0) {
        if ( (mtrBlinds.Action == actBlindsOpen && mtrBlinds.currentPosition >= mtrBlinds.targetPosition ) || (mtrBlinds.Action == actBlindsClose && mtrBlinds.currentPosition >= 0 && mtrBlinds.currentPosition <= mtrBlinds.targetPosition ) ) {
          //Serial.print(" >> ISR Motor: Stop motor. TARGET rotations reached. "); Serial.println(mtrBlinds.currentPosition);
          // Blinds are opened/closed by MQTT. Blinds reached the specified target position. Stop motor.
          xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
          mtrBlinds.AllowToRun = false;
          actionStopMotor = true;
          xSemaphoreGive(semBlindsCheck);
        }
      }
      actionProcessMotorRotation = false;
    }

    // --- OPEN BUTTON ---
    if ( btnBlindsOpen.Changed ) {
      // The OPEN switch status changed.
      if ( mtrBlinds.IsRunning ) {
        // The OPEN button status changed while the motor is running. Stop the motor.
        // This makes it possible to stop the motor by pressing any button (again) in any direction.
        btnBlindsOpen.lastStopTime = millis();             // Wait sufficient time before reacting to the button again.
        actionStopMotor = true;
        btnBlindsOpen.Changed = false;
        #ifdef TELNET_DEBUG
          TelnetStream.print(" - loop: OPEN button changed while running. Motor STOP - " );
          TelnetStream.println(btnBlindsOpen.lastDebounceTime);
        #endif
      } else {
        // The Motor is NOT running.
        unsigned long MillisNow = millis();
        if ( MillisNow - btnBlindsOpen.lastStopTime > 1000 ) {
          swcBlindsOpen.Set = (digitalRead(pin_StopOpen) == LOW);     // Confirm the blinds open status before proceeding
          if ( digitalRead(pin_BtnOpen) == LOW ) {
            // OPEN button was PRESSED (buttons are normal high, and will be pulled low when pressed). 
            #ifdef TELNET_DEBUG
              TelnetStream.print(" - loop: OPEN BUTTON pressed @ " ); TelnetStream.println(MillisNow);
              TelnetStream.print(" -   : diff= " ); TelnetStream.println(MillisNow - btnBlindsOpen.lastDebounceTime);
              TelnetStream.print(" -   : debounced? " ); TelnetStream.println(MillisNow - btnBlindsOpen.lastDebounceTime > appConfig.DebounceDurSwitches);
            #endif
            if ( !mtrBlinds.IsRunning && !swcBlindsOpen.Set ) {
              // Motor is not running, and blinds not fully open (limit switch not set). Ignore rotation position when using button.
              // START Motor
              xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
              mtrBlinds.Action = actBlindsOpen;
              mtrBlinds.AllowToRun = true;
              mtrBlinds.Owner = ownButton;
              xSemaphoreGive(semBlindsCheck);
              MotorStart();
            }
            else {
              #ifdef TELNET_DEBUG
                TelnetStream.println(" - loop: OPEN BUTTON pressed. NOT OPENING: motor running OR blinds fully open" );
              #endif
              DoBleepTimes = 2;       // Can't run as requested
            }          // Can't run as requested
          }
          btnBlindsOpen.Changed = false;
        }
      }
    }

    // --- CLOSE BUTTON ---
    if ( btnBlindsClose.Changed ) {
      // The CLOSE switch status changed.
      if ( mtrBlinds.IsRunning ) {
        // The CLOSE button status changed while the motor is running. Stop the motor.
        // This makes it possible to stop the motor by pressing any button (again) in any direction.
        btnBlindsClose.lastStopTime = millis();              // Wait sufficient time before reacting to the button again.
        actionStopMotor = true;
        btnBlindsClose.Changed = false;
        #ifdef TELNET_DEBUG
          TelnetStream.print(" - loop: CLOSED button changed while running. Motor STOP - " );
          TelnetStream.println(btnBlindsClose.lastDebounceTime);
        #endif
      } else {
        // The Motor is NOT running.
        if ( millis() - btnBlindsClose.lastStopTime > 1000 ) {
          swcBlindsClosed.Set = (digitalRead(pin_StopClosed) == LOW);   // Confirm blinds close status before proceeding
          if ( digitalRead(pin_BtnClose) == LOW ) {
            // CLOSE button was PRESSED (buttons are normal high, and will be pulled low when pressed). 
            #ifdef TELNET_DEBUG
              TelnetStream.print(" - loop: CLOSE BUTTON pressed @ " ); TelnetStream.println(millis());
            #endif
            if ( !mtrBlinds.IsRunning && !swcBlindsClosed.Set ) { 
              // Motor is not running, and blinds not fully closed, i.e. limit switch not set. (Ignore rotation position when using button)
              // START Motor
              xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
              mtrBlinds.Action = actBlindsClose;
              mtrBlinds.AllowToRun = true;
              mtrBlinds.Owner = ownButton;
              xSemaphoreGive(semBlindsCheck);
              MotorStart();
            }
            else {
              #ifdef TELNET_DEBUG
                TelnetStream.println(" - loop: CLOSE BUTTON pressed. NOT CLOSIING: motor running OR already closed" );
              #endif
              DoBleepTimes = 2;       // Can't run as requested
            }
          }
        }
        btnBlindsClose.Changed = false;
      }
    }

    // --- MQTT ---
    if ( mqttBlindsAction.NewAction ) {
      // --- MQTT action received ---
      // -- OPEN
      if ( mqttBlindsAction.Action == actBlindsOpen ) {
        #ifdef TELNET_DEBUG
          TelnetStream.println(" - loop: MQTT OPEN blinds" );
        #endif
        if ( !mtrBlinds.IsRunning && !swcBlindsOpen.Set ) { 
          // Only OPEN the blinds if they are not already opened.
          mtrBlinds.Action = mqttBlindsAction.Action;
          mtrBlinds.AllowToRun = true;
          mtrBlinds.Owner = ownMQTT;
          MotorStart();
        }
      }
      // -- CLOSE
      else if ( mqttBlindsAction.Action == actBlindsClose ) {
        #ifdef TELNET_DEBUG
          TelnetStream.println(" - loop: MQTT CLOSE blinds" );
        #endif
        if ( !mtrBlinds.IsRunning && !swcBlindsClosed.Set ) { 
          // Only CLOSE the blinds if they are not already closed.
          mtrBlinds.Action = actBlindsClose;
          mtrBlinds.AllowToRun = true;
          mtrBlinds.Owner = ownMQTT;
          MotorStart();
        }
      } 
      // -- STOP
      else if ( mqttBlindsAction.Action == actBlindsStop ) {
        #ifdef TELNET_DEBUG
          TelnetStream.println(" - loop: MQTT STOP" );
        #endif
        actionStopMotor = true;
      }
      mqttBlindsAction.Action = actUNDEF;
      mqttBlindsAction.NewAction = false;
    }

    // --- A stop was triggered (could be: limit switch, button release, rotation position, timer limit, current limit, MQTT)
    if ( actionStopMotor ) {
      countClosedOffset = 0;                    // Reset offset values (MQTT)
      actionClosedStopOffset = false;                   // Reset offset values (MQTT)
      xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
      actionStopMotor = false;
      xSemaphoreGive(semBlindsCheck);
      Serial.printf(" - loop: StopAction.   IsRunning=%i\n", mtrBlinds.IsRunning );
      MotorStop(); 
      SaveCurrentPosition(mtrBlinds.currentPosition);   // Remember the current pos in persistent memory, for use after restart. 
    }
  }   // loop
}

/**************************************************************************
 *  MotorStart
 *  - Soft-start the motor in the indicated direction (based on action). 
 **************************************************************************/
void MotorStart() {
  int pwmChannel = -1;
  bool blindsWasClosed = swcBlindsClosed.Set;

  if (mtrBlinds.Action == actBlindsOpen ) {
    pwmChannel = pwmChannel_Open;
    Serial.print(" => MotorStart OPEN: IsRunning="); Serial.println(mtrBlinds.IsRunning);
  } else if (mtrBlinds.Action == actBlindsClose) {
    pwmChannel = pwmChannel_Close;
    Serial.print(" => MotorStart CLOSE: IsRunning="); Serial.println(mtrBlinds.IsRunning);
  }

  if ( mtrBlinds.AllowToRun && !mtrBlinds.IsRunning && pwmChannel > -1 ) {              // Make sure the motor is not running already, and a valid action is set.
    mtrBlinds.IsRunning = true;

    if (mtrBlinds.Owner == ownMQTT && appConfig.Open_Duration > 0) {
      // If remotely opened (MQTT), and timeout configured, then set a timer to automatically stop blinds opening after configured duration.
      timerAlarmWrite(tmrBlindsOpen, (appConfig.Open_Duration * 1000000), false);       // fire timer after x seconds (in micro-seconds). Once.
      timerRestart(tmrBlindsOpen);                                                      // reset the timer counter.
      timerAlarmEnable(tmrBlindsOpen);                                                  // start the timer to automatically stop the motor after x seconds. Remove if upper limit switch exists.
    }
    if (appConfig.MaxRunDuration > 0) {
      // Start timer to limit max time motor can run.
      timerAlarmWrite(tmrBlindsMaster, (appConfig.MaxRunDuration * 1000000), false);    // fire timer after x seconds (in micro-seconds). Once.
      timerRestart(tmrBlindsMaster);                                                    // reset the timer counter.
      timerAlarmEnable(tmrBlindsMaster);                                                // start the timer to automatically stop the motor after x seconds. Remove if upper limit switch exists.
    }

    // START MOTOR: Set ENABLE pins on motor driver board (both Left and Right must be enabled for motor to run)
    digitalWrite(pin_LEN, HIGH);
    digitalWrite(pin_REN, HIGH);
    // Do a soft-start. Start with a low PWM dutycycle and increase to 100% over a short period.
    for (int dutyCycle=50; dutyCycle <= 255; dutyCycle++) {  
      // Keep checking to ensure the motor was not stopped during the ramp-up.
      if (mtrBlinds.AllowToRun) {
        ledcWrite(pwmChannel, dutyCycle);
        delay(5);
      } else {
        // Some interrupt stopped the motor.
        break;
      }
    }
  }

  xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
    swcBlindsClosed.Set = (digitalRead(pin_StopClosed) == LOW);
    swcBlindsOpen.Set = (digitalRead(pin_StopOpen) == LOW);             
  xSemaphoreGive(semBlindsCheck);

  if (mtrBlinds.IsRunning && blindsWasClosed && mtrBlinds.Action == actBlindsOpen) {
    mqttPublishBlindsState = true;              // Set flag to publish interim blinds open status.
  }
  Serial.print(" - Motor started: IsRunning="); Serial.print(mtrBlinds.IsRunning); 
  Serial.print(" WasClosed="); Serial.print(blindsWasClosed);
  Serial.print(" Action="); Serial.println(mtrBlinds.Action);
  
}

/**************************************************************************
 *  MotorStop
 *  - Stop the motor e.g. when a limit switch was triggered.
 *  - Read limit switches to sync status with reality again.
 *  - Set flag to publish Blinds status.
 **************************************************************************/
void MotorStop() {
  bool wasMotorRunning = mtrBlinds.IsRunning;
  // Disable both Right and Left "enable" Pins on motor driver board, and disable PWM. 
  // (always do without checks, as safety measure).
  digitalWrite(pin_REN, LOW);                                       // Set driver card enable pins low to immediately stop the motor.
  digitalWrite(pin_LEN, LOW);                                       // Set driver card enable pins low to immediately stop the motor.
  ledcWrite(pwmChannel_Open, 0);                                    // Stop the "OPEN" PWM channel.
  ledcWrite(pwmChannel_Close, 0);                                   // Stop the "CLOSE" PWM channel.
  timerStop(tmrBlindsOpen);                                         // Stop the "open" timer, just in case.
  timerStop(tmrBlindsMaster);                                       // Stop the "master" timer, just in case.
  // Reconfirm current situation.
  xSemaphoreTake(semBlindsCheck, portMAX_DELAY);
    swcBlindsClosed.Set = (digitalRead(pin_StopClosed) == LOW);     // If "CLOSED" limit switch closed then normal high is pulled low.
    swcBlindsOpen.Set = (digitalRead(pin_StopOpen) == LOW);         // If "OPEN" limit switch closed then normal high is pulled low.
    mtrBlinds.IsRunning = false;                                    // Clear flag that motor is running. Now it can be started again.
    mtrBlinds.Owner = ownUNDEF;                                     // Clear the previous motor action initiator.
    mtrBlinds.Action = actUNDEF;                                    // Clear the previous motor action.
  xSemaphoreGive(semBlindsCheck);

  mqttPublishBlindsState = true;                                    // Always publish the latest/updated state, regardless if motor was running.
  Serial.printf(" => MotorStop: Closed=%i, FullOpen=%i, WasRunning=%i\n", swcBlindsClosed.Set, swcBlindsOpen.Set, wasMotorRunning);
  #ifdef TELNET_DEBUG
    TelnetStream.print("MotorStop. Close limit:  " ); TelnetStream.print(swcBlindsClosed.Set);
    TelnetStream.print(" ClosedRotationOffset=" ); TelnetStream.println(appConfig.ClosedOffset);
  #endif
}

/**************************************************************************
 *  Bleep  (overloaded)
 *  Bleep active buzzer based on provided Number and Duration (defaulted to BleepTimeOn)
 **************************************************************************/
void MyBleep(int NrBleeps) {
  for (int i=0; i<NrBleeps; i++) {
    digitalWrite(pin_Buzzer, HIGH);
    delay(BleepTimeOn);
    digitalWrite(pin_Buzzer, LOW);
    if (i<NrBleeps) {
      delay(BleepTimeOff);
    }
  }
}

/**************************************************************************
 *  Bleep  (overloaded)
 *  Parse the provided parameter string, bleep based on provided pattern.
 *  - Format: 
 *       "AxB.B.B..."
 *    A:  Number of repititions. Single digit (only), i.e. range 1-9
 *    B:  Duration of bleep. Value is multiplied with the "BleepTimeOn" constant.
 *        1 = short, 2 = longer, etc. 9999 max value.
 *  Example:
 *    "1x1.0.1.2.1"             ->  beep-silent-beep-beeeep-beep 
 *    "2x1.1.1.3.3.3.1.1.1"     ->  Repeat SOS twice  
 **************************************************************************/
void Bleep (const String& BleepMsg) {
  char cDur[5];         // supports 9999 max (4 digits)
  int iDur = 0;
  int NrRepeats = 0;    // supports 9 max (single digit)
  int k = 0;
  int dataLength = BleepMsg.length();   // 

  //Serial.print("Bleep - ");   Serial.println(BleepMsg);

  if ( dataLength >=2 ) {
    if ( isDigit(BleepMsg.charAt(0)) ) {
      NrRepeats = int(BleepMsg.charAt(0))-48;     // int() returns ascii value of single char, 48 less is the actual int value.
      for (int i=1; i<=NrRepeats; i++) {
        k = 0;
        for (int j=2; j<=dataLength; j++) {
          if ( isDigit(BleepMsg.charAt(j)) && k<4 ) {
            cDur[k++] = BleepMsg.charAt(j);
          } 
          if (BleepMsg.charAt(j) == '.' || j==dataLength ) {
            cDur[k] = '\0';
            iDur = atoi(cDur);
            if (iDur>0) {
              // Bleep for the indicated duration, then off for the default wait time.
              digitalWrite(pin_Buzzer,HIGH);
              delay( BleepTimeOn*iDur );
              digitalWrite(pin_Buzzer,LOW);
              delay(BleepTimeOff);
            } else {
              // create silent space
              delay(300);
            }
            k = 0;
          }
        }
        if (i < NrRepeats) 
          delay(200);           // Wait a bit between repeats
      }
    }
  }
}
