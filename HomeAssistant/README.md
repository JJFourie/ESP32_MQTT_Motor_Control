# Home Assistant

Below are some notes on the related components and functionality I added to [Home Assistant](https://www.home-assistant.io/) (HA).    

Note that    
- in HA there are multiple ways to do things, and what is described here may not be the best way, or may be outdated by the time I finished writing as HA is evolving all the time.
- the examples below are from my HA implementation, containing my Entity and object names. You may want to adjust these to fit your own implementation. 
- I will just cover the basics, if you need more information on how to do things please visit the [HA Documentation](https://www.home-assistant.io/docs/) or [HA Forum](https://community.home-assistant.io/). 
    
<br>
    
## 1. "Configuration Attributes" Sensor
Define the sensor that will extract the attributes from the JSON message received from the Blinds controller.
Put the following in the HA `configuration.yaml`

```
mqtt:
  sensor:
    - name: 'BlindsController Config'
      unique_id: blindscontrollerconfig01
      state_topic: 'livingroom/blinds/config'
      json_attributes_topic: 'livingroom/blinds/config'
      value_template: '{{ value_json.MaxOpenRotations }}'
      device: 
        identifiers:
          - BLINDSCONTROLLER

```

## 2. Automation Scripts Examples

### Open Blinds 
Open the blinds to 50% if whichever one of the following happens first:
1. light level rises above 20
2. it is 8am
The blinds will only be opened if it is still in the "0" position (closed), and it is after 7am and before 12pm.
The automation will then publish a message to my phone (`mobile_app_<myphone>`) 30 seconds later to confirm the new blinds position.

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
  - condition: time
    after: "07:00:00"
    before: "12:00:00"
action:
  - service: mqtt.publish
    data:
      payload: open:50
      topic: livingroom/blinds/action
  - delay: 30
  - service: notify.mobile_app_<myphone>
    data:
      title: Home Blinds Change
      message: >-
        "The blinds are OPENING.. Current: {% if
        is_state('sensor.livingroom_blinds_position', '0') %}closed{% else
        %}open{% endif %}! ( {{states.sensor.livingroom_blinds_position.state }}
        )"
mode: single
```

### Close Blinds 
Close the blinds if the light level falls below a certain level. This will only be done if the blinds are not already in the "0" position (closed), and if it is after 4pm and before midnight. This is to prevent that in certain conditions the blinds are closed during wrong times of the day when e.g. a cloud blocks the sunlight.
I make use of a helper entity (`input_number.threshold_light_inside`) to make the threshold level when the blinds will be closed, configurable from the UI.
The automation will then publish a message to my phone (`mobile_app_<myphone>`) 30 seconds later to confirm the new blinds position.

```
alias: Livingroom Blinds close when dark
description: ""
trigger:
  - platform: numeric_state
    entity_id: sensor.livingroom_light_level
    below: input_number.threshold_light_inside
    enabled: true
  - platform: template
    value_template: >-
      value_template: "{{ states('sensor.livingroom_light_level')|int <
      states('input_number.threshold_blinds')|int }}"
    alias: When light level falls below Threshold_Blinds
condition:
  - condition: numeric_state
    entity_id: sensor.livingroom_blinds_position
    above: 0
  - condition: time
    after: "16:00:00"
    before: "23:59:59"
    alias: After 4pm and before midnight
action:
  - service: mqtt.publish
    data:
      topic: livingroom/blinds/action
      payload: close
    alias: Publish MQTT to close Blinds
  - delay: 30
  - service: notify.mobile_app_<myphone>
    data:
      title: Home Blinds Change
      message: >-
        "The blinds are CLOSING.. Current: {% if
        is_state('sensor.livingroom_blinds_position', '0') %}closed{% else
        %}open{% endif %}! ( {{states.sensor.livingroom_blinds_position.state }}
        )"
mode: single
```


## 3. Display States, and Configuration Attributes
Below is the JSON to set up 
1. an entity card to display the current status of the sensors.
2. an entity card that will display the configuration attributes that were last reported. 

```
type: vertical-stack
cards:
  - type: entities
    title: Controller States
    entities:
      - entity: binary_sensor.livingroom_blinds
        icon: hass:blinds
      - entity: sensor.livingroom_blinds_position
        icon: hass:blinds-open
      - entity: sensor.livingroom_temperature
      - entity: sensor.livingroom_humidity
      - entity: sensor.livingroom_light_level
  - type: entities
    title: Controller Configuration
    show_header_toggle: false
    entities:
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: AllowRemoteControl
        name: Allow Remote Control
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: AllowRemoteBleep
        name: Allow Remote Bleep
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: MinLuxReportDelta
        name: Minimum Lux Delta
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: LuxInterval
        name: Lux Interval
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: TempInterval
        name: Temperature Interval
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: StateInterval
        name: State Interval
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: DebounceDurSwitches
        name: Debounce Duration - Switches
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: DebounceDurMotor
        name: Debounce Duration - Motor
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: RotationLimits
        name: Rotation Limits
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: OpenDuration
        name: Open Duration
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: MaxOpenRotations
        name: Max Open Rotations
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: MaxCurrentLimit
        name: Max Current Limit
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: MaxRunDuration
        name: Max Run Duration
        icon: mdi:clock-time-ten-outline
      - type: attribute
        entity: sensor.livingroom_blinds_config
        attribute: SSID
        name: SSID
        icon: mdi:clock-time-ten-outline

```

## 4. Buttons Example
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
```
