/* ============================================================================
 *  jog_emulator_avc.ino  —  Эмулятор JOG-пульта для Honda/Alpine WinCE-навигации
 *  (AVCLib / SystemUcom, устройство "HKY1:")
 *
 *  Построен на основе реверса avcbus.dll ("AVCLib") и hardkey.dll ("HDK").
 *
 *  ФОРМАТ КАДРА НА ШИНЕ (JOG -> навигатор), подтверждён в валидаторе
 *  avcbus.dll::sub_36035dc:
 *
 *      [0x01] [ID] [LEN] [CKS] [payload x LEN]
 *        |      |     |     |      └─ полезные данные (LEN байт)
 *        |      |     |     └─ контрольная сумма = (сумма payload) & 0xFF
 *        |      |     └─ длина payload (весь кадр = LEN + 4)
 *        |      └─ идентификатор устройства/сообщения
 *        └─ SOF, ВСЕГДА 0x01
 *
 *  Навигатор после приёма валидного кадра отвечает по той же линии:
 *      0x06 (ACK)  — кадр принят и доставлен;
 *      0x15 (NAK)  — ошибка длины/CRC.
 *  Передатчик при отсутствии ACK повторяет кадр до 3 раз ("Send data 3times").
 *
 *  ВАЖНО: физические параметры линии (скорость, инверсия, порядок бит, режим
 *  выхода) пока не зафиксированы на 100% — они вынесены в настройки и
 *  подбираются по ответу навигатора (см. команду SCAN). Линия полудуплексная,
 *  один провод: передаём — драйвим пин, потом отпускаем (INPUT) и слушаем ACK.
 *
 *  ПК-интерфейс: USB-Serial @115200, построчные команды (см. printHelp()).
 * ============================================================================ */

#include <Arduino.h>

// ---- Пин линии данных JOG (через свой буфер/транзистор как у тебя на стенде) ----
#ifndef DATA_PIN
#define DATA_PIN 8
#endif

// ---------------------------- Конфигурация линии ----------------------------
struct LineCfg {
  uint16_t bitUs    = 208;   // длительность бита (208us ≈ 4800 бод)
  uint16_t gapUs    = 800;   // пауза между байтами кадра
  bool     inverted = true;  // true: idle=LOW, start=HIGH, '1'=LOW (как мы мерили)
  bool     msbFirst = false; // порядок бит: false=LSB-first (обычный UART)
  uint8_t  driveMode= 0;     // 0=push-pull, 1=open-drain (тянет только к активному уровню)
  uint16_t ackTmoMs = 25;    // окно ожидания ACK/NAK после кадра
  uint8_t  retries  = 3;     // повторов при отсутствии ACK ("Send data 3times")
  uint8_t  frameId  = 0x80;  // ID по умолчанию (frame[1]) — ПОДОБРАТЬ!
} cfg;

// Уровни: "активный" уровень линии и "уровень покоя" зависят от инверсии.
// inverted: idle=LOW(0), active/start/'0'-data=HIGH(1).
// non-inv : idle=HIGH(1), active/start/'0'-data=LOW(0).
static inline uint8_t idleLevel()   { return cfg.inverted ? LOW  : HIGH; }
static inline uint8_t startLevel()  { return cfg.inverted ? HIGH : LOW;  }
// Кодирование бита данных в физический уровень.
// В обычном UART '1'=mark(=idle level), '0'=space(=start level).
static inline uint8_t bitLevel(uint8_t b) {
  // b — логический бит данных (0/1). '1' = уровень покоя, '0' = активный.
  return b ? idleLevel() : startLevel();
}

// ----------------------------- Драйвер пина --------------------------------
// Драйвим линию к нужному уровню с учётом режима выхода.
static inline void driveLine(uint8_t level) {
  if (cfg.driveMode == 1) {
    // open-drain: активно тянем только к activeLevel, иначе отпускаем (Hi-Z).
    uint8_t active = startLevel();           // активный уровень шины
    if (level == active) { pinMode(DATA_PIN, OUTPUT); digitalWrite(DATA_PIN, active); }
    else                 { pinMode(DATA_PIN, INPUT);  }   // отпускаем — внешняя подтяжка держит idle
  } else {
    // push-pull: жёстко выставляем уровень.
    pinMode(DATA_PIN, OUTPUT);
    digitalWrite(DATA_PIN, level);
  }
}
static inline void releaseLine() { pinMode(DATA_PIN, INPUT); } // отпустить для приёма
static inline uint8_t readLine()  { return digitalRead(DATA_PIN); }

