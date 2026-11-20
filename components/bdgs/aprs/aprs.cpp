#include "aprs.h"
#include "bd_radio.h"

extern void aprsconsole_print(String in);

Aprs::Aprs(){
    _user = "";
    client = NULL;
}

void Aprs::setClient(Client *_client){
    client = _client;
}

bool Aprs::begin(String user,String version){
    _version = version;
    _user = user;
    aprs_connected = false;
    GPSOK = 0;
    tStatus = 0;
    if (client == NULL){ //if now client is set, we use the wifi-client
      client = new WiFiClient();
    }
    return true;
}

void Aprs::end(void){
    if(client){
        client->stop();
    }
}

int32_t getStringValue(String s,String begin,String end,int32_t fromIndex,String *sRet){
    int pos = s.indexOf(begin,fromIndex);
    if (pos < 0) return -1;
    pos += begin.length();
    int pos2 = s.indexOf(end,pos);
    if (pos2 < 0) return -1;
    *sRet = s.substring(pos,pos2);
    return pos2;
}

float kmh2mph(float f){
  return f / 1.609;
}

float deg2f(float f){
  return (f * 9/5) + 32;
}

void Aprs::setAirMode(bool _AirMode){
    AirMode = _AirMode;
}

void Aprs::sendLoginMsg(void){
    String login ="user " + _user + " pass " + calcPass(_user) + " vers " + _version + "\r\n";
    //log_i("%s",login.c_str());
    client->print(login);
    client->flush();
}

String Aprs::calcPass(String user){
    const int length = user.length();
    uint8_t buffer[length];
    String myString = user.substring(0,10);
    myString.toUpperCase();
    memcpy(buffer, myString.c_str(), length);
    uint16_t hash = 0x73e2;
    int i = 0;
    while (i < length){
        hash ^= buffer[i] << 8;
        if ((i+1) < length){
            hash ^= buffer[i+1];
        }
        i += 2;
    }
    hash &= 0x7fff;
    return String(hash);
}

void Aprs::setAprsServer(String server, uint32_t port){
    strcpy(aprs_server, server.c_str());
    aprs_port = port;
}


void Aprs::connect2Server(uint32_t tAct){
    static uint32_t tConnRetry = tAct;
    initOk = INIT_NONE;
    aprs_connected = false;
    if (aprs_port && ((tAct - tConnRetry) >= 5000) && (aprs_server[0] != '\0')){
        tConnRetry = tAct;
        client->stop();
        client->flush();
        int ret = client->connect(aprs_server, aprs_port);
        if (ret) {
            sendLoginMsg();
            aprs_connected = true;
            Serial.println(F("Connected to aprs server"));
        } else {
            Serial.println(F("aprs connect failed"));
        }
        
    }
    
}

void Aprs::checkLine(String line){
    String s = "";
    int32_t pos = 0;
    //log_i("%s",line.c_str());
    if (!line.startsWith("# aprsc")){
      //log_i("%s:%s",getActTimeString().c_str(),line.c_str());
    }
    if (initOk == INIT_NONE){
        aprsconsole_print(line);
        pos = getStringValue(line,"server ","\r\n",0,&s);
        if (pos > 0){
            tStatus = 0;
            tRecBaecon = millis() - APRSSTATUSINTERVALL;
            _servername = s;
            initOk = INIT_CONNECTED;
            log_i("logged in successfully");
        }
    }
}

void Aprs::setGPS(float lat,float lon,float alt,float speed,float heading){
    _lat = lat;
    _lon = lon;
    _alt = alt;
    _speed = speed;
    _heading = heading;
    GPSOK = 1;
}

// OGN id field: "id" + detail byte + 24 bit address. The detail byte is
// STttttaa - S stealth, T no-tracking, tttt aircraft type, aa address type
// (0 random, 1 ICAO, 2 FLARM, 3 OGN). Checked against the OGN parser
// (ogn-parser-rs position_comment.rs), the example beacons and
// Publishing_to_OGN.md in glidernet/ogn-aprs-protocol, OGN's FANET and ADS-L
// decoders (pjalocha esp32-ogn-tracker main/fanet.h, adsl.h) and live traffic.
//
// `hidden`: the sender does not want to be shown or stored. Its position is
// still relayed - search and rescue depend on it. OGN's FANET decoder marks
// this with the stealth bit, the parser and the publishing guide call the
// no-tracking bit the one that keeps a position off maps; both are set.
uint8_t Aprs::ognIdByte(uint8_t ognAircraftType, uint8_t ognAddrType, bool hidden) {
    return (hidden ? 0xC0 : 0x00) | ((ognAircraftType & 0x0F) << 2) | (ognAddrType & 0x03);
}

