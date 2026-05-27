#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <MFRC522.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include "time.h"

// ================= WIFI =================
#define WIFI_SSID "harin"
#define WIFI_PASSWORD "12345678"

// ================= FIREBASE =================
#define BASE_URL "https://smart-attendance-5d8ea-default-rtdb.asia-southeast1.firebasedatabase.app"

WiFiClientSecure client;

// ================= NTP =================
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800;
const int daylightOffset_sec = 0;

// ================= RFID =================
#define SS_PIN 5
#define RST_PIN 4

MFRC522 rfid(SS_PIN, RST_PIN);

// ================= OLED =================
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ================= LED + BUZZER =================
#define GREEN_LED 26
#define RED_LED 25
#define BUZZER 13

// ================= RTOS =================
QueueHandle_t rfidQueue;
QueueHandle_t firebaseQueue;
SemaphoreHandle_t i2cMutex;
TimerHandle_t statsTimer;

// ================= SESSION =================
bool sessionActive = false;
char sessionOwner[20] = "";

// ================= DATA =================
char scannedUIDs[50][20];

int userCount = 0;
int presentCount = 0;

// ================= MASTER UIDS =================
const char* masterUIDs[] = {

  "c75a973f",
  "91479504"
};

// ================= SERIAL STUDIO =================
// TASK IDs
// 1 = RFID
// 2 = PROCESS
// 3 = FIREBASE
// 4 = TIMER

void sendTaskTrace(int taskID){

  // QUICK PLOT FORMAT
  // TIME,TASKID

  Serial.print(millis());

  Serial.print(",");

  Serial.println(taskID);
}

// ================= MASTER NAME =================
const char* getMasterName(const char* uid){

  if(strcmp(uid, "c75a973f") == 0)
    return "RTOS";

  if(strcmp(uid, "91479504") == 0)
    return "ARM";

  return "UNKNOWN";
}

// ================= STUDENTS =================
typedef struct {
  const char* uid;
  const char* name;
} Student;

// 🔥 ALL 12 STUDENTS
Student students[] = {

  // OLD CARDS
  {"3342f6eb","Dileep"},
  {"33d61710","Manjusha"},
  {"43cc2510","Harshith"},
  {"93881810","Vamsi"},
  {"a3a017e2","Harin"},
  {"b53b6a05","Nithish"},
  {"e3d4b807","Preetham"},
  {"73f90810","Charitha"},
  {"f3020e08","Asritha"},
  {"a3bb0810","Nitin"},
  {"b30f0d10","Venkat Sai"},
  {"b3cd0708","Kishan"}
};

int totalStudents =
sizeof(students)/sizeof(students[0]);

// ================= DATE =================
String getCurrentDate(){

  struct tm timeinfo;

  if(!getLocalTime(&timeinfo))
    return "1970-01-01";

  char dateStr[15];

  strftime(dateStr,
           sizeof(dateStr),
           "%Y-%m-%d",
           &timeinfo);

  return String(dateStr);
}

// ================= TIME =================
String getCurrentTime(){

  struct tm timeinfo;

  if(!getLocalTime(&timeinfo))
    return "00:00";

  char timeStr[10];

  strftime(timeStr,
           sizeof(timeStr),
           "%H:%M",
           &timeinfo);

  return String(timeStr);
}

// ================= OLED =================
void showOLED(String l1,
              String l2,
              String l3){

  display.clearDisplay();

  display.setTextSize(2);
  display.setCursor(0,0);
  display.println(l1);

  display.setTextSize(1);
  display.setCursor(0,30);
  display.println(l2);

  display.setCursor(0,50);
  display.println(l3);

  display.display();
}

// ================= HELPERS =================
bool isMaster(const char* uid){

  for(int i=0;i<2;i++){

    if(strcmp(uid, masterUIDs[i]) == 0)
      return true;
  }

  return false;
}

const char* getName(const char* uid){

  for(int i=0;i<totalStudents;i++){

    if(strcmp(uid, students[i].uid) == 0)
      return students[i].name;
  }

  return "Unknown";
}

bool isAlreadyScanned(const char* uid){

  for(int i=0;i<userCount;i++){

    if(strcmp(uid, scannedUIDs[i]) == 0)
      return true;
  }

  return false;
}

// ================= BUZZER =================
void shortBeep(){

  digitalWrite(GREEN_LED, HIGH);
  digitalWrite(BUZZER, HIGH);

  vTaskDelay(100 / portTICK_PERIOD_MS);

  digitalWrite(BUZZER, LOW);
  digitalWrite(GREEN_LED, LOW);
}

void longBeep(){

  digitalWrite(RED_LED, HIGH);
  digitalWrite(BUZZER, HIGH);

  vTaskDelay(400 / portTICK_PERIOD_MS);

  digitalWrite(BUZZER, LOW);
  digitalWrite(RED_LED, LOW);
}

void doubleBeep(){

  shortBeep();

  vTaskDelay(100 / portTICK_PERIOD_MS);

  shortBeep();
}

