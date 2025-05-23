/* 
Basic CAN-BUS converter to Digital Output.  Used for MK2/MK3 'analog' clusters in ME7.x and aftermarket conversions and will provide an EML/EPC light.
All outputs are configurable 12v Square Wave with definable max limits based on x RPM
V1.00 - Optional 'traditional' coil output
V1.01 - Optional EML/EPC output.  EPC can be used as 'shift light', RPM configarble
V1.02 - Original RPM input is ~500Hz, speed is ~300Hz for VW Clusters.  Adjustable in code
V1.03 - Optional GPS module for calculating speed if ECU is blind.  Not as accurate but a valid solution...
V1.03 - Built-in LED used for error displaying.  For example - no satellites will illuminate LED
V1.04 - Added DSG support - gets current gear & rpm and calculates theory speed.  Ratios in '_dsg.ino'
V1.05 - Check for hanging
V1.06 - Slowed dowm RPM to minimise speed change during shift
V1.07 - calibrated PWM motor 

Forbes-Automotive, 2025
*/

// for CAN
#include "canbus2cluster_defs.h"
#include <ESP32_CAN.h>
ESP32_CAN<RX_SIZE_256, TX_SIZE_16> chassisCAN;

// for GPS
#include <TinyGPSPlus.h>
#include <SoftwareSerial.h>
SoftwareSerial ss(pinRX_GPS, pinTX_GPS);
TinyGPSPlus gps;

// for inputs / paddles
#include <ButtonLib.h>  //include the declaration for this class
buttonClass btnPadUp(pinPaddleUp, 0, false);
buttonClass btnPadDown(pinPaddleDown, 0, false);
buttonClass btnSpare1(pinSpare1, 0, false);
buttonClass btnSpare2(pinSpare2, 0, false);

// define two hardware timers for RPM & Speed outputs
hw_timer_t* timer0 = NULL;
hw_timer_t* timer1 = NULL;

bool rpmTrigger = true;
bool speedTrigger = true;
long frequencyRPM = 20;    // 20 to 20000
long frequencySpeed = 20;  // 20 to 20000

//if (1) {  // This contains all the timers/Hz/Freq. stuff.  Literally in a //(1) to let Arduino IDE code-wrap all this...
// timer for RPM
void IRAM_ATTR onTimer0() {
  rpmTrigger = !rpmTrigger;
  if (hasCoilOutput) {
    digitalWrite(pinCoil, rpmTrigger);
  } else {
    digitalWrite(pinRPM, rpmTrigger);
  }
}

// timer for Speed
void IRAM_ATTR onTimer1() {
  speedTrigger = !speedTrigger;
  digitalWrite(pinSpeed, speedTrigger);
}

// setup timers
void setupTimer() {
  timer0 = timerBegin(0, 40, true);  //div 80
  timerAttachInterrupt(timer0, &onTimer0, true);

  timer1 = timerBegin(1, 80, true);  //div 80
  timerAttachInterrupt(timer1, &onTimer1, true);
}

// adjust output frequency
void setFrequencyRPM(long frequencyHz) {
  if (frequencyHz != 0) {
    timerAlarmDisable(timer0);
    timerAlarmWrite(timer0, 1000000l / frequencyHz, true);
    timerAlarmEnable(timer0);
  } else {
    timerAlarmDisable(timer0);
  }
}

// adjust output frequency
void setFrequencySpeed(long frequencyHz) {
  if (frequencyHz != 0) {
    timerAlarmDisable(timer1);
    timerAlarmWrite(timer1, 1000000l / frequencyHz, true);
    timerAlarmEnable(timer1);
  } else {
    timerAlarmDisable(timer1);
  }
}
//}

void setup() {
  basicInit();   // basic init for setting up IO / CAN / GPS
  setupTimer();  // setup the timers (with a base frequency)

  if (hasNeedleSweep) {
    needleSweep();  // carry out needle sweep if defined
  }
}

void loop() {
  // get the easy stuff out the way first
  // has error - todo: set to flash, etc...
  if (selfTest) {
    //needleSweep();
    diagTest();
  }

  if (hasError) {
    digitalWrite(onboardLED, HIGH);  // light internal LED
  } else {
    digitalWrite(onboardLED, LOW);
  }

  // set EML & EPC
  digitalWrite(pinEML, vehicleEML);  // Check for EML/EPC light and trigger.  Will be caught by CAN messages
  digitalWrite(pinEPC, vehicleEPC);  // Check for EML/EPC light and trigger.  Will be caught by CAN messages

  btnPadUp.tick();    // paddle up
  btnPadDown.tick();  // paddle down
  btnSpare1.tick();   // input 'spare'
  btnSpare2.tick();   // input 2 'spare'

  // send CAN data for paddle up/down etc
  if (boolPadUp) {
    Serial.println(F("Paddle up"));
    sendPaddleUpFrame();
    boolPadUp = false;
  }
  if (boolPadDown) {
    Serial.println(F("Paddle down"));
    sendPaddleDownFrame();
    boolPadDown = false;
  }
  if (boolSpare1) {
    Serial.println(F("boolSpare1"));
    boolSpare1 = false;
  }
  if (boolSpare2) {
    Serial.println(F("boolSpare2"));
    boolSpare2 = false;
  }

  // get speed type (ECU, DSG or GPS)
  switch (speedType) {
    case 0:  // get speed from ecu
      if (calcSpeed > 0) {
        vehicleSpeed = (byte)(calcSpeed >= 255 ? 0 : calcSpeed);
      }
      break;

    case 1:                                       // get speed from dsg
      if ((millis() - lastMillis) > gearPause) {  // check to see if x ms (linPause) has elapsed - slow down the frames!
        lastMillis = millis();
        parseDSG();
      }
      vehicleSpeed = int(dsgSpeed);
      break;

    case 2:  // get speed from gps
      parseGPS();
      vehicleSpeed = int(gpsSpeed);
      break;

    case 3:
      vehicleSpeed = int(absSpeed);
      break;
  }

  // check to see what the current RPM is, if it's over the limit, trigger the EPC or EML light as a warning!
  if (useEPCShiftLight || useEMLShiftLight) {
    if (vehicleRPM > shiftLimit) {
      blinkLED(shiftLightRate, 3, useEPCShiftLight, useEMLShiftLight, 0, 0);  // args: flash rate, number of flashes, use EPC or use EML as light, RPM/Speed are set to 0, don't use them (kept in for self-test)
    }
  }

  // calculate final frequency:
  frequencySpeed = map(vehicleSpeed, 0, clusterSpeedLimit, 0, maxSpeed);
  frequencyRPM = map(vehicleRPM, 0, clusterRPMLimit, 0, maxRPM);

  // change the frequency of both RPM & Speed as per CAN information
  if ((millis() - lastMillis2) > rpmPause) {  // check to see if x ms (linPause) has elapsed - slow down the frames!
    lastMillis2 = millis();
#if stateDebug
    Serial.println(frequencyRPM);
    Serial.println(frequencySpeed);
#endif
    setFrequencyRPM(frequencyRPM);      // minimum speed may command 0 and setFreq. will cause crash, so +1 to error 'catch'
    setFrequencySpeed(frequencySpeed);  // minimum speed may command 0 and setFreq. will cause crash, so +1 to error 'catch'  }
  }
}