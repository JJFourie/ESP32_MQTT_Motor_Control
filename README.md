# ESP32_MQTT_Motor_Control
This is an ESP32-based project intended to automate the opening and closing of my livingroom blinds, making use of a 12V wiper motor controlled by MQTT messages received from Home Assistant (`HA`). An additional open/close button allows for manual control of the blinds in situations when it is more convenient.    
    
The same program can be used though to control any DC motor in combination with the appropriate hardware.    
    
#### Concepts covered by the design:
   1. Using *MQTT* the blinds can be managed (opened, closed) remotely from `HA` or any MQTT client.
   2. Using settings stored in *SPIFFS*, certain behaviour of the motor can be configured (through MQTT) on the fly.
   3. Supports over-the-air (*OTA*) updates once the first version of the software is deployed to the ESP32.
   4. Supports UDP Telnet debugging.
   5. Runs the motor controler functions in a dedicated EPS32 thread (multitasking).
   6. Uses *PWM* to smoothly start (softstart) the motor by ramping up the motor revolutions over time.
   7. Over-current protection is based on feedback provided by the motor driver.
   8. A buzzer provides audible feedback in error conditions (or whenever `HA` sends a buzzer notification).
   9. A lux sensor measures the light level, making this an autonomous unit to automate the blinds at the start and end of day.
    
