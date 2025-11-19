// ═════════════════════════════════════════════════════════════
//  ВКЛЮЧИТЬ / ВЫКЛЮЧИТЬ ОТЛАДКУ ЗДЕСЬ:
//  1 = полная отладка каждые 500 мс
//  0 = только важные сообщения (начало звонка, открытие и т.д.)
// ═════════════════════════════════════════════════════════════
#define DEBUG 1
// ═════════════════════════════════════════════════════════════

const int CALL_PIN = A0;      // Сюда подключить линию звонка через 3мОм резисторы
const int OPEN_PIN = 13;      // Сюда подключить кнопку открытия двери

const unsigned long CALL_DELAY = 3000;   // Открыть дверь через ... секунд
const unsigned long OPEN_TIME  = 4000;   // Удерживать кнопку зажатой ... секунд

const int THRESHOLD = 900;      // Порог срабатывания, нужно чуть увеличить, от ложных срабатываний
// Если в режиме покоя 400-450, порог можно установить в 600-700, в случае помех и наводок.
// После этого значения, идёт срабатывание на открытие двери!

bool calling = false;
unsigned long startTime = 0;
unsigned long lastDebugTime = 0;

void setup() {
  pinMode(OPEN_PIN, OUTPUT);
  digitalWrite(OPEN_PIN, HIGH);
  
  Serial.begin(9600);
  Serial.println(F("=== Автооткрыватель домофона запущен ==="));
#if DEBUG
  Serial.println(F("Режим: ПОЛНАЯ ОТЛАДКА"));
  Serial.println(F("Формат: время | val | напряжение | состояние | длительность звонка"));
#else
  Serial.println(F("Режим: только события"));
#endif
  Serial.println();
}

void loop() {
  int raw = analogRead(CALL_PIN);
  float voltage = raw * (5.00 / 1023.0);
  bool ringing = (raw > THRESHOLD);

  if (ringing && !calling) {
    calling = true;
    startTime = millis();
    Serial.println(F("►►► ЗВОНОК НАЧАЛСЯ!"));
  }

  if (!ringing && calling) {
    calling = false;
    digitalWrite(OPEN_PIN, HIGH);
    Serial.println(F("Звонок отменился (ошиблись квартирой?)"));
  }

  if (calling && (millis() - startTime >= CALL_DELAY)) {
    Serial.println(F("ОТКРЫВАЮ ДВЕРЬ НА 4 СЕКУНДЫ!"));
    digitalWrite(OPEN_PIN, LOW);
    delay(OPEN_TIME);
    digitalWrite(OPEN_PIN, HIGH);
    Serial.println(F("Дверь открыта"));

    while (analogRead(CALL_PIN) > THRESHOLD) delay(50);
    
    calling = false;
    Serial.println(F("Звонок закончился, готов к следующему"));
  }

  // ────────────── Отладочный вывод (включается/выключается #define DEBUG) ─────────────
#if DEBUG
  if (millis() - lastDebugTime >= 500) {
    lastDebugTime = millis();

    String state = "ПОКОЙ";
    if (calling && (millis() - startTime < CALL_DELAY)) state = "ЗВОНЯТ";
    if (calling && (millis() - startTime >= CALL_DELAY)) state = "ОТКРЫВАЮ";
    if (digitalRead(OPEN_PIN) == LOW) state = "КНОПКА НАЖАТА";

    unsigned long ringSec = calling ? (millis() - startTime) / 1000 : 0;

    Serial.print(millis() / 1000); Serial.print(F("с | "));
    Serial.print(raw); Serial.print(F(" | "));
    Serial.print(voltage, 3); Serial.print(F("В | "));
    Serial.print(state);
    if (calling) {
      Serial.print(F(" | звонок "));
      Serial.print(ringSec); Serial.print(F("с"));
    }
    Serial.println();
  }
#endif
  // ───────────────────────────────────────────────────────────────────────────────────

  delay(20);
}