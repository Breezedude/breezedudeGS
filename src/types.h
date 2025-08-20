 #pragma once
 #include <Arduino.h>
 #include "config.h"

struct Settings {
  char wifi_ssid[64];
  char wifi_password[64];
  char ap_ssid[64];
  char ap_password[64];
  int aprsPort;
  char aprsServer[64];
  int elevation;
  float longitude;
  float latitude;
  char deviceName[12];
  bool keepAP;
  bool sendBreezedude;

  Settings() {
   strncpy(wifi_ssid, DEFAULT_STA_SSID, sizeof(wifi_ssid));
   strncpy(wifi_password, DEFAULT_STA_PASSWD, sizeof(wifi_password));
   strncpy(ap_ssid, "BD-Groundstation", sizeof(ap_ssid));
   strncpy(ap_password, "configureme", sizeof(ap_password));
   aprsPort = 14580;
   strncpy(aprsServer, DEFAULT_APRS_SERVER, sizeof(aprsServer));
   elevation = 400;
   longitude = 12.0f;
   latitude = 47.0f;
   strncpy(deviceName, "MyGS", sizeof(deviceName));
   keepAP = true;
   sendBreezedude = true;
 }
};


template <typename Derived>
struct FanetBase {
    String name;
    String devId;
    uint8_t vid;
    uint16_t fanet_id;
    int rssi;
    float snr;
    time_t timestamp;
    uint32_t last_send;
    float lat;
    float lon;

    bool matches(const Derived& other) const {
        return vid == other.vid && fanet_id == other.fanet_id;
    }

    void assignBase(const FanetBase& other) {
        devId = other.devId;
        vid = other.vid;
        fanet_id = other.fanet_id;
        rssi = other.rssi;
        snr = other.snr;
        timestamp = other.timestamp;
        // last_send = other.last_send; // do not update here
        lat = other.lat;
        lon = other.lon;
    }
};

struct weatherData : public FanetBase<weatherData> {
    bool bTemp;
    float temp;
    float wHeading;
    bool bWind;
    float wSpeed;
    float wGust;
    bool bHumidity;
    float Humidity;
    bool bBaro;
    float Baro;
    bool bStateOfCharge;
    float Charge;
    bool bRain;
    float rain1h;
    float rain24h;

    void assign(const weatherData& other) {
        assignBase(other);
        bTemp = other.bTemp;
        temp = other.temp;
        wHeading = other.wHeading;
        bWind = other.bWind;
        wSpeed = other.wSpeed;
        wGust = other.wGust;
        bHumidity = other.bHumidity;
        Humidity = other.Humidity;
        bBaro = other.bBaro;
        Baro = other.Baro;
        bStateOfCharge = other.bStateOfCharge;
        Charge = other.Charge;
        bRain = other.bRain;
        rain1h = other.rain1h;
        rain24h = other.rain24h;
    }
};

struct trackingData : public FanetBase<trackingData> {
    float alt;
    uint16_t hdop;
    int aircraftType;
    String adressType;
    float speed;
    float climb;
    float heading;
    bool onlineTracking;

    void assign(const trackingData& other) {
        assignBase(other);
        alt = other.alt;
        hdop = other.hdop;
        aircraftType = other.aircraftType;
        adressType = other.adressType;
        speed = other.speed;
        climb = other.climb;
        heading = other.heading;
        onlineTracking = other.onlineTracking;
    }
};

 enum {
    FANET_PCK_TYPE_TRACKING = 0x01,
    FANET_PCK_TYPE_NAME = 0x02,
    FANET_PCK_TYPE_WEATHER = 0x04
  };

typedef struct {
    unsigned int type           :6;
    unsigned int forward        :1;
    unsigned int ext_header     :1;
    unsigned int vendor         :8;
    unsigned int address        :16;
    }  __attribute__((packed)) fanet_header;

typedef struct {
    fanet_header header;
    unsigned int latitude       :24;
    unsigned int longitude      :24;
    /* units are degrees, seconds, and meter */
    unsigned int altitude_lsb   :8; /* FANET+ reported alt. comes from ext. source */
    unsigned int altitude_msb   :3; /* I assume that it is geo (GNSS) altitude */
    unsigned int altitude_scale :1;
    unsigned int aircraft_type  :3;
    unsigned int track_online   :1;
    unsigned int speed          :7;
    unsigned int speed_scale    :1;
    unsigned int climb          :7;
    unsigned int climb_scale    :1;
    unsigned int heading        :8;
    unsigned int turn_rate      :7;
    unsigned int turn_scale     :1;
  } __attribute__((packed)) fanet_packet_t1;


typedef struct {
    fanet_header header;
    unsigned int bExt_header2     :1;
    unsigned int bStateOfCharge   :1;
    unsigned int bRemoteConfig    :1;
    unsigned int bBaro            :1;
    unsigned int bHumidity        :1;
    unsigned int bWind            :1;
    unsigned int bTemp            :1;
    unsigned int bInternetGateway :1;
    unsigned int latitude       :24;
    unsigned int longitude      :24;
    int8_t temp                 :8;
    unsigned int heading        :8;
    unsigned int speed          :7;
    unsigned int speed_scale    :1;
    unsigned int gust          :7;
    unsigned int gust_scale    :1;
    unsigned int humidity      :8;
    int baro          :16;
    unsigned int charge        :8;
  } __attribute__((packed)) fanet_packet_t4;

String FANET2String(uint8_t vid, uint16_t fid);

#define MAX_DEVICES 20
extern weatherData weatherStore[MAX_DEVICES];
extern trackingData trackingStore[MAX_DEVICES];

void pack_weatherdata(weatherData *wData, uint8_t * buffer);
void unpack_weatherdata(uint8_t *buffer, weatherData *wData, float snr, float rssi);
void unpack_trackingdata(uint8_t *buffer, trackingData *data, int rssi, int snr);
void print_fanet_packet_t4(fanet_packet_t4 *pkt);
void print_weatherData(weatherData *wData);
void fill_weatherData_dummy(weatherData *wData);
template <typename T> int storeFanetData(T* store, int maxSize, const T& newData);
int storeWeatherData(const weatherData& newData);
int storeTrackingData(const trackingData& newData);