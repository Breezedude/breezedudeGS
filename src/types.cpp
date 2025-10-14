#include "types.h"

weatherData weatherStore[MAX_DEVICES];
trackingData trackingStore[MAX_DEVICES];

int storeWeatherData(const weatherData& newData) {
    return storeFanetData(weatherStore, MAX_DEVICES, newData);
}

int storeTrackingData(const trackingData& newData) {
    return storeFanetData(trackingStore, MAX_DEVICES, newData);
}


String FANET2String(uint8_t vid, uint16_t fid){
    char str [10];
    sprintf(str,"%02X%04X", vid, fid);
    return str;
}


void pack_weatherdata(weatherData *wData, uint8_t * buffer){
  fanet_packet_t4 *pkt = (fanet_packet_t4 *)buffer;
  pkt->header.type = 4;
  pkt->header.vendor = wData->vid;
  pkt->header.forward = false;
  pkt->header.ext_header = false;
  pkt->header.address = wData->fanet_id;
  pkt->bExt_header2 = false;
  pkt->bStateOfCharge = wData->bStateOfCharge;
  pkt->bRemoteConfig = false;
  pkt->bBaro = wData->bBaro;
  pkt->bHumidity = wData->bHumidity;
  pkt->bWind = wData->bWind;
  pkt->bTemp = wData->bTemp;
  pkt->bInternetGateway = false;

  int32_t lat_i = roundf(wData->lat * 93206.0f);
	int32_t lon_i = roundf(wData->lon * 46603.0f);

  pkt->latitude = lat_i;
  pkt->longitude = lon_i;

  if (wData->bTemp){
    int iTemp = (int)(round(wData->temp * 2)); //Temperature (+1byte in 0.5 degree, 2-Complement)
    pkt->temp = iTemp & 0xFF;
  }
  if (wData->bWind){
    pkt->heading = uint8_t(round(wData->wHeading * 256.0 / 360.0)); //Wind (+3byte: 1byte Heading in 360/256 degree, 1byte speed and 1byte gusts in 0.2km/h (each: bit 7 scale 5x or 1x, bit 0-6))
    int speed = (int)roundf(wData->wSpeed * 5.0f);
    if(speed > 127) {
        pkt->speed_scale  = 1;
        pkt->speed        = (speed / 5);
    } else {
        pkt->speed_scale  = 0;
        pkt->speed        = speed & 0x7F;
    }
    speed = (int)roundf(wData->wGust * 5.0f);
    if(speed > 127) {
        pkt->gust_scale  = 1;
        pkt->gust        = (speed / 5);
    } else {
        pkt->gust_scale  = 0;
        pkt->gust        = speed & 0x7F;
    }
  }
  if (wData->bHumidity){
      pkt->humidity = uint8_t(round(wData->Humidity * 10 / 4)); //Humidity (+1byte: in 0.4% (%rh*10/4))
  }
  if (wData->bHumidity){
    pkt->baro = int16_t(round((wData->Baro - 430.0) * 10));  //Barometric pressure normailized (+2byte: in 10Pa, offset by 430hPa, unsigned little endian (hPa-430)*10)
  }
  pkt->charge = constrain(roundf(float(wData->Charge) / 100.0 * 15.0),0,15); //State of Charge  (+1byte lower 4 bits: 0x00 = 0%, 0x01 = 6.666%, .. 0x0F = 100%)
}