// OGN aircraft types are FLARM's: 0 unknown, 1 glider, 2 tow plane,
// 3 helicopter, 4 parachute, 5 drop plane, 6 hang glider, 7 paraglider,
// 8 powered, 9 jet, A UFO, B balloon, C airship, D UAV, E ground support,
// F static object.
uint8_t Aprs::ognAircraftType(const trackingData *td) {
    if (td->srcProto == TRACK_SRC_FLARM) {
        return td->nativeAircraftType & 0x0F;  // already FLARM numbering
    }
    if (td->srcProto == TRACK_SRC_ADSL) {
        // ADS-L aircraft category, as OGN's ADS-L decoder maps it (getAcftTypeOGN).
        static const uint8_t kAdsl[14] = {0, 8, 9, 3, 1, 0xC, 2, 7, 4, 0xD, 3, 0xD, 0xD, 0xD};
        return td->nativeAircraftType < sizeof(kAdsl) ? kAdsl[td->nativeAircraftType] : 0;
    }
    // FANET aircraft type 0-7, as OGN's FANET decoder maps it.
    static const uint8_t kFanet[8] = {0, 7, 6, 0xB, 1, 8, 3, 0xD};
    return kFanet[td->aircraftType & 0x07];
}

// From the call sign prefix. FANET addresses go out as FLARM type, as OGN's own
// FANET and ADS-L decoders do and as the Skytraxx example beacons show.
uint8_t Aprs::ognAddrType(const String& prefix) {
    if (prefix == "ICA") return 1;
    if (prefix == "FLR" || prefix == "FNT") return 2;
    if (prefix == "OGN") return 3;
    return 0;
}

void Aprs::sendNameData(String devId,String name,float snr){
    if (initOk < INIT_FULL) return; //nothing todo
    String sTime = getActTimeString();
    if (sTime.length() <= 0) return;
    char buff[200];
    sprintf (buff,"%s%s>%s,qAS,%s:>%sh Name=\"%s\" %0.1fdB\r\n","FNT",devId.c_str(),aprsTag,_user.c_str(),sTime.c_str(),name.c_str(),snr);
    
    client->print(buff); 
    aprsconsole_print(buff);
    client->flush();   
    //log_i("%s",buff);

}

bool Aprs::sendWeatherData(weatherData *wData){

    if (initOk < INIT_FULL) return false; //nothing todo
    float lLat = abs(wData->lat);
    float lLon = abs(wData->lon);
    int latDeg = int(lLat);
    int latMin = (roundf((lLat - int(lLat)) * 60 * 1000));
    int lonDeg = int(lLon);
    int lonMin = (roundf((lLon - int(lLon)) * 60 * 1000));
    int mHum = (int)wData->Humidity;
    if (mHum >= 100){ //humidity 100% --> 00
        mHum = 0;
    }else if (mHum == 0){
        mHum = 1;
    }
    String sTime = getTimeStringFromTimestamp(wData->timestamp);
    if (sTime.length() <= 0) return false;
    char buff[200];
    String send = "";
    sprintf (buff,"FNT%s>%s,qAS,%s:/%sh%02d%02d.%02d%c/%03d%02d.%02d%c_%03d/%03dg%03d"
    ,wData->devId.c_str(),aprsTag,_user.c_str(),sTime.c_str(),latDeg,latMin/1000,latMin/10 %100,(wData->lat < 0)?'S':'N',lonDeg,lonMin/1000,lonMin/10 %100,(wData->lon < 0)?'W':'E',
    int(wData->wHeading),int(kmh2mph(wData->wSpeed)),int(kmh2mph(wData->wGust)));
    send += buff;
    if (wData->bTemp){
        sprintf (buff,"t%03d",int(deg2f(wData->temp)));
        send += buff;
    }
    if (wData->bRain){
        if (wData->rain1h >= 100){
            send += "r999";
        }else{
            sprintf (buff,"r%03d",int(wData->rain1h * 10));
            send += buff;
        }
        if (wData->rain24h >= 100){
            send += "p999";
        }else{
            sprintf (buff,"p%03d",int(wData->rain24h * 10));
            send += buff;
        }
    }
    if (wData->bHumidity){
        sprintf (buff,"h%02d",mHum);
        send += buff;
    }
    if (wData->bBaro){
        sprintf (buff,"b%05d",int(wData->Baro * 10));
        send += buff;
    }
    sprintf (buff," %0.1fdB\r\n",wData->snr);
    send += buff;

    size_t res = client->print(send.c_str()); 
    aprsconsole_print(send);
    client->flush();
               
    
    log_i("%s",send.c_str());
    return res > 0;
}