// --------------------------- Передача одного байта --------------------------
// Бит-бэнг: start + 8 data + stop, с учётом инверсии и порядка бит.
static void txByte(uint8_t value) {
  // START
  driveLine(startLevel());
  delayMicroseconds(cfg.bitUs);
  // DATA
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t b = cfg.msbFirst ? ((value >> (7 - i)) & 1) : ((value >> i) & 1);
    driveLine(bitLevel(b));
    delayMicroseconds(cfg.bitUs);
  }
  // STOP (= уровень покоя)
  driveLine(idleLevel());
  delayMicroseconds(cfg.bitUs);
}

// --------------------------- Приём одного байта -----------------------------
// Ждём фронт start-бита в течение timeoutUs, затем сэмплируем 8 бит по центру.
// Возвращает 0..255 или -1 при таймауте.
static int rxByte(uint32_t timeoutUs) {
  releaseLine();
  uint32_t t0 = micros();
  // Ждём перехода линии в активный (start) уровень.
  uint8_t start = startLevel();
  while (readLine() != start) {
    if ((uint32_t)(micros() - t0) > timeoutUs) return -1;
  }
  // Мы на фронте start-бита. Сместимся к центру первого бита данных.
  delayMicroseconds(cfg.bitUs + cfg.bitUs / 2);
  uint8_t v = 0;
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t phys = readLine();
    // physical -> logical bit: '1'(data) соответствует idle level.
    uint8_t logical = (phys == idleLevel()) ? 1 : 0;
    if (cfg.msbFirst) v = (v << 1) | logical;
    else              v |= (logical << i);
    delayMicroseconds(cfg.bitUs);
  }
  // (стоп-бит пропускаем)
  return v;
}

// --------------------------- Сборка и отправка кадра ------------------------
static uint8_t checksum(const uint8_t *p, uint8_t len) {
  uint16_t s = 0;
  for (uint8_t i = 0; i < len; i++) s += p[i];
  return (uint8_t)(s & 0xFF);
}

// Собирает кадр [01 ID LEN CKS payload...] и шлёт. Возвращает:
//  6 (0x06) если пришёл ACK, 0x15 если NAK, -1 если тишина.
static int sendFrame(uint8_t id, const uint8_t *payload, uint8_t len) {
  uint8_t frame[4 + 255];
  frame[0] = 0x01;             // SOF
  frame[1] = id;               // ID
  frame[2] = len;              // LEN
  frame[3] = checksum(payload, len); // CKS = sum(payload)&0xFF
  for (uint8_t i = 0; i < len; i++) frame[4 + i] = payload[i];
  uint16_t total = 4 + len;

  for (uint8_t attempt = 0; attempt < cfg.retries; attempt++) {
    noInterrupts();
    driveLine(idleLevel());          // убедимся, что начинаем с покоя
    for (uint16_t i = 0; i < total; i++) {
      txByte(frame[i]);
      if (i + 1 < total && cfg.gapUs) { driveLine(idleLevel()); delayMicroseconds(cfg.gapUs); }
    }
    releaseLine();                   // отпускаем линию для ответа
    interrupts();
    int r = rxByte((uint32_t)cfg.ackTmoMs * 1000UL);
    if (r == 0x06) return 0x06;      // ACK
    if (r == 0x15) return 0x15;      // NAK
    // тишина — повторяем
    delay(5);
  }
  return -1;
}

// ------------------------- Предустановленные кнопки -------------------------
// payload подбирается по реальному пульту; ниже — ЗАГОТОВКИ (1 байт = код).
// Меняй после того, как SCAN/реальный захват дадут точные значения.
struct Btn { const char *name; uint8_t plen; uint8_t p[4]; };
static const Btn BTNS[] = {
  {"MAP",    1, {0x01}},
  {"MENU",   1, {0x20}},
  {"ENTER",  1, {0x40}},
  {"UP",     1, {0x08}},
  {"DOWN",   1, {0x02}},
  {"LEFT",   1, {0x01}},
  {"RIGHT",  1, {0x04}},
  {"BACK",   1, {0x20}},
  {"ENC_CW", 1, {0x01}},
  {"ENC_CCW",1, {0xFF}},
};
static const uint8_t NBTN = sizeof(BTNS)/sizeof(BTNS[0]);

