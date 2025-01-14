#include "EMGFilters.h"
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <DIYables_Keypad.h>
#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiS3.h>
#include <time.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <ArduinoHttpClient.h>
#include <PubSubClient.h>
#include "credentials.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

char keys[] = {
    '1', '2', '3',
    '4', '5', '6',
    '7', '8', '9',
    '*', '0', '#'
};

byte rowPins[4] = {12, 8, 7, 6};
byte colPins[3] = {5, 4, 3};
DIYables_Keypad keypad = DIYables_Keypad(keys, rowPins, colPins, 4, 3);

int SensorInputPins[] = {A0};
#define ARR_SIZE(a) (sizeof(a) / sizeof(a[0]))

typedef struct {
  uint8_t index;
  uint16_t buf[16];
  uint32_t sum;
} CycleBuf_t;

CycleBuf_t rectifiedAcBuf[ARR_SIZE(SensorInputPins)];
#define CYCLE_BUF_ADD(cb, val) { cb.sum -= cb.buf[cb.index]; cb.sum += (val); cb.buf[cb.index] = (val); cb.index = (cb.index + 1) % ARR_SIZE(cb.buf); }
#define CYCLE_BUF_MEAN(cb) (cb.sum / ARR_SIZE(cb.buf))

EMGFilters myFilter[ARR_SIZE(SensorInputPins)];
#define SerialToUSB Serial

unsigned long smallinterval = 200;
int repCount = 0;
bool muscleTensed = false;
bool sessionStarted = false;


const int redPin = 9;
const int greenPin = 10;
const int bluePin = 11;

//API instellingen
const char* serverAddress = SERVERADDRESS;
int port = PORT;
const char* apiEndpoint = APIENDPOINT;
const char* apiEndpoint2 = APIENDPOINT2;
const char* apiEndpoint3 = APIENDPOINT3;
const char* apiKey = APIKEY;


// MQTT-instellingen
const char* mqtt_server = "142.132.173.211";
WiFiClient espClient;
PubSubClient mqttClient(espClient);
const char* mqtt_username = MQTT_USERNAME;
const char* mqtt_password = MQTT_WACHTWOORD;

// WiFi-client
WiFiClient wifi;
HttpClient client = HttpClient(wifi, serverAddress, port);

String startTijd = "";
String eindTijd = "";


// Gebruikersinformatie
String enteredCode = "";
String user_id = "";
int exercise_id = 0;
bool loggedIn = false;
bool dataFetched = false;

bool exerciseSelected = false;  // Variabele om bij te houden of een oefening is geselecteerd


// Structuur om gebruikersinformatie op te slaan
struct User {
  String id;
  String name;
};

// Struct om oefeninggegevens op te slaan
struct Exercise {
  String id;
  String oefening;
};

Exercise exercises[10];  // Array om oefeningen op te slaan
int exerciseCount = 0;    // Aantal beschikbare oefeningen

// Array om gebruikers op te slaan
User users[10];
int userCount = 0;  // Aantal gebruikers (moet bijgewerkt worden bij het ophalen van data)


const char* ssid = SSID;
const char* password = PASSWORD;

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600);

// Functie om verbinding te maken met WiFi
void connectToWiFi() {
  Serial.println("Verbinden met WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nVerbonden met WiFi!");
}

// Functie om de huidige tijd op te halen en te formatteren naar "YYYY-MM-DD HH:MM:SS"
String getFormattedTime() {
  time_t rawTime = timeClient.getEpochTime();
  struct tm* timeInfo = localtime(&rawTime);
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeInfo);
  return String(buffer);
}

bool stopSessionFlag = false;

// 🆔 Functie om een UUID te genereren
String generateUUID() {
  char uuid[37];
  sprintf(uuid, "%08lX-%04X-%04X-%04X-%012lX", random(0xFFFFFFFF), random(0xFFFF), (random(0x0FFF) | 0x4000), (random(0x3FFF) | 0x8000), random(0xFFFFFFFFFFFF));
  return String(uuid);
}

void setLEDColor(int red, int green, int blue) {
  analogWrite(redPin, red);
  analogWrite(greenPin, green);
  analogWrite(bluePin, blue);
}

void updateOLED(int envelope, int repCount) {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("EMG Data:");
  oled.setCursor(0, 10);
  oled.print("Envelope: ");
  oled.println(envelope);
  oled.setCursor(0, 20);
  oled.print("Herhalingen: ");
  oled.println(repCount);
  oled.setCursor(0, 30);
  oled.println("'#' voor sessie te stoppen");
  oled.display();
}

