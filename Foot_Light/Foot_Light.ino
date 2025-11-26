// Управление подсветкой ног — Arduino Pro Mini
// Подключения:
// A0 - делитель от габаритов (12V -> ~0..4V)
// A1 - делитель от "двери (потолок)" (12V -> ~0..4V)
// D6 - PWM -> через 100R -> затвор N-MOSFET (исток к GND, сток к лампе)
// Vcc = 5V

const uint8_t PIN_PWM = 6;
const uint8_t PIN_LIGHTS_ADC = A1;  // Сигнал габаритов
const uint8_t PIN_DOOR_ADC   = A2;  // Сигнал двери (потолок)

// Параметры сглаживания
const float alpha = 0.1; // коэффициент EMA (0..1) - меньше = сильнее сглаживание

// Пороговые значения (подкорректируй под свои делители и напряжения)
// ADC читает 0..1023 (для 5V Pro Mini)
const int DOOR_OPEN_THRESHOLD = 200;  // если ниже — считаем дверь открытой (значения зависят от делителя)
const int LIGHTS_ON_THRESHOLD = 400;  // если выше — габариты включены

// Целевая яркость (0..255)
const uint8_t BRIGHTNESS_LIGHTS = 50;  // при включённых габаритах (пример 20%)
const uint8_t BRIGHTNESS_DOOR   = 255;  // при открытой двери (100%)

// Скорость фейда
const uint8_t FADE_STEP = 2;    // шаг яркости при каждом update
const uint16_t UPDATE_MS = 20;  // интервал обновления (ms)

float smoothedDoor = 0.0;
float smoothedLights = 0.0;
uint8_t currentPwm = 0;
uint8_t targetPwm = 0;
unsigned long lastUpdate = 0;

void setup() {
  pinMode(PIN_PWM, OUTPUT);
  analogWrite(PIN_PWM, 0); // Потом начинается инит с замерами
  int d0 = analogRead(PIN_DOOR_ADC);
  int l0 = analogRead(PIN_LIGHTS_ADC);
  smoothedDoor = d0;
  smoothedLights = l0;
  lastUpdate = millis();
}

void loop() {
  // частое чтение и EMA сглаживание
  int rawDoor = analogRead(PIN_DOOR_ADC);
  int rawLights = analogRead(PIN_LIGHTS_ADC);

  smoothedDoor = alpha * rawDoor + (1.0 - alpha) * smoothedDoor;
  smoothedLights = alpha * rawLights + (1.0 - alpha) * smoothedLights;

  // логика приоритетов
  bool doorOpen = (smoothedDoor < DOOR_OPEN_THRESHOLD);   // если ниже — открыт (При открытой двери 0В)
  bool lightsOn = (smoothedLights > LIGHTS_ON_THRESHOLD);

  if (doorOpen) {
    targetPwm = BRIGHTNESS_DOOR;
  } else {
    if (lightsOn) {
      targetPwm = BRIGHTNESS_LIGHTS;
    } else {
      targetPwm = 0;
    }
  }

  // плавный переход к targetPwm
  unsigned long now = millis();
  if (now - lastUpdate >= UPDATE_MS) {
    lastUpdate = now;
    if (currentPwm < targetPwm) {
      uint16_t diff = targetPwm - currentPwm;
      currentPwm += min(FADE_STEP, diff);
    } else if (currentPwm > targetPwm) {
      uint16_t diff = currentPwm - targetPwm;
      currentPwm -= min(FADE_STEP, diff);
    }
    analogWrite(PIN_PWM, currentPwm);
  }
}
