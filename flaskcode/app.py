import paho.mqtt.client as mqtt
import mysql.connector
import requests
import json
from flask import Flask, request, jsonify
import threading
from dotenv import load_dotenv
import os


# ========== CONFIGURATIE ==========

# MQTT-instellingen
MQTT_BROKER = "142.132.173.211"
MQTT_PORT = 1883
MQTT_TOPIC = "arduino/data"
MQTT_RESPONSE_TOPIC = "arduino/response"

# ========== LOGGING ==========

logging.basicConfig(level=logging.DEBUG)
mqtt_logger = logging.getLogger("paho.mqtt")
mqtt_logger.setLevel(logging.DEBUG)


# Laad .env-variabelen
load_dotenv()
API_KEY = os.getenv("API_KEY")

print(f"API Key from .env: {API_KEY}")


# Verbind met MariaDB
from mysql.connector import Error  # Dit is correct als je mysql.connector gebruikt

# Verbind met MariaDB
def connect_db():
    try:
        user = os.getenv("DB_USER")
        password = os.getenv("DB_PASS")
        host = os.getenv("DB_HOST")
        port = int(os.getenv("DB_PORT"))
        database = os.getenv("DB_NAME")

        # Print de verbindingsgegevens voor debug-doeleinden
        print(f"Connecting to MariaDB with: user={user}, host={host}, port={port}, database={database}")

        # Maak de verbinding
        conn = mysql.connector.connect(
            user=user,
            password=password,
            host=host,
            port=port,
            database=database
        )
        return conn
    except Error as e:
        print(f"Error connecting to MariaDB: {e}")
        return None

def save_to_mariadb(data):
    try:
        db = connect_db()
        cursor = db.cursor()
        print(data)
        sql = """INSERT INTO gebruikers (sessie_id, paswoord, oefening_id, aantal_herhaling, start_tijd, eind_tijd, feedback_oefening)
         VALUES (%s, %s, %s, %s, %s, %s, %s)"""

        cursor.execute(sql, (
            data["sessie_id"],
            data["paswoord"],
            data["oefening_id"],
            data["aantal_herhaling"],
            data["start_tijd"],
            data["eind_tijd"],
            data["feedback_oefening"]
        ))

        db.commit()
        cursor.close()
        db.close()
        print("✅ Data opgeslagen in MariaDB.")
    except mysql.connector.Error as err:
        print(f"❌ Fout bij opslaan in MariaDB: {err}")

# ========== MQTT CALLBACKS ==========

def on_connect(client, userdata, flags, rc):
    print("✅ Verbonden met MQTT-broker.")
    client.subscribe(MQTT_TOPIC)

def on_message(client, userdata, msg):
    data = msg.payload.decode()
    print(f"📩 Ontvangen via MQTT: {data}")

    # Data opslaan in MariaDB
    save_to_mariadb(data)

    # Data terugsturen naar de Arduino
    client.publish(MQTT_RESPONSE_TOPIC, f"Data ontvangen: {data}")

# ========== MQTT CLIENT ==========

mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)

# ========== API-SERVER ==========

app = Flask(__name__)

@app.route('/api/data', methods=['GET'])
def get_data():
    # Controleer de API-key
    if request.headers.get('X-API-Key') != API_KEY:
        return jsonify({"message": "Invalid API key"}), 401

    conn = connect_db()
    if conn is None:
        return jsonify({"message": "Database connection failed"}), 500

    cursor = conn.cursor()
    try:
        # Haal gegevens op uit een tabel
        cursor.execute("SELECT id, naam FROM gebruiker_ids")
        rows = cursor.fetchall()
        data = [{"id": row[0], "naam": row[1]} for row in rows]
        return jsonify(data), 200
    except mariadb.Error as e:
        return jsonify({"message": f"Database error: {e}"}), 500
    finally:
        cursor.close()
        conn.close()

@app.route('/')
def index():
    logging.info("Rendering dashboard...")
    # Log de data die naar de HTML wordt gestuurd
    # Log de data die naar de HTML wordt gestuurd
    if mqtt_data:
        logging.info("Data naar HTML: %s", mqtt_data)
        # Decodeer de ontvangen JSON string naar een Python object
        try:
            decoded_data = json.loads(mqtt_data['received_data'])
            logging.info("Decoded data: %s", decoded_data)
        except json.JSONDecodeError as e:
            logging.error("Fout bij het decoderen van de JSON: %s", e)
            decoded_data = None
    else:
        logging.info("Geen data om naar HTML te sturen")
        decoded_data = None
    return render_template('dashboard.html', data=decoded_data)


@app.route('/api/receive', methods=['POST'])
def receive_data():
    token = request.headers.get("Authorization")
    if token != f"Bearer {API_KEY}":
        return jsonify({"error": "Unauthorized"}), 401

    data = request.json.get("data")
    if not data:
        return jsonify({"error": "Geen data ontvangen"}), 400

    print(f"📩 Ontvangen data: {data}")  # Debugging: print de data

    # Opslaan in de database of verdere verwerking
    save_to_mariadb(data)

    return jsonify({"message": "Data succesvol ontvangen"}), 200

@app.route('/api/send', methods=['POST'])
def send_to_arduino():
    token = request.headers.get("Authorization")
    if token != f"Bearer {API_KEY}":
        return jsonify({"error": "Unauthorized"}), 401

    data = request.json.get("data")
    if not data:
        return jsonify({"error": "Geen data ontvangen"}), 400

    # Data naar de Arduino sturen via MQTT
    mqtt_client.publish(MQTT_RESPONSE_TOPIC, data)
    print(f"📤 Data naar Arduino gestuurd: {data}")

    return jsonify({"message": "Data succesvol verzonden"}), 200

# ========== MULTITHREADING ==========

def start_mqtt_loop():
    mqtt_client.loop_forever()

def start_api_server():
    app.run(port=5000, debug=True)

# ========== MAIN ==========

if __name__ == "__main__":
    # Start MQTT-client in een aparte thread
    mqtt_thread = threading.Thread(target=start_mqtt_loop)
    mqtt_thread.start()

    # Start de API-server
    start_api_server()