void displayOnOLED(String message) {
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println(message);
  oled.display();  // Werk het scherm bij
}

// Functie om de code in te voeren vanaf het klavier
String getEnteredCode() {
  String code = "";
  displayOnOLED("Voer code in:");

  while (true) {
    char key = keypad.getKey();
    if (key) {
      Serial.print("Toets ingedrukt: ");
      Serial.println(key);

      if (key == '#') {
        return code;  // Retourneer de code als '#' wordt ingedrukt
      } else if (key == '*') {
        code = "";  // Reset de code als '*' wordt ingedrukt
        displayOnOLED("Code reset");
      } else {
        code += key;  // Voeg de toets toe aan de code
        displayOnOLED("Code: " + code);
      }
    }
  }
}

// Functie om de HTTP-aanvraag te versturen en gebruikersdata op te halen
void getUserDataFromAPI() {
  Serial.println("Verbinding maken met server...");

  // API-verzoek sturen
  client.beginRequest();
  client.get(apiEndpoint);
  client.sendHeader("X-API-Key", apiKey);
  client.endRequest();

  // Antwoord verwerken
  int statusCode = client.responseStatusCode();
  String response = client.responseBody();

  Serial.print("Status code: ");
  Serial.println(statusCode);

  if (statusCode == 200) {
    // JSON-deserialisatie
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
      Serial.print("Fout bij het verwerken van JSON: ");
      Serial.println(error.c_str());
      return;
    }

    // Debug: Bekijk de ontvangen JSON-gegevens
    serializeJsonPretty(doc, Serial);

    // Controleer of de JSON een array is
    if (!doc.is<JsonArray>()) {
      Serial.println("Fout: Verwachte JSON-array.");
      return;
    }

    // Itereer over de ontvangen JSON-gegevens
    userCount = 0;  // Reset het aantal gebruikers
    for (JsonObject user : doc.as<JsonArray>()) {
      if (userCount < 10) {
        users[userCount].id = user["id"].as<String>();  // Haal de gebruikerscode op
        users[userCount].name = user["naam"].as<String>();  // Haal de gebruikersnaam op
        userCount++;  // Verhoog het aantal gebruikers
      }
    }

    dataFetched = true;  // Markeer dat de data is opgehaald
    Serial.println("Gebruikersdata opgehaald.");
  } else {
    Serial.println("Fout bij het ophalen van gegevens van de server.");
  }
}


void compareCodeWithResponse(String enteredCode) {
  enteredCode.trim();  // Verwijder spaties aan het begin en einde van de string

  bool codeFound = false;

  // Doorloop alle gebruikers in de lijst
  for (int i = 0; i < userCount; i++) {
    String userIdStr = users[i].id;  // Haal de gebruikers-ID op
    user_id = enteredCode;
    // Vergelijk de ingevoerde code met de gebruikers-ID
    if (enteredCode == userIdStr) {
      // Als er een match is, toon de gebruikersnaam
      displayOnOLED("Welkom " + users[i].name);
      Serial.println("Welkom " + users[i].name);
      loggedIn = true;
      codeFound = true;
      break;
    }
  }

  // Als geen match wordt gevonden
  if (!codeFound) {
    displayOnOLED("Code incorrect!");
    Serial.println("Code incorrect!");
  }
}

// Functie om oefeninggegevens op te halen van de server
void getExerciseDataFromAPI() {
  Serial.println("Oefeningen ophalen van server...");

  client.beginRequest();
  client.get(apiEndpoint2);
  client.sendHeader("X-API-Key", apiKey);
  client.endRequest();

  int statusCode = client.responseStatusCode();
  String response = client.responseBody();

  Serial.print("Status code: ");
  Serial.println(statusCode);

  if (statusCode == 200) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, response);
    if (error) {
      Serial.print("Fout bij het verwerken van JSON: ");
      Serial.println(error.f_str());
      return;
    }

    // Itereer over de ontvangen JSON-gegevens
    exerciseCount = 0;
    for (JsonObject exercise : doc.as<JsonArray>()) {
      exercises[exerciseCount].id = exercise["id"].as<String>();
      exercises[exerciseCount].oefening = exercise["oefening"].as<String>();
      exerciseCount++;
    }

    Serial.println("Oefeningen succesvol opgehaald.");
    dataFetched = true;
  } else {
    Serial.println("Fout bij het ophalen van oefeninggegevens.");
  }
}

