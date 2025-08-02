#ifndef __OGN_H__
#define __OGN_H__

#include <Arduino.h>
#include <string.h>
#include <WiFi.h>
#include "time.h"
#include "types.h"
#include <TimeLib.h>

#define OGNSTATUSINTERVALL 300000ul

class Ogn {
public:
  enum aircraft_t : uint8_t
  {
    UNKNOWN = 0,
    GLIDER_MOTOR_GLIDER = 1,
    TOW_PLANE = 2,
    HELICOPTER_ROTORCRAFT = 3,
    SKYDIVER = 4,
    DROP_PLANE_SKYDIVER = 5,
    HANG_GLIDER = 6,
    PARA_GLIDER = 7,
    AIRCRAFT_RECIPROCATING_ENGINE = 8,
    AIRCRAFT_JET_TURBO_ENGINE = 9,
    UFO = 10,
    BALLOON = 11,
    AIRSHIP = 12,
    UAV = 13,
    GROUND_SUPPORT = 14,
    STATIC_OBJECT = 15
  };

  Ogn(); //constructor
  bool begin(String user,String version);
  void end(void);
  void run(bool bNetworkOk); //has to be called cyclic
  void setAirMode(bool _AirMode); //sets the mode (for sending heading and speed only if Air-Module)
  void setGPS(float lat,float lon,float alt,float speed,float heading);
  void sendTrackingData(trackingData *td);
  void sendGroundTrackingData(time_t timestamp,float lat,float lon,float alt,String devId,uint8_t state,uint8_t adressType,float snr);
  void sendNameData(String devId,String name,float snr);
  void sendWeatherData(weatherData *wData);
  void setClient(Client *_client);
  void setBattVoltage(float battVoltage);
  void setStatusData(float pressure, float temp,float hum, float battVoltage,uint8_t battPercent);
  bool connected(){return aprs_connected;};
  void setAprsServer(String server, uint32_t port);

private:
    void checkClientConnected(uint32_t tAct);
    void connect2Server(uint32_t tAct);
    void sendLoginMsg(void);
    String calcPass(String user);
    void readClient();
    void checkLine(String line);
    void sendStatus(uint32_t tAct);
    void sendReceiverStatus(String sTime);    
    void sendReceiverBeacon(String sTime);
    String getActTimeString();
    String getActTimeString(time_t timestamp);
    String getTimeStringFromTimestamp(time_t timestamp);
    //uint8_t getSenderDetails(bool onlinetracking,aircraft_t aircraftType,uint8_t addressType);
    uint8_t getSenderDetails(bool OnlineTracking, aircraft_t aircraftType);
    String getOrigin(uint8_t addressType);
    uint8_t getFANETAircraftType(aircraft_t aircraftType);

    char aprs_server[90] = "";
    uint32_t aprs_port = 0; // default is 14580
    bool aprs_connected = false;
    Client *client;
    String _user;
    String _version;
    String _servername;
    float _lat = NAN;
    float _lon = NAN;
    float _alt = NAN;
    float _speed = NAN;
    float _heading = NAN;
    float _BattVoltage = NAN;
    uint8_t _BattPercent = 0;
    float _Pressure = NAN;
    float _Temp = NAN;
    float _Hum = NAN;
    uint32_t tStatus;
    uint32_t tRecBaecon;
    uint8_t initOk;
    uint8_t GPSOK;
    bool AirMode = false;
    const char *AprsIcon[16] = // Icons for various FLARM acftType's
    { "/z",  //  0 = ?
      "/'",  //  1 = (moto-)glider    (most frequent)
      "/'",  //  2 = tow plane        (often)
      "/X",  //  3 = helicopter       (often)
      "/g" , //  4 = parachute        (rare but seen - often mixed with drop plane)
      "\\^", //  5 = drop plane       (seen)
      "/g" , //  6 = hang-glider      (rare but seen)
      "/g" , //  7 = para-glider      (rare but seen)
      "\\^", //  8 = powered aircraft (often)
      "/^",  //  9 = jet aircraft     (rare but seen)
      "/z",  //  A = UFO              (people set for fun)
      "/O",  //  B = balloon          (seen once)
      "/O",  //  C = airship          (seen once)
      "/'",  //  D = UAV              (drones, can become very common)
      "/z",  //  E = ground support   (ground vehicles at airfields)
      "\\n"  //  F = static object    (ground relay ?)
    };

    enum {
        INIT_NONE,
        INIT_CONNECTED,
        INIT_REGISTERED,
        INIT_FULL,
    };
};

#endif