bool unpack_weatherdata(uint8_t *buffer, weatherData *wData, float snr, float rssi){
    fanet_packet_t4 *pkt = (fanet_packet_t4 *)buffer;
    wData->snr = snr;
    wData->rssi = rssi;
    // Basic header
    wData->vid = pkt->header.vendor;
    wData->fanet_id = pkt->header.address;
    wData->bStateOfCharge = pkt->bStateOfCharge;
    wData->bBaro = pkt->bBaro;
    wData->bHumidity = pkt->bHumidity;
    wData->bWind = pkt->bWind;
    wData->bTemp = pkt->bTemp;
    wData->devId = FANET2String(wData->vid, wData->fanet_id);
    wData->timestamp = time(nullptr);
    // Latitude
    int32_t lat_raw = pkt->latitude & 0xFFFFFF;  // keep only 24 bits
    if (lat_raw & 0x800000)                            // if sign bit (bit 23) is set
        lat_raw |= 0xFF000000;                         // sign extend to 32 bits
    wData->lat = (float)lat_raw / 93206.0f;
    // Longitude
    int32_t lon_raw = pkt->longitude & 0xFFFFFF; // keep only 24 bits
    if (lon_raw & 0x800000)
        lon_raw |= 0xFF000000;
    wData->lon = (float)lon_raw / 46603.0f;
    // Temperature
    if(wData->bTemp){
        int8_t iTemp = pkt->temp; // signed, as it uses 2's complement
        wData->temp = iTemp / 2.0f; // recover 0.5 deg resolution
    }
    // Wind
    if(wData->bWind){
        wData->wHeading = (pkt->heading * 360.0f) / 256.0f; // recover heading
        int speed;
        if(pkt->speed_scale){
            speed = pkt->speed * 5; // scale 5x
        } else {
            speed = pkt->speed;
        }
        wData->wSpeed = speed / 5.0f; // recover to km/h

        if(pkt->gust_scale){
            speed = pkt->gust * 5;
        } else {
            speed = pkt->gust;
        }
        wData->wGust = speed / 5.0f;
    }

    // Humidity
    if(wData->bHumidity){
        wData->Humidity = pkt->humidity * 4.0f / 10.0f; // recover to %rh
    }
    // Baro
    if(wData->bBaro){
        wData->Baro = (pkt->baro / 10.0f) + 430.0f; // recover to hPa
    }
    // State of charge
    if(wData->bStateOfCharge){
        wData->Charge = (pkt->charge & 0x0F) * 100.0f / 15.0f; // recover 0-100%
    }
    return true;
}

float fns_buf2coord_compressed(uint16_t *buf, float mycoord)
{
	/* decode buffer */
	bool odd = !!((1<<15) & *buf);
	int16_t sub_deg_int = (*buf&0x7FFF) | (1<<14&*buf)<<1;
	const float sub_deg = sub_deg_int / 32767.0f;


	/* retrieve coordinate */
	float mycood_rounded = roundf(mycoord);
	bool mycoord_isodd = ((int)mycood_rounded) & 1;

	/* target outside our segment. estimate where it is in */
	if(mycoord_isodd != odd)
	{
		/* adjust deg segment */
		const float mysub_deg = mycoord - mycood_rounded;
		if(sub_deg > mysub_deg)
			mycood_rounded--;
		else
			mycood_rounded++;
	}
	return mycood_rounded + sub_deg;
}

bool unpack_trackingdata(uint8_t *buffer, trackingData *data, int rssi, int snr) {
    fanet_packet_t1 *packet = (fanet_packet_t1 *)buffer;

    data->vid = packet->header.vendor;
    data->fanet_id = packet->header.address;
    data->devId = FANET2String(data->vid, data->fanet_id);
    data->rssi = rssi;
    data->snr = snr;
    data->adressType = "FNT";
    data->timestamp = time(nullptr);
    // Latitude
    int32_t lat_raw = packet->latitude_raw & 0xFFFFFF;  // keep only 24 bits
    if (lat_raw & 0x800000)                            // if sign bit (bit 23) is set
        lat_raw |= 0xFF000000;                         // sign extend to 32 bits
    data->lat = (float)lat_raw / 93206.0f;

    // Longitude
    int32_t lon_raw = packet->longitude_raw & 0xFFFFFF; // keep only 24 bits
    if (lon_raw & 0x800000)
        lon_raw |= 0xFF000000;
    data->lon = (float)lon_raw / 46603.0f;
   // Altitude
float altitude = (float)packet->altitude * (packet->altitude_scale ? 4.0f : 1.0f);
data->alt = altitude;

// HDOP (not transmitted in FANET)
data->hdop = 0.0f;

// Aircraft Type
data->aircraftType = (trck_acft_type) (packet->aircraft_type); 

// Speed
float speed = (float)packet->speed_value * 0.5f * (packet->speed_scale ? 5.0f : 1.0f);
data->speed = speed;
if(data->speed > 315){return false;}// (max 317.5km/h)

// Climb
int8_t climb_raw = packet->climb_value;
if (climb_raw & 0x40) climb_raw |= 0x80;  // sign extend 7-bit 2’s complement
float climb = (float)climb_raw * 0.1f * (packet->climb_scale ? 5.0f : 1.0f);
data->climb = climb;

// Heading
data->heading = (float)packet->heading * (360.0f / 256.0f);

// Online Tracking flag
data->onlineTracking = packet->track_online;
data->state = state_Flying;

// Optional fields
// Turn rate (if present)
#ifdef FANET_HAS_TURN_RATE
int8_t turn_raw = packet->turn_value;
if (turn_raw & 0x40) turn_raw |= 0x80;
data->turnRate = (float)turn_raw * 0.25f * (packet->turn_scale ? 4.0f : 1.0f);
#endif

// QNE offset (if present)
#ifdef FANET_HAS_QNE
int8_t qne_raw = packet->qne_value;
if (qne_raw & 0x40) qne_raw |= 0x80;
data->qneOffset = (float)qne_raw * (packet->qne_scale ? 4.0f : 1.0f);
#endif
return true;
}


