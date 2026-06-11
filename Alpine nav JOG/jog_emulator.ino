/*
  Полный эмулятор управления JOG (Honda/Alpine nav)
  UART 4800 бод, 8N1, LSB-first, ИНВЕРТИРОВАННЫЙ (покой LOW, 0..5В).
  Кадр: [тип][млад][старш][cks], сумма 4 байт = 0xFF.
    тип 0x80 = кнопки, 0x81 = энкодер CW, 0xFF = энкодер CCW.

  Подключение: D8 -> JOG, GND -> GND блока (общая земля!). Плата 5В.

  Команды (9600), удобно слать из HTML-пульта:
    hold LO HI  - НАЖАТЬ и держать маску (повтор кадра ~90мс), напр. hold 21 00
    rel         - отпустить (кадр 80 00 00)
    cw [N]      - энкодер по часовой N щелчков
    ccw [N]     - энкодер против часовой
    beat        - heartbeat (повтор отпускания ~90мс для старта блока)
    <имя>       - одиночное нажатие из консоли: map menu back audio info
                  climate brightness enter up down left right
  Удержание задаётся длительностью реального нажатия в HTML (hold..rel),
  никаких фиксированных таймингов.
*/
#include <SoftwareSerial.h>
const uint8_t JOG_TX = 8;
SoftwareSerial jog(7, JOG_TX, true);   // inverse_logic: покой LOW

uint8_t heldLo=0, heldHi=0;
bool holding=false, beatMode=false;
unsigned long lastTx=0;
const unsigned long PERIOD=90;         // период повтора кадра, мс

struct Btn{ uint8_t lo,hi; const char* n; };
Btn BTN[]={
  {0x01,0x00,"map"},  {0x02,0x00,"brightness"},{0x04,0x00,"climate"},
  {0x10,0x00,"info"}, {0x20,0x00,"menu"},      {0x40,0x00,"enter"},
  {0x00,0x01,"left"}, {0x00,0x02,"down"},      {0x00,0x04,"right"},
  {0x00,0x08,"up"},   {0x00,0x20,"back"},      {0x00,0x40,"audio"},
};
const int NB=sizeof(BTN)/sizeof(BTN[0]);

void sendFrame(uint8_t t,uint8_t lo,uint8_t hi){
  uint8_t c=(uint8_t)(0xFF-t-lo-hi);
  jog.write(t); jog.write(lo); jog.write(hi); jog.write(c);
}
void encoder(uint8_t t,int n){ for(int i=0;i<n;i++){ sendFrame(t,0,0); delay(60);} }

uint8_t hx(const String& s){ return (uint8_t)strtol(s.c_str(),0,16); }

void setup(){
  Serial.begin(9600); jog.begin(4800);
  Serial.println(F("JOG ready: hold LO HI / rel / cw / ccw / beat / <name>"));
}

void loop(){
  unsigned long now=millis();
  if(now-lastTx>=PERIOD){                 // фоновый повтор
    if(holding)        sendFrame(0x80,heldLo,heldHi);
    else if(beatMode)  sendFrame(0x80,0,0);
    lastTx=now;
  }
  if(!Serial.available()) return;
  String s=Serial.readStringUntil('\n'); s.trim(); if(!s.length()) return;
  String ls=s; ls.toLowerCase();

  if(ls=="rel"){ holding=false; sendFrame(0x80,0,0); return; }
  if(ls.startsWith("hold")){
    int a=s.indexOf(' '), b=s.indexOf(' ',a+1);
    heldLo=hx(s.substring(a+1,b)); heldHi=hx(s.substring(b+1));
    holding=true; sendFrame(0x80,heldLo,heldHi); lastTx=millis(); return;
  }
  if(ls=="beat"){ beatMode=!beatMode; Serial.println(beatMode?F("beat ON"):F("beat OFF")); return; }
  if(ls.startsWith("ccw")){ int n=s.substring(3).toInt(); encoder(0xFF, n>0?n:1); return; }
  if(ls.startsWith("cw")){  int n=s.substring(2).toInt(); encoder(0x81, n>0?n:1); return; }

  for(int i=0;i<NB;i++) if(ls==BTN[i].n){      // одиночное нажатие из консоли
    for(int k=0;k<3;k++){ sendFrame(0x80,BTN[i].lo,BTN[i].hi); delay(90);} 
    sendFrame(0x80,0,0); return;
  }
  Serial.println(F("?"));
}
