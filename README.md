# Bear Detector – IoT Wildlife Early Warning System

## Overview

Bear Detector is an IoT-based wildlife monitoring system designed to help prevent dangerous encounters between humans and bears while respecting wildlife and avoiding harm to animals.

The system uses a motion sensor and an ESP32-CAM to monitor an area for activity. When movement is detected, the camera captures an image and sends it for analysis. Artificial Intelligence can then be used to identify whether the detected object is a bear, a human, or another animal. If a bear is detected, the system can immediately notify nearby users through a mobile application or other alert mechanisms.

The goal of this project is to demonstrate how low-cost IoT technologies and AI can be combined to improve safety in areas where human-wildlife interactions are becoming more frequent.

## Features

* Motion detection using a PIR sensor
* Image capture using ESP32-CAM
* Low-power operation suitable for remote locations
* AI-based animal classification
* Bear detection and alert generation
* Android application integration
* Cloud connectivity through Wi-Fi
* Expandable architecture for detecting other wildlife species

## Hardware Used

* ESP32-CAM
* PIR Motion Sensor (AM312 or similar)
* LDR (Ambient Light Sensor)
* LM393 Comparator Module
* LEDs for local indication
* Power Supply / Battery
* Optional solar charging system

## System Workflow

1. The PIR sensor continuously monitors the area.
2. When motion is detected, the ESP32-CAM wakes up and captures an image.
3. The image is transmitted to a server or AI service.
4. The AI model analyzes the image.
5. If a bear is identified, an alert is sent to the user.
6. Images and detection events can be stored for future analysis.

## Project Objectives

* Reduce the risk of human-bear encounters.
* Provide an affordable wildlife monitoring solution.
* Demonstrate practical IoT and AI integration.
* Promote coexistence between humans and wildlife through technology.
* Create an educational open-source platform for makers and students.

## Disclaimer

This project is intended for educational, research, and prototype purposes. It should not be considered a replacement for professional wildlife management systems or official safety procedures.

## License

This project is released as open-source software. Feel free to use, modify, and improve it according to the terms of the selected license.
