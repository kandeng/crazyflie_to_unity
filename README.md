# Integrate Crazyflie Drone with Unity Game Engine

## 1. Objective

This project integrates [Crazyflie 2.1_drone](https://www.bitcraze.io/products/crazyflie-2-1-brushless/) 
with [Unity6 game engine](https://unity.com/releases/unity-6). 
We use Unity6 as the controller of the Crazyflie2.1, to receive the drone's telemetry and its video stream, 
and to send commands to control the movement of the drone. 

## 2. Hardware

We bought a Crazyflie 2.1 hardware kit from 
[a Taobao store named DinosaurTech](https://item.taobao.com/item.htm?id=985921221709). 
The kit includes four components, with a total cost of US$230.

It is quite straightforward to [assemble the Crazyflie 2.1](https://www.bitcraze.io/documentation/tutorials/getting-started-with-crazyflie-brushless/), 
with well-written documentation. 

It is also very easy to use Python to use radio to 
[communicate with the Crazyflie 2.1](https://www.bitcraze.io/documentation/repository/crazyflie-lib-python/master/user-guides/sbs_connect_log_param/), 
and control its movement.

~~~
1 * crazyflie2.1
1 * flow deck for navigation
1 * crazyradio PA
1 * esp32s3_AI_deck
~~~

   <p align="center" vertical-align="top">
     <img alt="crazyflie2.1" src="./asset/crazyflie_webrtc_dataflow.png" width="48%">
     &nbsp;
     <img alt="flow deck for navigation" src="./asset/crazyflie_webrtc_dataflow.png" width="48%">
   </p>  
   <p align="center" vertical-align="top">
     <img alt="crazyradio PA" src="./asset/crazyflie_webrtc_dataflow.png" width="48%">
     &nbsp;
     <img alt="esp32s3_AI_deck" src="./asset/crazyflie_webrtc_dataflow.png" width="48%">
   </p>  