bool unpack_ground_trackingdata(uint8_t *buffer, trackingData *data, int rssi, int snr) {
    fanet_packet_t7 *packet = (fanet_packet_t7 *)buffer;

    data->vid = packet->header.vendor;
    data->fanet_id = packet->header.address;
    data->devId = FANET2String(data->vid, data->fanet_id);
    data->rssi = rssi;
    data->snr = snr;
    data->adressType = "FNT";
    data->timestamp = time(nullptr);
    // Latitude
    int32_t lat_raw = packet->latitude_raw & 0xFFFFFF;  // keep only 24 bits
    if (lat_raw & 0x800000)                            // if sign bit (bit 23) is set
        lat_raw |= 0xFF000000;                         // sign extend to 32 bits
    data->lat = (float)lat_raw / 93206.0f;

    // Longitude
    int32_t lon_raw = packet->longitude_raw & 0xFFFFFF; // keep only 24 bits
    if (lon_raw & 0x800000)
        lon_raw |= 0xFF000000;
    data->lon = (float)lon_raw / 46603.0f;
    data->state = packet->type;
    data->climb = 0;
    data->heading = 0;
    data->aircraftType = acft_Other;
    data->speed =0;
    data->alt =0;
    return true;
}



template <typename T>
int storeFanetData(T* store, int maxSize, const T& newData) {
    // Check for existing match
    for (int i = 0; i < maxSize; ++i) {
        if (store[i].timestamp != 0 && store[i].matches(newData)) {
            String nameTemp = store[i].name;
            store[i].assign(newData);
            store[i].name = nameTemp;
            return i;
        }
    }

    // Find free slot
    for (int i = 0; i < maxSize; ++i) {
        if (store[i].timestamp == 0) {
            store[i] = newData;
            return i;
        }
    }

    // Overwrite oldest
    int oldestIndex = 0;
    time_t oldest = store[0].timestamp;
    for (int i = 1; i < maxSize; ++i) {
        if (store[i].timestamp < oldest) {
            oldest = store[i].timestamp;
            oldestIndex = i;
        }
    }

    store[oldestIndex] = newData;
    return oldestIndex;
}



