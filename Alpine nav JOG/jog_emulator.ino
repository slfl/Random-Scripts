/*
  Полный эмулятор управления JOG (Honda/Alpine nav)
  Протокол: UART 4800 бод, 8N1, LSB-first, ИНВЕРТИРОВАННЫЙ (покой = LOW, 0..5В).
  Кадр: 4 байта  [тип][млад][старш][cks],  сумма всех 4 байт = 0xFF.
    тип 0x80 = кнопки (повтор ~90мс пока зажата; отпускание 80 00 00 7F)
    тип 0x81 = энкодер по часовой (1 кадр на щелчок, без отпускания)
    тип 0xFF = энкодер против часовой

  Подключение:
    D8  -> линия JOG головного устройства
    GND -> GND головного устройства (общая земля обязательна!)
    Плата 5В (Uno/Nano/Mega): уровень 0..5В.

  Монитор порта (9600). Команды:
    имя кнопки: map menu back audio info climate brightness enter up down left right
    cw [N]     - энкодер по часовой N щелчков (по умолч. 1)
    ccw [N]    - энкодер против часовой
    m LO HI    - произвольная маска кнопок (hex), напр. m 40 00
    h N LO HI  - держать кнопку N кадров (длинное нажатие)
    beat       - heartbeat: слать отпускание ~каждые 90мс (для старта блока)
*/

#include <SoftwareSerial.h>
const uint8_t JOG_TX = 8;                 // <- провод JOG
SoftwareSerial jog(7, JOG_TX, true);      // inverse_logic: покой LOW

struct Btn { uint8_t lo, hi; const char* name; };
Btn BTN[] = {
  {0x01,0x00,"map"},   {0x02,0x00,"brightness"}, {0x04,0x00,"climate"},
  {0x10,0x00,"info"},  {0x20,0x00,"menu"},       {0x40,0x00,"enter"},
  {0x00,0x01,"left"},  {0x00,0x02,"down"},       {0x00,0x04,"right"},
  {0x00,0x08,"up"},    {0x00,0x20,"back"},       {0x00,0x40,"audio"},
};
const int NBTN = sizeof(BTN)/sizeof(BTN[0]);

void sendFrame(uint8_t type, uint8_t lo, uint8_t hi){
  uint8_t cks = (uint8_t)(0xFF - type - lo - hi);   // сумма 4 байт = 0xFF
  jog.write(type); jog.write(lo); jog.write(hi); jog.write(cks);
}
void press(uint8_t lo, uint8_t hi, int hold){
  for(int i=0;i<hold;i++){ sendFrame(0x80, lo, hi); delay(90); }
  sendFrame(0x80,0x00,0x00); delay(90);             // отпускание
}
void encoder(uint8_t type, int n){                  // 0x81=CW, 0xFF=CCW
  for(int i=0;i<n;i++){ sendFrame(type,0x00,0x00); delay(60); }
}

bool beatMode=false; unsigned long lastBeat=0;

void setup(){
  Serial.begin(9600); jog.begin(4800);
  Serial.println(F("JOG pult ready. Кнопки + cw/ccw + m/h/beat"));
}
void loop(){
  if(beatMode && millis()-lastBeat>=90){ sendFrame(0x80,0,0); lastBeat=millis(); }
  if(!Serial.available()) return;
  String s=Serial.readStringUntil('\n'); s.trim(); if(!s.length()) return;
  String ls=s; ls.toLowerCase();

  if(ls=="beat"){ beatMode=!beatMode; Serial.println(beatMode?F("beat ON"):F("beat OFF")); return; }
  if(ls.startsWith("cw")){  int n=s.substring(2).toInt(); encoder(0x81, n>0?n:1); Serial.println(F("cw"));  return; }
  if(ls.startsWith("ccw")){ int n=s.substring(3).toInt(); encoder(0xFF, n>0?n:1); Serial.println(F("ccw")); return; }
  if(ls[0]=='m'){ int a=s.indexOf(' '),b=s.indexOf(' ',a+1);
    uint8_t lo=strtol(s.substring(a+1,b).c_str(),0,16), hi=strtol(s.substring(b+1).c_str(),0,16);
    press(lo,hi,3); Serial.println(F("ok")); return; }
  if(ls[0]=='h'){ int a=s.indexOf(' '),b=s.indexOf(' ',a+1),c=s.indexOf(' ',b+1);
    int n=s.substring(a+1,b).toInt();
    uint8_t lo=strtol(s.substring(b+1,c).c_str(),0,16), hi=strtol(s.substring(c+1).c_str(),0,16);
    press(lo,hi,n); Serial.println(F("ok")); return; }

  for(int i=0;i<NBTN;i++) if(ls==BTN[i].name){ press(BTN[i].lo,BTN[i].hi,3); Serial.print(F("press ")); Serial.println(BTN[i].name); return; }
  Serial.println(F("?"));
}
