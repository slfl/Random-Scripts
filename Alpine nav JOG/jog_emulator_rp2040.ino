/* ============================================================================
 *  jog_emulator_rp2040.ino — эмулятор пульта JOG на RP2040 через NPN-каскад
 *
 *  ВАЖНО: малинка управляет НЕ линией напрямую, а БАЗОЙ транзистора — точно как
 *  микроконтроллер в оригинальном пульте. Прямой GPIO давал «жёсткий» ноль и
 *  глушил шину; транзистор даёт «мягкий» ноль (Vce_sat), как настоящий JOG,
 *  поэтому эмулятор не глушит линию и сосуществует с оригиналом.
 *
 *  СХЕМА (повторяет выходной каскад оригинального пульта):
 *      GPIO2 ──[10к]── база BC847C        (Rb = 10к, как в оригинале!)
 *      коллектор ───── линия данных (pin5 навигатора)
 *      эмиттер   ───── GND (общий с навигацией)
 *      (подтяжку линии к 3.3В даёт навигатор; транзистор ИНВЕРТИРУЕТ сигнал)
 *
 *  Логика уровней (транзистор инвертирует базу):
 *      GPIO2 = HIGH → база ~0.7В → транзистор ОТКРЫТ → линия LOW (~0.2В)
 *      GPIO2 = LOW  → база 0В     → транзистор ЗАКРЫТ → линия HIGH (3.3В)
 *      покой: GPIO2=HIGH → линия LOW (как держит оригинальный JOG)
 *
 *  ПРОТОКОЛ (подтверждён захватом):
 *      4800 бод (~208us/бит), 8N1, LSB-first
 *      кадр: 4 байта [type][lo][hi][cks], (type+lo+hi+cks)&0xFF == 0xFF
 *      type: 0x80=кнопки (маски lo/hi), 0x81=энкодер CW, 0xFF=энкодер CCW
 *      RELEASE (80 00 00 7F) — ТОЛЬКО после направлений (UP/DOWN/LEFT/RIGHT,
 *      диагонали). Остальные кнопки и энкодер — без отпускания.
 *
 *  Малинка кормит базу ОБЫЧНЫМ (неинвертированным) UART (inv=0, push-pull) —
 *  инверсию делает транзистор, и на линии выходит ровно сигнал JOG.
 *
 *  ПК-интерфейс: USB @115200, команды построчно (CR или LF). См. printHelp().
 * ============================================================================ */

#include <Arduino.h>

#ifndef DATA_PIN
#define DATA_PIN 2          // GPIO2 -> [10к] -> база BC847C
#endif

// ---------------------------- Конфигурация линии ----------------------------
struct LineCfg {
  uint16_t bitUs    = 208;   // ~208us = 4800 бод
  uint16_t gapUs    = 900;   // пауза между байтами кадра
  bool     inverted = false; // НЕ инвертируем на малинке — инверсию делает транзистор
  bool     msbFirst = false; // LSB-first
  uint8_t  driveMode= 0;     // 0=push-pull (кормим базу как МК), 1=open-drain
  uint16_t relMs    = 60;    // задержка перед авто-RELEASE для направлений
} cfg;

static inline uint8_t idleLevel()  { return cfg.inverted ? LOW  : HIGH; } // покой базы=HIGH -> линия LOW
static inline uint8_t startLevel() { return cfg.inverted ? HIGH : LOW;  }
static inline uint8_t bitLevel(uint8_t b) { return b ? idleLevel() : startLevel(); }

// ----------------------------- Драйвер базы --------------------------------
static inline void driveLine(uint8_t level) {
  if (cfg.driveMode == 0) {                 // push-pull (как выход МК на базу)
    pinMode(DATA_PIN, OUTPUT); digitalWrite(DATA_PIN, level);
  } else {                                  // open-drain (на всякий случай)
    if (level == LOW) { pinMode(DATA_PIN, OUTPUT); digitalWrite(DATA_PIN, LOW); }
    else              { pinMode(DATA_PIN, INPUT); }
  }
}
static inline void idleLine() { driveLine(idleLevel()); }  // покой: база HIGH -> линия LOW