void print_fanet_packet_t4(fanet_packet_t4 *pkt) {
    Serial.println(F("=== FANET Packet T4 ==="));
    Serial.printf("Header:\n");
    Serial.printf("  Type: %u\n", pkt->header.type);
    Serial.printf("  Forward: %u\n", pkt->header.forward);
    Serial.printf("  Ext Header: %u\n", pkt->header.ext_header);
    Serial.printf("  Vendor: %u\n", pkt->header.vendor);
    Serial.printf("  Address: %u\n", pkt->header.address);

    Serial.println(F("Flags:"));
    Serial.printf("  bExt_header2: %u\n", pkt->bExt_header2);
    Serial.printf("  bStateOfCharge: %u\n", pkt->bStateOfCharge);
    Serial.printf("  bRemoteConfig: %u\n", pkt->bRemoteConfig);
    Serial.printf("  bBaro: %u\n", pkt->bBaro);
    Serial.printf("  bHumidity: %u\n", pkt->bHumidity);
    Serial.printf("  bWind: %u\n", pkt->bWind);
    Serial.printf("  bTemp: %u\n", pkt->bTemp);
    Serial.printf("  bInternetGateway: %u\n", pkt->bInternetGateway);

    Serial.println(F("Data:"));
    Serial.printf("  Latitude (raw): %ld\n", pkt->latitude);
    Serial.printf("  Longitude (raw): %ld\n", pkt->longitude);

    Serial.printf("  Temp: %d\n", pkt->temp);
    Serial.printf("  Heading: %u\n", pkt->heading);
    Serial.printf("  Speed: %u\n", pkt->speed);
    Serial.printf("  Speed Scale: %u\n", pkt->speed_scale);
    Serial.printf("  Gust: %u\n", pkt->gust);
    Serial.printf("  Gust Scale: %u\n", pkt->gust_scale);
    Serial.printf("  Humidity: %u\n", pkt->humidity);
    Serial.printf("  Baro: %d\n", pkt->baro);
    Serial.printf("  Charge: %u\n", pkt->charge);
    Serial.println(F("=======================\n"));
}


void print_weatherData(weatherData *wData) {
    Serial.println(F("=== WeatherData ==="));
    Serial.printf("Timestamp (tLastMsg): %lu ms\n", wData->timestamp);
    //Serial.printf("DevID: %lu\n", wData->devId);
    //Serial.printf("Name: %s\n", wData->name.c_str());
    Serial.printf("VID: %02X\n", wData->vid);
    Serial.printf("Fanet ID: %04X\n", wData->fanet_id);
    Serial.printf("RSSI: %d dBm\n", wData->rssi);
    Serial.printf("SNR: %d dB\n", wData->snr);
    Serial.printf("Latitude: %.6f°\n", wData->lat);
    Serial.printf("Longitude: %.6f°\n", wData->lon);

    Serial.println(F("Measurements:"));

    if (wData->bTemp) {
        Serial.printf("  Temperature: %.1f °C\n", wData->temp);
    } else {
        Serial.println(F("  Temperature: N/A"));
    }

    if (wData->bWind) {
        Serial.printf("  Wind Heading: %.1f °\n", wData->wHeading);
        Serial.printf("  Wind Speed: %.1f km/h\n", wData->wSpeed);
        Serial.printf("  Wind Gust: %.1f km/h\n", wData->wGust);
    } else {
        Serial.println(F("  Wind: N/A"));
    }

    if (wData->bHumidity) {
        Serial.printf("  Humidity: %.1f %%RH\n", wData->Humidity);
    } else {
        Serial.println(F("  Humidity: N/A"));
    }

    if (wData->bBaro) {
        Serial.printf("  Barometric Pressure: %.1f hPa\n", wData->Baro);
    } else {
        Serial.println(F("  Barometric Pressure: N/A"));
    }
    if (wData->bStateOfCharge) {
        Serial.printf("  State of Charge: %.1f %%\n", wData->Charge);
    } else {
        Serial.println(F("  State of Charge: N/A"));
    }
    Serial.println(F("=====================\n"));
}


void fill_weatherData_dummy(weatherData *wData) {
    wData->timestamp = time(nullptr);
    wData->vid = 0xBD;
    wData->fanet_id = 0x1234;
    wData->rssi = -55;
    wData->snr = 10;
    wData->lat = 47.123456;
    wData->lon = 11.654321;

    wData->bTemp = true;
    wData->temp = 21.5;

    wData->bWind = true;
    wData->wHeading = millis()%360;
    wData->wSpeed = 12.3;
    wData->wGust = 20.1;

    wData->bHumidity = true;
    wData->Humidity = 55.2;

    wData->bBaro = true;
    wData->Baro = 1012.8;

    wData->bStateOfCharge = true;
    wData->Charge = 85.0;
}