// ------------------------------- ПК-команды --------------------------------
static int hexNibble(char c){ if(c>='0'&&c<='9')return c-'0'; c|=0x20; if(c>='a'&&c<='f')return c-'a'+10; return -1; }
static uint8_t parseHexBytes(const char *s, uint8_t *out, uint8_t maxn){
  uint8_t n=0; while(*s&&n<maxn){ while(*s==' ')s++; int h=hexNibble(*s++); if(h<0)break; int l=hexNibble(*s); if(l>=0){out[n++]=(h<<4)|l; s++;} else {out[n++]=h;} } return n;
}

static void printCfg(){
  Serial.print(F("CFG bit=")); Serial.print(cfg.bitUs);
  Serial.print(F(" gap=")); Serial.print(cfg.gapUs);
  Serial.print(F(" inv=")); Serial.print(cfg.inverted);
  Serial.print(F(" msb=")); Serial.print(cfg.msbFirst);
  Serial.print(F(" drive=")); Serial.print(cfg.driveMode==1?F("od"):F("pp"));
  Serial.print(F(" acktmo=")); Serial.print(cfg.ackTmoMs);
  Serial.print(F(" retry=")); Serial.print(cfg.retries);
  Serial.print(F(" id=0x")); Serial.println(cfg.frameId, HEX);
}
static void printHelp(){
  Serial.println(F("=== JOG/AVCLib emulator ==="));
  Serial.println(F("Frame = 01 ID LEN CKS payload, CKS=sum(payload)&0xFF; reply 0x06=ACK 0x15=NAK"));
  Serial.println(F("CFG?                         - показать настройки"));
  Serial.println(F("CFG bit=208 gap=800 inv=1 msb=0 drive=pp acktmo=25 retry=3 id=80"));
  Serial.println(F("FRAME <id> <hex payload>     - собрать и послать кадр (id,payload в hex)"));
  Serial.println(F("PL <hex payload>             - кадр с текущим id (cfg.id)"));
  Serial.println(F("RAW <hex bytes>              - послать сырые байты без обёртки"));
  Serial.println(F("BTN <name>                   - предустановленная кнопка"));
  Serial.println(F("BTN?                         - список кнопок"));
  Serial.println(F("MON [ms]                     - слушать линию, печатать принятые байты"));
  Serial.println(F("SCAN <id> <hex payload>      - перебор inv/msb/baud, искать ACK"));
  Serial.println(F("HELP"));
}
static void printResult(int r){
  if(r==0x06) Serial.println(F("-> ACK (0x06)  [принято]"));
  else if(r==0x15) Serial.println(F("-> NAK (0x15)  [дошло, но валидация не прошла]"));
  else Serial.println(F("-> (тишина)    [нет ответа — проверь линию/скорость/инверсию]"));
}

static void cmdCfg(char *args){
  char *tok=strtok(args," ");
  while(tok){
    char *eq=strchr(tok,'='); if(eq){ *eq=0; long v=strtol(eq+1,nullptr,0);
      if(!strcmp(tok,"bit"))cfg.bitUs=v; else if(!strcmp(tok,"gap"))cfg.gapUs=v;
      else if(!strcmp(tok,"inv"))cfg.inverted=v; else if(!strcmp(tok,"msb"))cfg.msbFirst=v;
      else if(!strcmp(tok,"drive"))cfg.driveMode=(eq[1]=='o')?1:0;
      else if(!strcmp(tok,"acktmo"))cfg.ackTmoMs=v; else if(!strcmp(tok,"retry"))cfg.retries=v;
      else if(!strcmp(tok,"id"))cfg.frameId=(uint8_t)v;
    }
    tok=strtok(nullptr," ");
  }
  printCfg();
}

static void cmdScan(uint8_t id, uint8_t *pl, uint8_t plen){
  Serial.println(F("SCAN: перебор inv x msb x baud, ищем ACK 0x06..."));
  const uint16_t bauds[] = {208 /*4800*/, 104 /*9600*/, 416 /*2400*/};
  LineCfg saved = cfg;
  for(uint8_t inv=0; inv<2; inv++)
   for(uint8_t msb=0; msb<2; msb++)
    for(uint8_t bi=0; bi<3; bi++){
      cfg.inverted=inv; cfg.msbFirst=msb; cfg.bitUs=bauds[bi]; cfg.retries=1;
      int r=sendFrame(id, pl, plen);
      Serial.print(F("  inv=")); Serial.print(inv);
      Serial.print(F(" msb=")); Serial.print(msb);
      Serial.print(F(" bit=")); Serial.print(bauds[bi]);
      Serial.print(F(" -> "));
      Serial.println(r==0x06?F("ACK !!!"):(r==0x15?F("NAK"):F("-")));
      delay(40);
    }
  cfg = saved;
  Serial.println(F("SCAN done."));
}