// Functie om de gebruiker een oefening te laten selecteren
void selectExercise() {
  // Toon de beschikbare oefeningen op het OLED-scherm
  displayOnOLED("Beschikbare oefeningen:");
  for (int i = 0; i < exerciseCount; i++) {
    Serial.println(String(i + 1) + ": " + exercises[i].oefening);
  }

  // Vraag de gebruiker om een oefening-ID in te voeren
  displayOnOLED("Voer oefening-ID in:");
  String selectedExercise = getEnteredCode();
  int exerciseIndex = selectedExercise.toInt() - 1;

  // Controleer of de ingevoerde oefening-ID geldig is
  if (exerciseIndex >= 0 && exerciseIndex < exerciseCount) {
    displayOnOLED("Oefening: " + exercises[exerciseIndex].oefening);
    Serial.println("Geselecteerde oefening: " + exercises[exerciseIndex].oefening);
    exercise_id = selectedExercise.toInt();  // Stel het geselecteerde exercise_id in
  } else {
    displayOnOLED("Ongeldige keuze!");
    Serial.println("Ongeldige keuze.");
  }
}

void processEMGData() {
  static unsigned long lastTime = 0;

  if (millis() - lastTime >= smallinterval) {
    lastTime = millis();

    // Starttijd instellen bij de eerste keer dat de sessie begint
    if (!sessionStarted) {
      sessionStarted = true;  // Sessie markeren als gestart
    }

    for (int i = 0; i < ARR_SIZE(SensorInputPins); i++) {
      int data = analogRead(SensorInputPins[i]);
      int dataAfterFilter = myFilter[i].update(data);

      // Voeg de gefilterde waarde toe aan de cycle buffer
      CYCLE_BUF_ADD(rectifiedAcBuf[i], abs(dataAfterFilter));

      // Bereken de envelopewaarde
      uint16_t envelope = CYCLE_BUF_MEAN(rectifiedAcBuf[i]);

      // Bepaal de status op basis van de envelopewaarde
      bool isTensed = (envelope > 50);

      // LED-kleur instellen op basis van envelopewaarde
      if (envelope < 25) {
        setLEDColor(0, 255, 255);  // Rood - Ontspannen
      } else if (envelope < 50) {
        setLEDColor(0, 255, 0);  // Oranje - Lage spanning
      } else {
        setLEDColor(255, 140, 0);  // Groen - Volledige spanning
      }

      // Detecteer een herhaling
      if (isTensed && !muscleTensed) {
        repCount++;
        SerialToUSB.print("Herhaling gedetecteerd! Aantal reps: ");
        SerialToUSB.println(repCount);
      }

      // Update spierstatus
      muscleTensed = isTensed;

      // OLED-scherm bijwerken als de sessie gestart is
      if (sessionStarted) {
        updateOLED(envelope, repCount);
        // Verzenden van gegevens naar MQTT bij elke update
        String feedback = generateFinalFeedback(repCount, envelope);  // Verplaats deze regel boven de MQTT-aanroep
        sendDataToMQTT(envelope, repCount, feedback);  // Correcte aanroep van de functie
      }
    }
  }
}


String generateFinalFeedback(int repCount, int envelope) {
  if (repCount < 5) {
    return "Te weinig herhalingen, oefening niet goed uitgevoerd.";
  } else if (envelope < 25) {
    return "Te weinig spanning, probeer harder te werken!";
  } else if (envelope > 100) {
    return "Spanning te hoog, probeer te ontspannen!";
  } else if (repCount >= 5 && repCount < 12) {
    return "Goed gedaan, maar werk aan snelheid en consistentie.";
  } else {
    return "Uitstekend! Goede uitvoering van de oefening!";
  }
}


unsigned long lastDebounceTime = 0;  // Houdt bij wanneer de toets voor het laatst werd ingedrukt
unsigned long debounceDelay = 300;   // Debounce tijd in milliseconden

void checkKeypad() {
  static char lastKey = '\0';  // Houdt de vorige ingedrukte toets bij
  char key = keypad.getKey();  // Lees de toets van het keypad

  // Controleer of er een nieuwe toets is ingedrukt en voldoende tijd is verstreken sinds de vorige toets
  if (key && key != lastKey && (millis() - lastDebounceTime) > debounceDelay) {
    lastDebounceTime = millis();  // Reset de debounce timer

    if (key == '#') {
      if (sessionStarted) {
        // Sessie stoppen
        sessionStarted = false;
        timeClient.update();  // Update tijd
        String endTime = getFormattedTime();  // Registreer eindtijd
        SerialToUSB.println("Sessie gestopt.");
        SerialToUSB.print("Eindtijd: ");
        SerialToUSB.println(endTime);
      } else {
        // Nieuwe sessie starten
        sessionStarted = true;
        SerialToUSB.println("Nieuwe sessie gestart.");
      }
    }
    // Update de vorige toets
    lastKey = key;
  }

  // Reset `lastKey` als er geen toets wordt ingedrukt
  if (!key) {
    lastKey = '\0';
  }
}



