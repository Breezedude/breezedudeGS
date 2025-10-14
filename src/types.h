 #pragma once
 #include <Arduino.h>
 #include "config_template.h"

struct Settings {
  char wifi_ssid[64];
  char wifi_password[64];
  char ap_ssid[64];
  char ap_password[64];
  bool sendAPRS;
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



typedef enum trck_state_ {
    state_Other = 0,
	state_Walking = 1,
	state_Vehicle = 2,
	state_Bike = 3,
	state_Boot = 4,
	state_Need_ride = 8,
	state_Landed_well = 9,
	state_Need_technical_support = 12,
	state_Need_medical_help = 13,
	state_Distress_call = 14,
	state_Distress_call_automatically = 15,
    state_Flying = 16 // if fanet packet type = 1
  } trck_state;

  

const String trck_state_names [16] = {"Other","Walking","Vehicle","Bike","Boot","Need a ride","Landed well","Need technical support","Need medical help","Distress call","Distress call automatically","Flying"};

typedef enum trck_acft_type_ {
    acft_Other = 0,
	acft_Paraglider= 1,
	acft_Hangglider= 2,
	acft_Balloon= 3,
	acft_Glider= 4,
	acft_Powered_Aircraft= 5,
	acft_Helicopter= 6,
	acft_UAV= 7,
  } trck_acft_type;

const String trck_acft_names[8] = {"Other","Paraglider","Hangglider","Balloon","Glider","Powered Aircraft","Helicopter","UAV"};

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
    trck_acft_type aircraftType;
    String adressType;
    float speed;
    float climb;
    float heading;
    bool onlineTracking;
    trck_state state;

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
        state = other.state; // either groundmode_ or -1 for flying
    }
};

 enum {
    FANET_PCK_TYPE_TRACKING = 0x01,
    FANET_PCK_TYPE_NAME = 0x02,
    FANET_PCK_TYPE_WEATHER = 0x04,
    FANET_PCK_TYPE_GROUND_TRACKING = 0x07
  };

typedef struct {
    unsigned int type           :6;
    unsigned int forward        :1;
    unsigned int ext_header     :1;
    
    unsigned int vendor         :8;
    unsigned int address        :16;
    }  __attribute__((packed)) fanet_header;

typedef struct __attribute__((packed)) {
    fanet_header header;
    // Bytes 0–2 : Latitude (24-bit signed, little endian)
    unsigned int latitude_raw : 24;

    // Bytes 3–5 : Longitude (24-bit signed, little endian)
    unsigned int longitude_raw : 24;

    // Bytes 6–7 : Type + altitude field
    unsigned int altitude       : 11;  // bits 0–10 (meters)
    unsigned int altitude_scale : 1;   // bit 11 (0=×1, 1=×4)
    unsigned int aircraft_type  : 3;   // bits 12–14 (0..7)
    unsigned int track_online   : 1;   // bit 15

    // Byte 8 : Speed
    unsigned int speed_value : 7;  // bits 0–6 (×0.5 km/h)
    unsigned int speed_scale : 1;  // bit 7 (×5)
    
    // Byte 9 : Climb rate
    unsigned int climb_value : 7;  // bits 0–6 (×0.1 m/s, 2’s complement)
    unsigned int climb_scale : 1;  // bit 7 (×5)
    

    // Byte 10 : Heading (0–255 → 0–360°)
    unsigned int heading : 8;

    // Byte 11 : (optional) Turn rate
    unsigned int turn_value : 7;  // bits 0–6 (×0.25°/s, 2’s complement)
    unsigned int turn_scale : 1;  // bit 7 (×4)
    
    // Byte 12 : (optional) QNE offset
    unsigned int qne_value : 7;   // bits 0–6 (meters, 2’s complement)
    unsigned int qne_scale : 1;   // bit 7 (×4)
    
    
} fanet_packet_t1;



typedef struct __attribute__((packed)) {
    fanet_header header;
    // Bytes 0–2 : Latitude (24-bit signed, little endian)
    unsigned int latitude_raw : 24;

    // Bytes 3–5 : Longitude (24-bit signed, little endian)
    unsigned int longitude_raw : 24;

    // Bytes 6 : Type
    unsigned int track_online   : 1;   // bit 0
    unsigned int tbd  : 3;             // bits 3-1
    trck_state type : 4;             // bits 7-4

} fanet_packet_t7;


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
bool unpack_weatherdata(uint8_t *buffer, weatherData *wData, float snr, float rssi);
bool unpack_trackingdata(uint8_t *buffer, trackingData *data, int rssi, int snr);
bool unpack_ground_trackingdata(uint8_t *buffer, trackingData *data, int rssi, int snr);
void print_fanet_packet_t4(fanet_packet_t4 *pkt);
void print_weatherData(weatherData *wData);
void fill_weatherData_dummy(weatherData *wData);
template <typename T> int storeFanetData(T* store, int maxSize, const T& newData);
int storeWeatherData(const weatherData& newData);
int storeTrackingData(const trackingData& newData);