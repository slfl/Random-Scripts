// задаем аналоговый пин ардуино куда подключены кнопки
int wheelPin = A5;

// Команды имеющихся на руле кнопок
int VOL_DN = 1;
int VOL_UP = 2;
int UP = 3;
int DOWN = 4;
int MODE = 5;
int MUTE = 6;

// Пины выхода с ардуинки
int VOL_DN_A = 2; //D2
int VOL_UP_A = 3; //D3
int UP_A = 4; //D4
int DOWN_A = 5; //D5
int MODE_A = 6; //D6
int MUTE_A = 7; //D7

// Инициализировать переменные для состояний кнопок
int buttonState_1 = 0;
int buttonState_2 = 0;
int buttonState_3 = 0;
int buttonState_4 = 0;
int buttonState_5 = 0;
int buttonState_6 = 0;

// Эта функция читает сопротивление с кнопок и возвращает код нажатой кнопки, либо 0
// Ищем, какая кнопка соответствует этому сопротивлению
int getR(){
  int r = analogRead(wheelPin); 
  if (r >= 1 && r <= 5) return (1); // VOL_DN_A
  if (r >= 10 && r <= 20) return (2); // VOL_UP_A
  if (r >= 25 && r <= 35) return (3); // UP_A
  if (r >= 50 && r <= 60) return (4); // DOWN_A
  if (r >= 90 && r <= 100) return (5); // MODE_A
  if (r >= 165 && r <= 185) return (6); // MUTE_A
  return (0); // если ни одна из кнопок не нажата, возвращаем 0
}

void setup() {

  pinMode(VOL_DN_A, OUTPUT);
  pinMode(VOL_UP_A, OUTPUT);
  pinMode(UP_A, OUTPUT);
  pinMode(DOWN_A, OUTPUT);
  pinMode(MODE_A, OUTPUT);
  pinMode(MUTE_A, OUTPUT);

  // Debug
  //Serial.begin(9600); // Включаем UART для отладки
}

void loop() {

  int buttonState_1 = getR(); // VOL_DN_A
    if (buttonState_1 == 1) {
      digitalWrite(VOL_DN_A, HIGH); // Set output pin to HIGH (5V)
    } else {
      digitalWrite(VOL_DN_A, LOW); // Set output pin to LOW (0V)
    }

  int buttonState_2 = getR(); // VOL_UP_A
    if (buttonState_2 == 2) {
      digitalWrite(VOL_UP_A, HIGH); // Set output pin to HIGH (5V)
    } else {
      digitalWrite(VOL_UP_A, LOW); // Set output pin to LOW (0V)
    }

  int buttonState_3 = getR(); // UP_A
    if (buttonState_3 == 3) {
      digitalWrite(UP_A, HIGH); // Set output pin to HIGH (5V)
    } else {
      digitalWrite(UP_A, LOW); // Set output pin to LOW (0V)
    }

  int buttonState_4 = getR(); // DOWN_A
    if (buttonState_4 == 4) {
      digitalWrite(DOWN_A, HIGH); // Set output pin to HIGH (5V)
    } else {
      digitalWrite(DOWN_A, LOW); // Set output pin to LOW (0V)
    }

  int buttonState_5 = getR(); // MODE_A
    if (buttonState_5 == 5) {
      digitalWrite(MODE_A, HIGH); // Set output pin to HIGH (5V)
    } else {
      digitalWrite(MODE_A, LOW); // Set output pin to LOW (0V)
    }

  int buttonState_6 = getR(); // MUTE_A
    if (buttonState_6 == 6) {
      digitalWrite(MUTE_A, HIGH); // Set output pin to HIGH (5V)
    } else {
      digitalWrite(MUTE_A, LOW); // Set output pin to LOW (0V)
    }

    // Debug
    //Serial.println(getR()); // выводим номер кнопки в serial

  delay(10);       
}