void countdownTimer(int seconds) {
  for (int i = seconds; i > 0; i--) {
    oled.clearDisplay();                // Wis het display
    oled.setCursor(0, 0);               // Zet de cursor naar de bovenkant
    oled.print("Start over in: ");      // Tekst
    oled.setCursor(0, 20);              // Zet de cursor naar beneden
    oled.print(i);                      // Toont de resterende tijd
    oled.display();                     // Update het display
    delay(1000);                        // Wacht 1 seconde
  }
  oled.clearDisplay();
  oled.setCursor(0, 0);
  oled.println("Go!");                  // Geef aan dat de countdown klaar is
  oled.display();
}

void checkSession() {
  char key = keypad.getKey();
  if (key) {
    if (key == '#') {
      if (sessionStarted) {
        // Stop de sessie als deze gestart is
        sessionStarted = false;

        // Genereer sessiegegevens
        String sessionId = generateUUID();
        int paswoord = user_id.toInt();
        int oefeningId = exercise_id;
        eindTijd = getFormattedTime();
        String feedback = generateFinalFeedback(repCount, CYCLE_BUF_MEAN(rectifiedAcBuf[0]));

        // Toon sessiedata op de Serial Monitor
        Serial.println("Session Data:");
        Serial.print("Session ID: "); SerialToUSB.println(sessionId);
        Serial.print("Exercise ID: "); SerialToUSB.println(oefeningId);
        Serial.print("Repetitions: "); SerialToUSB.println(repCount);
        Serial.print("Start Time: "); SerialToUSB.println(startTijd);
        Serial.print("End Time: "); SerialToUSB.println(eindTijd);
        Serial.print("Feedback: "); SerialToUSB.println(feedback);

        // Verzenden van gegevens naar de server
        sendDataToServer(sessionId, paswoord, oefeningId, repCount, startTijd, eindTijd, feedback);

        // Vraag de gebruiker of ze de sessie willen herstarten of stoppen
        oled.clearDisplay();
        oled.setCursor(0, 0);
        oled.println("Restart?");
        oled.setCursor(0, 20);
        oled.println("[1] Yes / [2] No");
        oled.display();

        // Wacht op gebruikersinvoer met een timeout van 10 seconden
        unsigned long startTime = millis();
        while (true) {
          char choice = keypad.getKey();
          if (choice == '1') {
            SerialToUSB.println("Restarting exercise...");
            countdownTimer(10);

            // Herstart de sessie
            sessionStarted = true;
            repCount = 0;
            break;
          } else if (choice == '2') {
            SerialToUSB.println("Exercise finished.");
            oled.setCursor(0, 30);
            oled.println("Exercise finished");
            oled.setCursor(0, 40);
            oled.println("Saving session...");
            oled.display();
            break;
          }
          
          // Voeg een timeout van 10 seconden toe
          if (millis() - startTime > 10000) {
            SerialToUSB.println("Timeout: No input received.");
            oled.clearDisplay();
            oled.setCursor(0, 0);
            oled.println("No input. Ending session.");
            oled.display();
            break;
          }
        }
      } else {
        // Start een nieuwe sessie als deze nog niet gestart is
        sessionStarted = true;
        repCount = 0;
        SerialToUSB.println("Session started...");
      }
    }
  }
}


void sendDataToServer(String sessionId, int paswoord, int oefeningId, int repCount, String startTijd, String eindTijd, String feedback) {
  // Maak een DynamicJsonDocument voor het JSON-object
  DynamicJsonDocument data(1024);  // Allocate memory for the JSON object

  // Vul het JSON-object met gegevens
  JsonObject jsonData = data.createNestedObject("data");
  jsonData["sessie_id"] = sessionId;
  jsonData["paswoord"] = paswoord;
  jsonData["oefening_id"] = oefeningId;
  jsonData["aantal_herhaling"] = repCount;
  jsonData["start_tijd"] = startTijd;
  jsonData["eind_tijd"] = eindTijd;
  jsonData["feedback_oefening"] = feedback;

  // Converteer het JSON-object naar een string
  String jsonStr;
  serializeJson(data, jsonStr);

  // Verzend de JSON-string (hier gebruik je bijvoorbeeld Serial voor debugging)
  Serial.println("Sending data to server:");
  Serial.println(jsonStr);

  /// Verstuur data via HTTP
  WiFiClient wifi;
  HttpClient client = HttpClient(wifi, serverAddress, port);
  client.beginRequest();
  client.post(apiEndpoint3);
  client.sendHeader("Content-Type", "application/json");
  client.sendHeader("Content-Length", jsonStr.length());  // Gebruik de lengte van de JSON-string
  client.sendHeader("Authorization", String("Bearer ") + apiKey);
  client.beginBody();
  client.print(jsonStr);  // Stuur de JSON-string
  client.endRequest();

  // Antwoord van de server lezen
  int statusCode = client.responseStatusCode();
  String response = client.responseBody();

  Serial.print("Status code: ");
  Serial.println(statusCode);
  Serial.println("Response:");
  Serial.println(response);
}

