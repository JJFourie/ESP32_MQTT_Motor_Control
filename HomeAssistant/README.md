# Home Assistant

This document contains notes on the related components and functionality I added to [Home Assistant](https://www.home-assistant.io/) (HA).    

Note that    
- in HA there are multiple ways to do things, and what is described below may not be the best way, or may be outdated by the time I finish writing as HA is evolving all the time.
- the examples below are from my HA implementation, containing my Entity and object names. You may want to adjust these to fit your own implementation. 
- I will just cover the basics, if you need more information on how to do things please visit the [HA Documentation](https://www.home-assistant.io/docs/) or [HA Forum](https://community.home-assistant.io/). 

## 1. "Open" Button
This is a button in the HA UI that can be used to send a MQTT message to the Blinds Controller to open the blinds. 

