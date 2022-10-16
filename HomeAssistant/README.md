# Home Assistant

This document contains notes on the related components and functionality I added to [Home Assistant](https://www.home-assistant.io/) (HA).    

Note that    
- in HA there are multiple ways to do things, and what is described below may not be the best way, or may be outdated by the time I finish writing as HA is evolving all the time.
- the examples below are from my HA implementation, containing my Entity and object names. You may want to adjust these to fit your own implementation. 
- I will just cover the basics, if you need more information on how to do things please visit the [HA Documentation](https://www.home-assistant.io/docs/) or [HA Forum](https://community.home-assistant.io/). 
    
    
## MQTT
Using the `"mqtt.publish"` service, the following commands can be send:
    
### Motor Actions
Below is a list of MQTT *commands* that control the blinds:    
    
Topic: `livingroom/blinds/action`
    
Payload | Description
-- | --
open | Open blinds fully
open:50 | Open the blinds to the indicated percentage. (e.g. 50%)
close | Close the blinds if not already closed.
stop | Stop the blinds if the motor is currently running.

### App Commands
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
`MinLuxReportDelta:<lux>`         | Set the minimum difference in Lux level before publishing MQTT (0 = no threshold, interval only)
`MaxCurrentLimit:<value>`         | Set max load current motor is allowed to draw (raw analog value) (0 = disabled)
`AllowRemoteControl:<true/false>` | Set control Blinds using MQTT (true), else (false)
`AllowRemoteBleep:<true/false>`   | Set if Bleep notifications must be processed (true) or ignored (false)
`WiFiSetup:SSID/password`         | Set the SSID and password to be used ("default" for defaults)
    
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
The active buzzer can be used by HA to send general notifications, in any combination of durations and number of pulses.    
Format of the payload:  `"AxB.B.B..."`
Where
    A:  Number of repititions. Single digit (only), i.e. range 1-9
    B:  Duration of bleep. Value is multiplied with the "BleepTimeOn" constant.
        1 = short, 2 = longer, etc. 9999 max value.

Topic: `all/notify/bleep`
    
Payload | Description
-- | --
`1x1.0.1.2.1`           | one times: beep-silent-beep-beeeep-beep
`2x1.1.1.3.3.3.1.1.1`   | repeat SOS twice

<br>

## 1. Automations Examples

### Open Blinds 
Open the blinds if one of the following happens, and the blinds are still in the "0" position (closed):
1. light level rises above 20
2. it is 8am

```
alias: Livingroom Blinds open in morning
description: ""
trigger:
  - platform: numeric_state
    entity_id: sensor.livingroom_light_level
    above: 20
  - platform: time
    at: "08:00:00"
condition:
  - condition: numeric_state
    entity_id: sensor.livingroom_blinds_position
    below: 1
action:
  - service: mqtt.publish
    data:
      payload: open:40
      topic: livingroom/blinds/action
mode: single
```

### Close Blinds 
Close the blinds if the light level falls below 60, and the blinds are still in the "0" position (closed):

```
alias: Livingroom Blinds close when dark
description: ""
trigger:
  - platform: numeric_state
    entity_id: sensor.livingroom_light_level
    below: 60
condition:
  - condition: numeric_state
    entity_id: sensor.livingroom_blinds_position
    above: 0
action:
  - service: mqtt.publish
    data:
      topic: livingroom/blinds/action
      payload: close
mode: single
```

## 2. Buttons Example
To help you get going, below is a couple of buttons to send various commands to the Blinds Controller, and to show the status of some of the sensors. 

```
type: vertical-stack
cards:
  - type: custom:button-card
    color_type: label-card
    color: var(--primary-color)
    name: BLINDS
    styles:
      card:
        - border-radius: 20%;
        - height: 20px
        - background-image: >-
            linear-gradient(90deg, rgba(2,0,136,1) 0%, rgba(9,9,121,1) 7%,
            rgba(0,212,255,1) 100%)
  - type: horizontal-stack
    cards:
      - type: button
        name: Open
        icon: mdi:blinds
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/action
            payload: open
      - type: button
        name: Open 40
        icon: mdi:blinds
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/action
            payload: open:40
      - type: button
        name: Open 100
        icon: mdi:blinds
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/action
            payload: open:100
      - type: button
        name: Close
        icon: mdi:blinds
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/action
            payload: close
      - type: button
        name: Stop
        icon: mdi:blinds
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/action
            payload: stop
      - type: button
        name: Get Config
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: getconfig
  - type: horizontal-stack
    cards:
      - type: button
        name: Run Dur  0
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: OpenDuration:0
      - type: button
        name: Run Dur 15
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: OpenDuration:15
      - type: button
        name: OpenCnt 0
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: MaxOpenRotations:0
      - type: button
        name: OpenCnt 12
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: MaxOpenRotations:12
      - type: button
        name: MaxLoad 0
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: MaxCurrentLimit:0
      - type: button
        name: MaxLoad 1500
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: MaxCurrentLimit:1500
          target: {}
  - type: horizontal-stack
    cards:
      - type: custom:button-card
        color_type: blank-card
      - type: button
        name: Lux m=0
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: LuxInterval:0
      - type: button
        name: Lux m=1
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: LuxInterval:1
      - type: button
        name: Temp m=0
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: TempInterval:0
      - type: button
        name: Temp m=1
        icon: mdi:cog-outline
        tap_action:
          action: call-service
          service: mqtt.publish
          service_data:
            topic: livingroom/blinds/appcmd
            payload: TempInterval:1
      - type: custom:button-card
        color_type: blank-card
  - type: entities
    entities:
      - entity: binary_sensor.livingroom_blinds
        icon: hass:blinds
      - entity: sensor.livingroom_blinds_position
        icon: hass:blinds-open
      - entity: sensor.livingroom_temperature
      - entity: sensor.livingroom_humidity
      - entity: sensor.livingroom_light_level
```