// --------------------------- Передача байта/кадра ---------------------------
static void txByte(uint8_t value) {
  driveLine(startLevel());                  // START
  delayMicroseconds(cfg.bitUs);
  for (uint8_t i = 0; i < 8; i++) {         // 8 DATA, LSB-first
    uint8_t b = cfg.msbFirst ? ((value >> (7 - i)) & 1) : ((value >> i) & 1);
    driveLine(bitLevel(b));
    delayMicroseconds(cfg.bitUs);
  }
  driveLine(idleLevel());                   // STOP
  delayMicroseconds(cfg.bitUs);
}
static void sendRaw(const uint8_t* b, uint8_t n) {
  idleLine();
  for (uint8_t i = 0; i < n; i++) { txByte(b[i]); if (i + 1 < n && cfg.gapUs) { idleLine(); delayMicroseconds(cfg.gapUs); } }
  idleLine();
}

// --------------------------- Сборка кадра JOG -------------------------------
static uint8_t cksFF(uint8_t t, uint8_t lo, uint8_t hi) { return (uint8_t)(0xFF - ((t + lo + hi) & 0xFF)); }
static void sendKey(uint8_t t, uint8_t lo, uint8_t hi) {
  uint8_t f[4] = { t, lo, hi, cksFF(t, lo, hi) };
  sendRaw(f, 4);
  Serial.print(F("TX ")); for (uint8_t i=0;i<4;i++){ if(f[i]<16)Serial.print('0'); Serial.print(f[i],HEX); Serial.print(' '); }
  Serial.println();
}
static void sendRelease() { sendKey(0x80, 0x00, 0x00); } // 80 00 00 7F
static void sendCW()      { sendKey(0x81, 0x00, 0x00); }
static void sendCCW()     { sendKey(0xFF, 0x00, 0x00); }

// ------------------------- Кнопки по имени ----------------------------------
struct Btn { const char* name; uint8_t lo, hi; bool isDir; };
static const Btn BTNS[] = {
  {"MAP",0x01,0x00,false},{"BRIGHT",0x02,0x00,false},{"CLIMATE",0x04,0x00,false},
  {"INFO",0x10,0x00,false},{"MENU",0x20,0x00,false},{"ENTER",0x40,0x00,false},
  {"BACK",0x00,0x20,false},{"AUDIO",0x00,0x40,false},{"ENG",0x21,0x20,false},
  {"UP",0x00,0x08,true},{"DOWN",0x00,0x02,true},{"LEFT",0x00,0x01,true},{"RIGHT",0x00,0x04,true},
  {"UL",0x00,0x09,true},{"UR",0x00,0x0C,true},{"DL",0x00,0x03,true},{"DR",0x00,0x06,true},
};
static const uint8_t NBTN = sizeof(BTNS)/sizeof(BTNS[0]);

// нажатие: кадр маски; для направлений — авто-RELEASE (как у настоящего JOG)
static void pressBtn(uint8_t lo, uint8_t hi, bool isDir) {
  sendKey(0x80, lo, hi);
  if (isDir) { delay(cfg.relMs); sendRelease(); }
}