// ================= RFID TASK =================
void RFID_Task(void *pv){

  SPI.begin();

  while(1){

    if(!rfid.PICC_IsNewCardPresent()){

      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    if(!rfid.PICC_ReadCardSerial()){

      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    char uid[20] = "";

    for(byte i=0;i<rfid.uid.size;i++){

      char temp[3];

      sprintf(temp,"%02x",
              rfid.uid.uidByte[i]);

      strcat(uid,temp);
    }

    // RTOS TRACE
    sendTaskTrace(1);

    // SEND TO QUEUE
    xQueueSend(rfidQueue,
               uid,
               portMAX_DELAY);

    rfid.PICC_HaltA();

    rfid.PCD_StopCrypto1();

    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
}

// ================= PROCESS TASK =================
void Process_Task(void *pv){

  char uid[20];

  while(1){

    if(xQueueReceive(rfidQueue,
                     &uid,
                     portMAX_DELAY)){

      // RTOS TRACE
      sendTaskTrace(2);

      const char* name =
        getName(uid);

      xSemaphoreTake(i2cMutex,
                     portMAX_DELAY);

      // ===== MASTER =====
      if(isMaster(uid)){

        // START SESSION
        if(!sessionActive){

          sessionActive = true;

          strcpy(sessionOwner, uid);

          showOLED("MASTER",
                    getMasterName(sessionOwner),
                    "START");

          doubleBeep();

          presentCount = 0;
          userCount = 0;
        }

        // END SESSION
        else{

          if(strcmp(uid,
                    sessionOwner) == 0){

            sessionActive = false;

            showOLED("END",
                      "Session",
                      "Done");

            doubleBeep();
          }

          else{

            showOLED("DENIED",
                      "",
                      "Not Owner");

            longBeep();
          }
        }
      }

      // ===== STUDENT =====
      else{

        if(!sessionActive){

          showOLED("OFF",
                    "",
                    "Start First");

          longBeep();
        }

        else if(strcmp(name,
                       "Unknown") == 0){

          showOLED("ERROR",
                    "",
                    "Unauthorized");

          longBeep();
        }

        else if(isAlreadyScanned(uid)){

          showOLED("ERROR",
                    "",
                    "Already Marked");

          longBeep();
        }

        else{

          strcpy(scannedUIDs[userCount++],
                 uid);

          presentCount++;

          showOLED("OK",
                    name,
                    "P:" + String(presentCount));

          shortBeep();

          char data[64];

          sprintf(data,
                  "%s,%s",
                  name,
                  uid);

          xQueueSend(firebaseQueue,
                     data,
                     portMAX_DELAY);
        }
      }

      xSemaphoreGive(i2cMutex);
    }
  }
}

// ================= FIREBASE TASK =================
void Firebase_Task(void *pv){

  char data[64];

  while(1){

    if(xQueueReceive(firebaseQueue,
                     &data,
                     portMAX_DELAY)){

      // RTOS TRACE
      sendTaskTrace(3);

      char name[32];
      char uid[20];

      sscanf(data,
             "%[^,],%s",
             name,
             uid);

      if(WiFi.status() == WL_CONNECTED){

        HTTPClient https;

        String url =
          String(BASE_URL) +
          "/logs/" +
          getMasterName(sessionOwner) +
          "/" +
          getCurrentDate() +
          ".json";

        https.begin(client,
                    url);

        https.addHeader("Content-Type",
                        "application/json");

        String json =
          "{\"name\":\"" + String(name) +
          "\",\"uid\":\"" + String(uid) +
          "\",\"time\":\"" + getCurrentTime() + "\"}";

        https.POST(json);

        https.end();
      }
    }
  }
}

// ================= TIMER CALLBACK =================
void statsCallback(TimerHandle_t xTimer){

  // RTOS TRACE
  sendTaskTrace(4);
}

// ================= SETUP =================
void setup(){

  Serial.begin(115200);

  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  // ===== SPI =====
  SPI.begin();

  rfid.PCD_Init();

  delay(500);

  // ===== OLED =====
  Wire.begin(21,22);

  display.begin(SSD1306_SWITCHCAPVCC,
                0x3C);

  display.clearDisplay();

  display.setTextColor(WHITE);

  showOLED("SMART",
            "Attendance",
            "Starting");

  // ===== WIFI =====
  WiFi.begin(WIFI_SSID,
             WIFI_PASSWORD);

  while(WiFi.status() != WL_CONNECTED){

    delay(500);
  }

  showOLED("WIFI",
            "Connected",
            "");

  client.setInsecure();

  // ===== NTP =====
  configTime(gmtOffset_sec,
             daylightOffset_sec,
             ntpServer);

  // ===== RTOS OBJECTS =====
  rfidQueue =
    xQueueCreate(5,
                 sizeof(char[20]));

  firebaseQueue =
    xQueueCreate(5,
                 sizeof(char[64]));

  i2cMutex =
    xSemaphoreCreateMutex();

  // ===== TASKS =====
  xTaskCreate(
    RFID_Task,
    "RFID",
    4096,
    NULL,
    4,
    NULL
  );

  xTaskCreate(
    Process_Task,
    "PROCESS",
    8192,
    NULL,
    2,
    NULL
  );

  xTaskCreate(
    Firebase_Task,
    "FIREBASE",
    8192,
    NULL,
    1,
    NULL
  );

  // ===== SOFTWARE TIMER =====
  statsTimer =
    xTimerCreate(
      "Stats",
      pdMS_TO_TICKS(5000),
      pdTRUE,
      NULL,
      statsCallback
    );

  xTimerStart(statsTimer,
              0);

  Serial.println("\nSYSTEM READY");
}

// ================= LOOP =================
void loop(){

}