// As OGN's FANET decoder writes a ground position (pjalocha main/fanet.h):
// static-object symbol, "!" for an emergency, aircraft type F, no altitude, and
// "FNT7<status>" with the FANET ground status in hex. The packet carries no time
// of its own, so it is the time of reception.
void Aprs::sendGroundTrackingData(const trackingData *td){
  if (initOk < INIT_FULL) return;
  String sTime = getActTimeString(td->timestamp);
  if (sTime.length() <= 0) return;
  const float lLat = fabsf(td->lat);
  const float lLon = fabsf(td->lon);
  const int latDeg = int(lLat);
  const int latMin = roundf((lLat - int(lLat)) * 60 * 1000);
  const int lonDeg = int(lLon);
  const int lonMin = roundf((lLon - int(lLon)) * 60 * 1000);
  const uint8_t status = (uint8_t)td->state & 0x0F;
  const char symbol = status >= 13 ? '!' : 'n';
  const uint8_t idByte = ognIdByte(0x0F, ognAddrType(td->adressType), !td->onlineTracking);
  char buff[200];
  snprintf(buff, sizeof(buff), "%s%s>%s,qAS,%s:/%sh%02d%02d.%02d%c\\%03d%02d.%02d%c%c !W%01d%01d! id%02X%s FNT7%X %0.1fdB\r\n",
           td->adressType.c_str(), td->devId.c_str(), aprsTag, _user.c_str(), sTime.c_str(),
           latDeg, latMin / 1000, latMin / 10 % 100, (td->lat < 0) ? 'S' : 'N',
           lonDeg, lonMin / 1000, lonMin / 10 % 100, (td->lon < 0) ? 'W' : 'E', symbol,
           latMin % 10, lonMin % 10, idByte, td->devId.c_str(), status, td->snr);
  client->print(buff);
  aprsconsole_print(buff);
  client->flush();
}

