# ESP32_MQTT_Motor_Control
This is an ESP32-based project intended to automate the opening and closing of my livingroom blinds, making use of a 12V wiper motor controlled by MQTT messages received from Home Assistant (HA). An additional open/close button allows for manual control of the blinds in situations when it is more convenient.    
    
The same program can be used though to control any DC motor in combination with the appropriate hardware.    
    
#### Concepts covered by the design:
   1. Using MQTT the blinds can be opened, closed, or opened to a certain position (percentage of full open).
   2. Using settings stored in SPIFFS certain behaviour of the motor can be configured (through MQTT) on the fly.
   3. Supports over-the-air (OTA) updates once the first version of the software is deployed to the ESP32.
   4. Supports UDP Telnet debugging.
   5. Runs motor controler functions in dedicated thread (multitasking).
   6. Using PWM a smooth start (softstart) is created by ramping up the motor revolutions over time.
   7. Over-current protection is based on feedback provided by the motor driver.
   8. A buzzer provides audible feedback in error conditions (or whenever HA sends a buzzer notification).
    
## Functional Description
If all works as intended, the following should happen:
   - Periodic temperature and luminosity sensor readings are uploaded to HA.    
     The frequency of upload, and minimum delta between readings, can be configured in the settings.    
   - A HA automation decides when the blinds should be opened, and then publishes the appropriate "open" MQTT message.    
     My automation has two triggers: (1) when the lux exceeds a certain value or (2) at a certain time of day, whichever comes first. This particular room faces east, so to prevent too much sunlight into the room in the morning the blinds initially only opens to 50% of the fully open position.    
   - Another HA automation decides when the blinds should be closed (lux drops below a threshold), and then publishes a "close" MQTT message.    
   - In addition to the mentioned automations it is also possible to create buttons in HA to open or close the blinds ad hoc through HA. (see [HA Examples](https://github.com/JJFourie/ESP32_MQTT_Motor_Control/tree/main/HomeAssistant#readme) )        
   - If either of the "open" or "closed" limit switches are triggered then the motor stops.    
   - In addition to using MQTT to control the motor, it is also possible to open or close the blinds manually using a rocker switch mounted on the motor housing.    
    
#### Additional Features

   1. Because the motor rotations are detected/monitored, it is possible to set the maximum number of *rotations* that the motor is allowed to make from a fully closed position. And as these maximum number of rotations represents fully open, it is then possible to instruct the blinds to open e.g. halfway (50% of the full number of rotations).     
   2. As a safety measure it is possible to configure the maximum *duration* that the motor is allowed to run at a time. This is to prevent that the motor runs forever if something goes wrong (e.g. the cord breaks) and the "open" limit switch is never reached.    


## Technical Detail

#### BOM
   1. 12V wiper motor.    
   2. ESP32.    
   3. [12V 60W PSU](https://nl.aliexpress.com/item/4000079945543.html?spm=a2g0o.order_list.0.0.503e79d2w3pDfm&gatewayAdapt=glo2nld). I went for a split dual-channel supply, one output providing power for the motor and the other feeding the ESP32 (via a buck converter).     
   4. 12V to 3.3V buck converter. ( [LM2596](https://lygte-info.dk/review/Power%20Adjustable%20buck%20converter%20LM2596%20UK.html) )    
   5. Motor driver. ( [BTS7960 (IBT-2)](https://www.handsontec.com/dataspecs/module/BTS7960%20Motor%20Driver.pdf) )    
   6. SPDT momentary rocker switch. I strongly advice to use something decent and don't play it cheap. Like me. Contact bounce drove me crazy and I ended up with compromises and messy code.    
   7. Active buzzer.    
   8. Housing.    
   9. Reed switches (wall) and small neodymium magnets (blinds) to use as "open" and "close" limit switches.    


<br>       
#### Sensors/Pins used:   
    
Type | Sensor | Function | GPIO
--- | --- | --- | ---
Switch | Button | Close blinds |  16
Switch | Button | Open blinds |  17
Switch | Reed | Full Close position |  18
Switch | Reed | Full Open position |  19
Switch | Slipring | Motor rotation |  13
I2C | [TSL2561](https://learn.adafruit.com/tsl2561) | Luminosity (Lux) | 21 (SDA) <br>  22 (SCL)
I2C | [AM2320](https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf) | Temperature & Humidity |  21 (SDA) <br>  22 (SCL) 
DO | [Active Buzzer]() | Beeps when error, MQTT msg from HA |  5
IBT-2 | PWM | Clock pulses to determine rotation speed |  25 (Right PWM) <br> 26 (Left PWM)
IBT-2 | EN | Enables rotation in a specific direction |  14 (Right Enable) <br> 27 (Left Enable) 
IBT-2 | Current sensor | Protects motor driver from over current |  32

    
### Issues
Remarks on some aspects of the design:    
   1. **Wiper Motor**    
      I chose for a wiper motor because the blinds I wanted to control are rather heavy, and are of the type that must be pulled up with a cord when opened (i.e. not slats). The gear mechanism of a wiper motor is designed to prevent the wind from moving the wipers while driving. So the motor can turn the output axle, but the axle can't turn the motor. This comes in very handy in this case, as no additional breaking mechanism was necessary to lock the blinds when in the open position.    
      **NOTE** that some rear wiper motors have a mechanism that causes the output axle to make semi-circles (only turn 180 degrees) instead of the desired full rotations!
   2. **Rotations**    
      Another advantage of (all?) wiper motors is that they have slip rings that allow the car electronics to know when the blinds are in the "close" position. This mechanism enables us to detect and count the motor rotations in order to keep track of the position of the blinds. In my case I had to open the gear housing and remove one of the connections, as it made a short to the 12V slip ring for a short period while the motor was running. After removing the slip ring connector it was possible to simply connect the remaining pin to a GPIO, and monitor when it is pulled down to ground to indicate one axle rotation.    
      An alternative to using the internal wiper motor sliprings and pins could be to put a very small neodymium magnet on the motor axle, and then use a hall-effect transistor to detect rotations.
   3. **Momentary rocket switch**    
      The idea was to make it possible to open/close the blinds manually using push buttons, as it just could happen that a phone/laptop/HA client is not at hand at the moment you stand in front of the window, and it would be just so much easier to press a button to quickly manage the blinds.    
      By adding a double-throw momentary rocker switch it becomes easy to move the blinds in the desired direction for as long as the button is pressed. A la electric car windows!    
      Just be sure to use quality buttons, and/or properly debounce them, as else you will not be able to use ESP32 interrupts on the GPIO pins to detect button changes. The related code can then also be cleaned up considerably.
   4. **Output Gear**    
      To connect the blinds pull cord to the wiper motor I used a [3D printer timing belt gear](https://www.google.com/search?q=3d+printer+drive+belt+gear&tbm=isch&ved=2ahUKEwiUocK_5-T6AhVNOewKHXt2BAQQ2-cCegQIABAA&oq=3d+printer+drive+belt+gear&gs_lcp=CgNpbWcQAzoHCAAQgAQQGFDMDlifFGCGFmgAcAB4AIABbIgBggOSAQM1LjGYAQCgAQGqAQtnd3Mtd2l6LWltZ8ABAQ&sclient=img&ei=2QBMY9TQBc3ysAf77JEg&bih=703&biw=1536). I drilled two "dimpels" into the wiper motor axle for the grub screws of the gear. The gear's splines provide sufficient grip for the cord to not slip when the blinds are open(ed).


#### Wire Diagram

![Fritzing Diagram](https://github.com/JJFourie/ESP32_MQTT_Motor_Control/blob/main/Images/Fritzing-Blinds_Motor_Control.jpg)