// ------------------------------- ПК-команды ---------------------------------
static int hexNib(char c){ if(c>='0'&&c<='9')return c-'0'; c|=0x20; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static uint8_t parseHex(const char* s, uint8_t* out, uint8_t maxn){
  uint8_t n=0; while(*s&&n<maxn){ while(*s==' ')s++; int h=hexNib(*s); if(h<0)break; s++; int l=hexNib(*s);
    if(l>=0){ out[n++]=(h<<4)|l; s++; } else out[n++]=h; } return n; }

static void printCfg(){
  Serial.print(F("CFG bit=")); Serial.print(cfg.bitUs);
  Serial.print(F(" gap=")); Serial.print(cfg.gapUs);
  Serial.print(F(" inv=")); Serial.print(cfg.inverted);
  Serial.print(F(" msb=")); Serial.print(cfg.msbFirst);
  Serial.print(F(" drive=")); Serial.print(cfg.driveMode==1?F("od"):F("pp"));
  Serial.print(F(" rel=")); Serial.println(cfg.relMs);
}
static void printHelp(){
  Serial.println(F("=== JOG emulator RP2040 (NPN-каскад, Rb=10к, 4800 бод) ==="));
  Serial.println(F("BTN <name>  - кнопка. Направления (UP/DOWN/LEFT/RIGHT/UL/UR/DL/DR) сами шлют RELEASE."));
  Serial.println(F("              остальные: MAP CLIMATE MENU ENTER BACK AUDIO INFO BRIGHT ENG"));
  Serial.println(F("CW / CCW    - энкодер"));
  Serial.println(F("KEY <lo> <hi> - кнопка по маскам (hex). REL - отпускание (80 00 00 7F)"));
  Serial.println(F("RAW <hex>   - сырые байты;  CFG? / CFG inv=0 drive=pp bit=208 gap=900 rel=60;  BTN?"));
}
static void cmdCfg(char* a){
  char* t=strtok(a," ");
  while(t){ char* e=strchr(t,'='); if(e){ *e=0; long v=strtol(e+1,nullptr,0);
    if(!strcmp(t,"bit"))cfg.bitUs=v; else if(!strcmp(t,"gap"))cfg.gapUs=v;
    else if(!strcmp(t,"inv"))cfg.inverted=v; else if(!strcmp(t,"msb"))cfg.msbFirst=v;
    else if(!strcmp(t,"drive"))cfg.driveMode=(e[1]=='o')?1:0; else if(!strcmp(t,"rel"))cfg.relMs=v; }
    t=strtok(nullptr," "); }
  printCfg();
}

static char line[96]; static uint8_t lp=0;
static void handleLine(char* s){
  while(*s==' ')s++;
  if(!strncasecmp(s,"CFG?",4)){ printCfg(); return; }
  if(!strncasecmp(s,"CFG",3)){ cmdCfg(s+3); return; }
  if(!strncasecmp(s,"HELP",4)){ printHelp(); return; }
  if(!strncasecmp(s,"BTN?",4)){ for(uint8_t i=0;i<NBTN;i++){Serial.print(BTNS[i].name);Serial.print(BTNS[i].isDir?F("* "):F(" "));} Serial.println(F("\n(* = направление, шлёт RELEASE)")); return; }
  if(!strncasecmp(s,"REL",3)){ sendRelease(); return; }
  if(!strncasecmp(s,"CCW",3)){ sendCCW(); return; }
  if(!strncasecmp(s,"CW",2) && !isalpha((unsigned char)s[2])){ sendCW(); return; }
  if(!strncasecmp(s,"BTN",3)){ char* n=s+3; while(*n==' ')n++;
    for(uint8_t i=0;i<NBTN;i++) if(!strcasecmp(n,BTNS[i].name)){ pressBtn(BTNS[i].lo,BTNS[i].hi,BTNS[i].isDir); return; }
    Serial.println(F("нет такой кнопки (BTN?)")); return; }
  if(!strncasecmp(s,"KEY",3)){ char* a=s+3; uint8_t lo=(uint8_t)strtol(a,&a,16); uint8_t hi=(uint8_t)strtol(a,&a,16);
    sendKey(0x80,lo,hi); return; }
  if(!strncasecmp(s,"RAW",3)){ uint8_t b[16]; uint8_t n=parseHex(s+3,b,16); sendRaw(b,n);
    Serial.print(F("RAW ")); Serial.println(n); return; }
  Serial.println(F("?  (HELP)"));
}

void setup(){
  Serial.begin(115200);
  idleLine();                     // покой: база HIGH -> линия LOW (как JOG)
  delay(50);
  Serial.println(F("\njog_emulator_rp2040: NPN-каскад (Rb=10к), база как у МК пульта."));
  printHelp(); printCfg();
}
void loop(){
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\r'||c=='\n'){ if(lp){ line[lp]=0; handleLine(line); lp=0; } }
    else if(lp<sizeof(line)-1) line[lp++]=c;
  }
}