static void cmdMon(uint32_t ms){
  Serial.println(F("MON: слушаю линию..."));
  uint32_t t0=millis(); uint8_t n=0;
  while(millis()-t0<ms){
    int b=rxByte(20000);
    if(b>=0){ if(b<16)Serial.print('0'); Serial.print(b,HEX); Serial.print(' ');
      if(++n>=16){Serial.println();n=0;} }
  }
  Serial.println(F("\nMON done."));
}

static char line[128]; static uint8_t lpos=0;
static void handleLine(char *s){
  while(*s==' ')s++;
  if(!strncasecmp(s,"CFG?",4)){ printCfg(); return; }
  if(!strncasecmp(s,"CFG",3)){ cmdCfg(s+3); return; }
  if(!strncasecmp(s,"HELP",4)){ printHelp(); return; }
  if(!strncasecmp(s,"BTN?",4)){ for(uint8_t i=0;i<NBTN;i++){Serial.print(BTNS[i].name);Serial.print(' ');} Serial.println(); return; }
  if(!strncasecmp(s,"BTN",3)){ char *n=s+3; while(*n==' ')n++;
    for(uint8_t i=0;i<NBTN;i++) if(!strcasecmp(n,BTNS[i].name)){ Serial.print(F("BTN ")); Serial.print(n);
      int r=sendFrame(cfg.frameId, BTNS[i].p, BTNS[i].plen); Serial.print(' '); printResult(r); return; }
    Serial.println(F("неизвестная кнопка (BTN?)")); return; }
  if(!strncasecmp(s,"FRAME",5)){ char *a=s+5; while(*a==' ')a++; uint8_t id=(uint8_t)strtol(a,&a,16);
    uint8_t pl[255]; uint8_t n=parseHexBytes(a,pl,255); Serial.print(F("FRAME id=0x")); Serial.print(id,HEX);
    Serial.print(F(" len=")); Serial.print(n); int r=sendFrame(id,pl,n); Serial.print(' '); printResult(r); return; }
  if(!strncasecmp(s,"PL",2)){ uint8_t pl[255]; uint8_t n=parseHexBytes(s+2,pl,255);
    int r=sendFrame(cfg.frameId,pl,n); printResult(r); return; }
  if(!strncasecmp(s,"RAW",3)){ uint8_t b[64]; uint8_t n=parseHexBytes(s+3,b,64);
    noInterrupts(); driveLine(idleLevel()); for(uint8_t i=0;i<n;i++){txByte(b[i]); if(cfg.gapUs){driveLine(idleLevel());delayMicroseconds(cfg.gapUs);}} releaseLine(); interrupts();
    int r=rxByte((uint32_t)cfg.ackTmoMs*1000UL); Serial.print(F("RAW sent ")); Serial.print(n); Serial.print(' '); printResult(r); return; }
  if(!strncasecmp(s,"SCAN",4)){ char *a=s+4; while(*a==' ')a++; uint8_t id=(uint8_t)strtol(a,&a,16);
    uint8_t pl[255]; uint8_t n=parseHexBytes(a,pl,255); cmdScan(id,pl,n); return; }
  if(!strncasecmp(s,"MON",3)){ long ms=strtol(s+3,nullptr,0); if(ms<=0)ms=3000; cmdMon(ms); return; }
  Serial.println(F("?  (HELP)"));
}

void setup(){
  Serial.begin(115200);
  releaseLine();
  delay(50);
  Serial.println(F("\njog_emulator_avc ready."));
  printHelp(); printCfg();
}
void loop(){
  while(Serial.available()){
    char c=Serial.read();
    if(c=='\r')continue;
    if(c=='\n'){ line[lpos]=0; if(lpos)handleLine(line); lpos=0; }
    else if(lpos<sizeof(line)-1) line[lpos++]=c;
  }
}