void sendDataToMQTT(int envelope, int repCount, String feedback) {
  // Maak een DynamicJsonDocument voor het JSON-object
  DynamicJsonDocument data(1024);  // Allocate memory for the JSON object

  // Vul het JSON-object met gegevens
  data["envelope"] = envelope;
  data["repCount"] = repCount;
  data["feedback"] = feedback;

  // Converteer het JSON-object naar een string
  String jsonData;
  serializeJson(data, jsonData);

  // Nu verzend de jsonData via MQTT
  Serial.println("Sending data via MQTT:");
  Serial.println(jsonData);

  // Verzend de JSON-gegevens via MQTT
  mqttClient.publish("arduino/data", jsonData.c_str());

  Serial.println("Data verzonden via MQTT:");
  Serial.println(jsonData);
}

// Functie om MQTT te verbinden
void setup_mqtt() {
  mqttClient.setServer(mqtt_server, 1883);
  mqttClient.setCallback(mqttCallback);

  while (!mqttClient.connected()) {
    Serial.print("Verbinden met MQTT...");
    if (mqttClient.connect("ArduinoClient",mqtt_username, mqtt_password)) {
      Serial.println("Verbonden met MQTT!");
    } else {
      Serial.print("Fout, rc=");
      Serial.print(mqttClient.state());
      delay(5000);
    }
  }
}

// Callback functie voor MQTT-berichten
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Bericht ontvangen op topic: ");
  Serial.println(topic);
}


void setup() {
  Serial.begin(115200);

  // Verbinden met WiFi
  connectToWiFi();

  SerialToUSB.begin(115200);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Fout bij OLED-initialisatie!"));
    while (true);
  }
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("'#' Om sessie te starten");
  oled.display();
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Verbinden met Wi-Fi...");
  }
  Serial.println("Verbonden met Wi-Fi!");
  timeClient.begin();
    // Wacht maximaal 10 seconden om de tijd op te halen
  unsigned long startTime = millis();
  while (!timeClient.update()) {
    if (millis() - startTime > 10000) {
      Serial.println("Kon tijd niet synchroniseren.");
      break;
    }
    delay(500);
  }
  
  // Starttijd opslaan
  startTijd = getFormattedTime();
  Serial.println("Systeem klaar!");
  pinMode(redPin, OUTPUT);
  pinMode(greenPin, OUTPUT);
  pinMode(bluePin, OUTPUT);
  for (int i = 0; i < ARR_SIZE(SensorInputPins); i++) {
    myFilter[i].init(SAMPLE_FREQ_500HZ, NOTCH_FREQ_50HZ, true, true, true);
  }
  SerialToUSB.println("Start met meten...");

  // Verbinden met MQTT
  setup_mqtt();
}


void loop() {
  // Synchroniseer de tijd met de NTP-server alleen als het nodig is
  if (!timeClient.update()) {
    timeClient.update();
  }
  // Haal gebruikersgegevens op als dat nog niet gebeurd is
  if (!dataFetched) {
    getUserDataFromAPI();
  }

  // Verwerk de keypad-input
  checkKeypad();

  // Inlogprocedure en oefeningselectie
  if (dataFetched && !loggedIn) {
    enteredCode = getEnteredCode();
    compareCodeWithResponse(enteredCode);
  } else if (loggedIn && !exerciseSelected) {
    getExerciseDataFromAPI();  // Haal oefeningen op na inloggen
    selectExercise();          // Laat de gebruiker een oefening kiezen
    exerciseSelected = true;   // Markeer dat een oefening is geselecteerd
  }

  // Verwerk de EMG-gegevens alleen als de sessie gestart is
  if (sessionStarted) {

    // Verwerk EMG-data en stuur deze door naar MQTT-dashboard
    processEMGData();


    // Controleer op sessie-einde
    checkSession();
  }

  // Houd MQTT-verbinding actief
  mqttClient.loop();
}