void Aprs::sendTrackingData(trackingData *td){
    if (initOk < INIT_FULL) return;
    // Senders that asked not to be tracked are kept locally but never relayed.
    if (td->noTrack) return;
    if ((td->aircraftType < 0) || (td->aircraftType > 15)){
        Serial.printf("wrong aircraftType %d --> set to unknown\n",(int)(td->aircraftType));
        td->aircraftType = acft_Other;
    }
    char buff[255];
    float lLat = abs(td->lat);
    float lLon = abs(td->lon);
    int latDeg = int(lLat);
    int latMin = (roundf((lLat - int(lLat)) * 60 * 1000));
    int lonDeg = int(lLon);
    int lonMin = (roundf((lLon - int(lLon)) * 60 * 1000));
    String sTime = getActTimeString(td->timestamp);
    if (sTime.length() <= 0) return;
    int pos = 0;
    pos += snprintf(&buff[pos],255-pos,"%s",td->adressType.c_str());
    // OGN tells the receiving technology by the destination call, as its own
    // receivers do: OGFLR for FLARM, OGADSL for ADS-L, OGNFNT for FANET.
    const char* dest = aprsTag;
    if (td->srcProto == TRACK_SRC_FLARM) dest = "OGFLR";
    else if (td->srcProto == TRACK_SRC_ADSL) dest = "OGADSL";
    pos += snprintf(&buff[pos],255-pos,"%s>%s,qAS,",td->devId.c_str(), dest);
    pos += snprintf(&buff[pos],255-pos,"%s:/",_user.c_str());
    pos += snprintf(&buff[pos],255-pos,"%sh",sTime.c_str());
    pos += snprintf(&buff[pos],255-pos,"%02d%02d.%02d",latDeg,latMin/1000,latMin/10 %100);
    pos += snprintf(&buff[pos],255-pos,"%c",(td->lat < 0)?'S':'N');
    // The symbol table is indexed by OGN (= FLARM) aircraft type, like the id byte.
    const uint8_t ognType = ognAircraftType(td);
    pos += snprintf(&buff[pos],255-pos,"%c",AprsIcon[ognType][0]);
    pos += snprintf(&buff[pos],255-pos,"%03d%02d.%02d",lonDeg,lonMin/1000,lonMin/10 %100);
    pos += snprintf(&buff[pos],255-pos,"%c",(td->lon < 0)?'W':'E');
    pos += snprintf(&buff[pos],255-pos,"%c",AprsIcon[ognType][1]);
    pos += snprintf(&buff[pos],255-pos,"%03d/%03d",int(td->heading),int(td->speed * 0.53996));
    pos += snprintf(&buff[pos],255-pos,"/A=%06d",int(td->alt * 3.28084));
    pos += snprintf(&buff[pos],255-pos," !W%01d%01d! ",int(latMin %10),int(lonMin %10));
    // FLARM no-track and stealth senders never get here (bd_radio.cpp); ADS-L
    // has no such flag. A FANET sender without online tracking is relayed but
    // marked hidden.
    const bool hidden = td->srcProto == TRACK_SRC_FANET && !td->onlineTracking;
    uint8_t idByte = ognIdByte(ognType, ognAddrType(td->adressType), hidden);
    pos += snprintf(&buff[pos],255-pos,"id%02X%s ",idByte,td->devId.c_str());
    pos += snprintf(&buff[pos],255-pos,"%+04.ffpm ",td->climb*196.85f);
    if (td->srcProto == TRACK_SRC_FANET){
        // FANET packet type 1 and the FANET aircraft type, which aircraftType
        // already holds (as OGN's FANET decoder writes it: "FNT11" = paraglider).
        pos += snprintf(&buff[pos],255-pos,"FNT1%d ",td->aircraftType & 0x07);
        pos += snprintf(&buff[pos],255-pos,"%0.1fdB\r\n",td->snr);
    }else{
        // GFSK gives no SNR; OGN receivers report the signal above the noise
        // floor instead. Left out until a noise floor has been measured.
        float noise = bd_radio_noise_floor();
        if (!isnan(noise)){
            pos += snprintf(&buff[pos],255-pos,"%0.1fdB\r\n",td->rssi - noise);
        }else{
            pos += snprintf(&buff[pos],255-pos,"\r\n");
        }
    }
    client->print(buff);
    aprsconsole_print(buff);
    client->flush();
}

void Aprs::setStatusData(float pressure, float temp,float hum, float battVoltage,uint8_t battPercent){
    _Pressure = pressure;
    _Temp = temp;
    _Hum = hum;
    _BattVoltage = battVoltage;
    _BattPercent = battPercent;
}

void Aprs::setBattVoltage(float battVoltage) {
    _BattVoltage = battVoltage;
}

String Aprs::buildReceiverStatusBody(const String& note) {
    String body = "BDGS:" + _version + " UP:" + String(millis() / 60000) + "min";
    if (!isnan(_BattVoltage)) {
        char battBuf[16];
        snprintf(battBuf, sizeof(battBuf), " B:%.2fV", _BattVoltage);
        body += battBuf;
    }
    if (note.length() > 0) {
        body += " ";
        body += note;
    }
    return body;
}

void Aprs::sendReceiverStatus(String sTime, const String& note){
    String sStatus = _user + ">" + aprsTag + ",TCPIP*,qAC," + _servername + ":>" + sTime + "h " + buildReceiverStatusBody(note) + "\r\n";

    client->print(sStatus);
    aprsconsole_print(sStatus);
    client->flush();

    
    if (initOk < INIT_FULL) initOk = INIT_FULL; //now we can send, because we have sent GPS-Position
    log_i("%s",sStatus.c_str());
}

bool Aprs::sendSystemStatus(const String& note) {
    if (!aprs_connected || !GPSOK) {
        return false;
    }

    String sTime = getActTimeString();
    if (sTime.length() == 0) {
        return false;
    }

    if (initOk < INIT_REGISTERED) {
        sendReceiverBeacon(sTime);
    }
    sendReceiverStatus(sTime, note);
    return true;
}