## Functional Description
If all works as intended, the following should happen:
   - Periodic temperature and luminosity sensor readings are uploaded to `HA`.    
     The frequency of upload, and minimum delta between readings, can be configured in the configuration/settings.    
   - A `HA` automation decides when the blinds should be opened, and then publishes the appropriate "open" MQTT message.    
     My automation has two triggers: (1) when the lux exceeds a certain value or (2) at a certain time of day, whichever comes first. This particular room faces east, so to prevent too much sunlight into the room in the morning the blinds initially only opens to 50% of the fully open position.    
   - Another HA automation decides when the blinds should be closed (lux drops below a threshold), and then publishes a "close" MQTT message.    
   - In addition to the mentioned automations it is also possible to create buttons in `HA` to "manually" open or close the blinds ad hoc from e.g. your phone. (see [HA Examples](https://github.com/JJFourie/ESP32_MQTT_Motor_Control/tree/main/HomeAssistant#readme) )        
   - If either of the "open" or "closed" limit switches are triggered then the motor stops.    
   In my current implementation I only have a "bottom" limit switch, that is used to reset/confirm the blinds position each time the blinds are closed. The "open" position is based on the number of rotations from the closed position.    
   - In addition to using MQTT to control the motor, it is also possible to open or close the blinds manually using a rocker switch mounted on the motor housing.    
    
#### Additional Features

   1. Because the motor rotations are detected/monitored, it is possible to set the maximum number of *rotations* that the motor is allowed to make from a fully closed position. And as these maximum number of rotations represents fully open, it is then possible to instruct the blinds to open e.g. halfway (50% of the full number of rotations).     
   2. As a safety measure it is possible to configure the maximum *duration* that the motor is allowed to run at a time. This is to prevent that the motor runs forever if something goes wrong (e.g. the blinds cord breaks) and the "open" limit switch is never reached.    
    
<br>
    
-----    
    
    
## MQTT
Using the `"mqtt.publish"` service, the following commands can be send:
    
### Motor Actions
Below is a list of MQTT *commands* that control the blinds:    
    
Topic: `livingroom/blinds/action`
    
Payload | Description
-- | --
`open` | Open blinds fully
`open:50` | Open the blinds to the indicated percentage. (e.g. 50%)
`close` | Close the blinds if not already closed.
`stop` | Stop the blinds if the motor is currently running.

### App Configuration Commands
Below is a list of MQTT *commands* that control the behaviour of the ESP32:    
    
Topic: `livingroom/blinds/appcmd`
    
Payload | Description
-- | --
`restart`      | Restart ESP32
`getstate`     | Report the current state and telemetry values (RSSI, Memory, ..)
`getconfig`    | Report the current application configuration
`StateInterval:<minutes>`   | Set the interval between state updates (0 = disabled)
`LuxInterval:<minutes>`     | Set the interval between Lux updates (0 = disabled)
`TempInterval:<minutes>`    | Set the interval between Temperature updates (0 = disabled)
`RotationLimits:<true/false>`     | Set if blinds is considered open/closed on rotations (true) or at limit switches (false) 
`DebounceDurSwitches:<mseconds>`  | Set the debounce time for Buttons and Limit switches (milliseconds)
`DebounceDurMotor:<mseconds>`     | Set the debounce time for the motor rotation switch (milliseconds)
`OpenDuration:<seconds>`          | Set max duration the motor will run when OPENING the blinds (0 = check and timer disabled)
`MaxRunDuration:<seconds>`        | Set max duration the motor may run in ANY direction (0 = check and timer disabled)
`MaxOpenRotations:<count>`        | Set max number of axis rotations that blinds can open (0 = disabled)
`MinLuxReportDelta:<lux>`         | Set the minimum difference in Lux level before publishing MQTT (0 = no delta trigger, interval only)
`MaxCurrentLimit:<value>`         | Set max load current motor is allowed to draw (raw analog value) (0 = disabled)
`AllowRemoteControl:<true/false>` | Set allow control of Blinds using MQTT (true), else (false)
`AllowRemoteBleep:<true/false>`   | Set if (MQTT) Bleep notifications must be processed (true) or ignored (false)
`WiFiSetup:SSID/password`         | Set the SSID and password to be used ("default" for hardcoded defaults)
    
### Published Messages
Below is a list of MQTT messages published by the ESP32:    
    
Message | Description
-- | --
`livingroom/blinds/state`      | Current Blinds state (open/closed + %)
`livingroom/blinds/config`     | Configuration settings (JSON settings)
`livingroom/blinds/app_state`  | Telemetry metrics (JSON parameters)
`livingroom/lux/state`         | Current Lux reading
`livingroom/temperature/state` | Current temperate reading
`livingroom/humidity/state`    | Current humidity reading

    
### Bleep
The active buzzer can be used to send general notifications, in any combination of duration and number of pulses.    
Format of the payload:  `"AxB.B.B..."`    
Where    
    A:  Number of repititions. Single digit (only), i.e. range 1-9    
    B:  Duration of bleep. Value is multiplied with the "BleepTimeOn" constant in the code.    
        1 = short, 2 = longer, etc. 9999 max value.    
    
Topic: `all/notify/bleep`    
        
Payload | Description
-- | --
`1x1.0.1.2.1`           | do once: beep-silent-beep-beeeep-beep
`2x1.1.1.3.3.3.1.1.1`   | repeat SOS twice
    
<br>
    
-----    
    
## Technical Detail

#### BOM
   1. 12V wiper motor. Or DC motor of your liking.    
   2. ESP32.    
   3. [12V 60W PSU](https://nl.aliexpress.com/item/4000079945543.html?spm=a2g0o.order_list.0.0.503e79d2w3pDfm&gatewayAdapt=glo2nld). I went for a split dual-channel supply, one output providing power for the motor and the other feeding the ESP32 (via a buck converter).     
   4. 12V to 3.3V buck converter. ( [LM2596](https://lygte-info.dk/review/Power%20Adjustable%20buck%20converter%20LM2596%20UK.html) )    
   5. Motor driver. ( [BTS7960 (IBT-2)](https://www.handsontec.com/dataspecs/module/BTS7960%20Motor%20Driver.pdf) )    
   6. SPDT momentary rocker switch. I strongly advice to use something decent and don't play it cheap. Like me. Contact bounce drove me crazy and I ended up with compromises and messy code.    
   7. Active buzzer. An alternative could be using a passive buzzer that will allow you to play around with tones in your notification.    
   8. Housing.    
   9. Reed switches (for the window frame) and maybe small neodymium magnets (glued to blinds) to use as "open" and "close" limit switches.    

    
<br>    
    
#### Sensors/Pins used:   
    
Type | Sensor | Function | GPIO
--- | --- | --- | ---
Switch | Button | Close blinds |  16
Switch | Button | Open blinds |  17
Switch | Reed | Full Close position |  18
Switch | Reed | Full Open position |  19
Switch | Slipring contact| Motor rotation |  13
I2C | [TSL2561](https://learn.adafruit.com/tsl2561) | Luminosity (Lux) | 21 (SDA) <br>  22 (SCL)
I2C | [AM2320](https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf) | Temperature & Humidity. Optional |  21 (SDA) <br>  22 (SCL) 
DO | [Active Buzzer]() | Beeps when error, MQTT msg (from HA)  |  5
IBT-2 | PWM | Clock pulses to determine rotation speed |  25 (Right PWM) <br> 26 (Left PWM)
IBT-2 | EN | Enables rotation in a specific direction |  14 (Right Enable) <br> 27 (Left Enable) 
IBT-2 | Current sensor | Protects motor driver from over current |  32

    
### Notes
Remarks on some aspects of the design:    
   1. **Wiper Motor**    
      I chose for a wiper motor because the blinds I wanted to control are rather heavy, and are of the type that must be pulled up with a cord when opened (i.e. not slats that can be rotated). The gear mechanism of a wiper motor is designed to prevent the wind from moving the wipers while driving. This means that the motor can turn the output axle, but the axle can't turn the motor. This behaviour comes in very handy in this case, as no additional breaking mechanism was necessary to lock the blinds when in the open position.    
      **NOTE** that some rear wiper motors have a mechanism that causes the output axle to only rotate through 180 degrees instead of the desired full 360 degree rotations!    
   2. **Rotation Monitoring**    
      Another advantage of (all?) wiper motors is that they have slip rings that allow the car electronics to know when the blinds are in the "close" position. This mechanism enables us to detect and count the motor rotations in order to keep track of the position of the blinds. In my case I had to open the gear housing and remove one of the slip ring connections, as it made a short to the 12V slip ring for a short period while the motor was running. After removing this connector it was possible to simply connect the remaining connector to a GPIO pin, and monitor when it is pulled down to ground to indicate one axle rotation. In my case this connector bounces for quite a long time during rotations, so (software?) debouncing is essential.    
      An alternative to using the internal wiper motor sliprings and connector could be to e.g. put a very small neodymium magnet on the motor axle, and then use a hall-effect transistor to detect rotations.    
            Note that monitoring the motor rotations is not essential to the design. It just provides a nice and easy way to know where the blinds are, and thus to posision the blinds much more flexibly than when using fixed limit switches.    
   3. **Momentary rocker switch**    
      The idea was to make it possible to open/close the blinds manually using push buttons, as it just could happen that a phone/laptop/HA client is not at hand at the moment you stand in front of the window, and it would be just so much easier to press a button to quickly manage the blinds. This also makes it possible to open/close the blinds when you have a system outtage (of course this is something that will never happen..).    
      By adding a double-throw momentary rocker switch it becomes easy to move the blinds in the desired direction for as long as the button is pressed. A la electric car windows!    
      Just be sure to use quality buttons, and/or properly debounce them, as else you will not be able to use ESP32 interrupts on the GPIO pins to detect button changes. The related code can then also be cleaned up considerably.    
   4. **Output Gear**    
      To connect the blinds pull cord to the wiper motor I used a [3D printer timing belt gear](https://www.google.com/search?q=3d+printer+drive+belt+gear&tbm=isch&ved=2ahUKEwiUocK_5-T6AhVNOewKHXt2BAQQ2-cCegQIABAA&oq=3d+printer+drive+belt+gear&gs_lcp=CgNpbWcQAzoHCAAQgAQQGFDMDlifFGCGFmgAcAB4AIABbIgBggOSAQM1LjGYAQCgAQGqAQtnd3Mtd2l6LWltZ8ABAQ&sclient=img&ei=2QBMY9TQBc3ysAf77JEg&bih=703&biw=1536). I drilled two "dimpels" into the wiper motor axle for the grub screws of the gear to take hold. The gear's splines provide sufficient grip for the cord to not slip when the blinds are open(ed).


#### Wire Diagram

![Fritzing Diagram](https://github.com/JJFourie/ESP32_MQTT_Motor_Control/blob/main/Images/Fritzing-Blinds_Motor_Control.jpg)