void Aprs::sendReceiverBeacon(String sTime){
    char buff[200];
    float lLat = abs(_lat);
    float lLon = abs(_lon);
    int latDeg = int(lLat);
    int latMin = (roundf((lLat - int(lLat)) * 60 * 1000));
    int lonDeg = int(lLon);
    int lonMin = (roundf((lLon - int(lLon)) * 60 * 1000));

    if (AirMode){
        sprintf (buff,"%s>%s,TCPIP*,qAC,%s:/%sh%02d%02d.%02d%cI%03d%02d.%02d%c&%03d/%03d/A=%06d !W%01d%01d!\r\n"
                    ,_user.c_str(),aprsTag,_servername.c_str(),sTime.c_str(),latDeg,latMin/1000,latMin/10 %100,(_lat < 0)?'S':'N',lonDeg,lonMin/1000,lonMin/10 %100,(_lon < 0)?'W':'E',int(_heading),int(_speed * 0.53996),int(_alt * 3.28084),int(latMin %10),int(lonMin %10));
    }else{
        sprintf (buff,"%s>%s,TCPIP*,qAC,%s:/%sh%02d%02d.%02d%cI%03d%02d.%02d%c&/A=%06d\r\n"
                    ,_user.c_str(),aprsTag,_servername.c_str(),sTime.c_str(),latDeg,latMin/1000,latMin/10 %100,(_lat < 0)?'S':'N',lonDeg,lonMin/1000,lonMin/10 %100,(_lon < 0)?'W':'E',int(_alt * 3.28084));
    }

    client->print(buff);
    aprsconsole_print(buff);
    client->flush();
 
    
    if (initOk < INIT_REGISTERED) initOk = INIT_REGISTERED; //now we can send, because we have sent GPS-Position
    //log_i("%s",buff);
}

String Aprs::getActTimeString(time_t timestamp){
  struct tm timeinfo;
  char buff[20];
  gmtime_r(&timestamp, &timeinfo);
  sprintf (buff,"%02d%02d%02d",timeinfo.tm_hour,timeinfo.tm_min,timeinfo.tm_sec);
  //log_i("%s",buff);
  return String(buff);
}

String Aprs::getActTimeString(){
  char buff[20];
  if (timeStatus() == timeSet){
      //log_i("%d %d %d %d:%d:%d",year(),month(),day(),hour(),minute(),second());
      sprintf (buff,"%02d%02d%02d",hour(),minute(),second());
      //log_i("%s",buff);
      return String(buff);
  }else{
      return "";
  }
}

// APRS time stamps are UTC. localtime() follows the station's time zone, which
// is set from its position: weather went out an hour or two in the future.
String Aprs::getTimeStringFromTimestamp(time_t timestamp) {
    struct tm tm;
    gmtime_r(&timestamp, &tm);
    char buff[8];
    snprintf(buff, sizeof(buff), "%02d%02d%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return String(buff);
}

void Aprs::sendStatus(uint32_t tAct){
    if (initOk > INIT_NONE){        
        if (GPSOK){
            if (((tAct - tRecBaecon) >= APRSSTATUSINTERVALL) || tRecBaecon == 0){
                tRecBaecon = tAct;
                tStatus = tAct;
                String sTime = getActTimeString();
                Serial.println(sTime);
                if (sTime.length() > 0){
                    sendReceiverBeacon(sTime);
                    //sendReceiverStatus(sTime);
                }
            }
        }
        //we have to send ReceiverBeacon and afterwards the status (Receiverbaecon always first)
        if (((tAct - tStatus) >= 5000) && (tStatus != 0)){
            String sTime = getActTimeString();
            if (sTime.length() > 0){
                sendReceiverStatus(sTime, "");
            }
            tStatus = 0;
        }
    }
}

void Aprs::readClient(){
  String line = "";
  if (client->available()){
    while (client->available()){
        char c = client->read(); //read 1 character 
        
        line += c; //read 1 character
        if (c == '\n'){
            checkLine(line);
            line = "";
        }
    }
  }
}

void Aprs::checkClientConnected(uint32_t tAct){
    static uint32_t tCheck = tAct;
    if ((tAct - tCheck) >= 5000){
        tCheck = tAct;
        if (!client->connected()){
            if(aprs_connected){
                Serial.println("aprs disonnected");
            }
            aprs_connected = false;
        }
    }
}

void Aprs::run(bool bNetworkOk){    
    uint32_t tAct = millis();    
    //if (WiFi.status() == WL_CONNECTED){
    if (bNetworkOk){
        checkClientConnected(tAct);
        if (aprs_connected){
            //Serial.print("aprs_connected: "); Serial.println(aprs_connected);
            readClient();
            sendStatus(tAct);
        }else{
            //not connected --> try to connect
            connect2Server(tAct);
        }
    }else{
        if(aprs_connected){
            Serial.println("WiFi disonnected");
        }
        aprs_connected = false;
    }
}