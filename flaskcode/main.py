import paho.mqtt.client as mqtt
import logging
import json
from flask import Flask, request, jsonify, render_template,flash,redirect,url_for
import threading
from dotenv import load_dotenv
import os
import threading
import mysql.connector  # Add this import
from mysql.connector import Error  # Add this import

# MQTT-instellingen
MQTT_BROKER = "142.132.173.211"
MQTT_PORT = 1883
MQTT_TOPIC = "arduino/data"
MQTT_RESPONSE_TOPIC = "arduino/response"


# Laad .env-variabelen
load_dotenv()
API_KEY = os.getenv("API_KEY")

print(f"API Key from .env: {API_KEY}")

# ========== LOGGING ==========

logging.basicConfig(level=logging.DEBUG)
mqtt_logger = logging.getLogger("paho.mqtt")
mqtt_logger.setLevel(logging.DEBUG)

# ========== CONFIGURATIE ==========

# MQTT Authenticatie
MQTT_USERNAME = "eindproject"  # Vul hier je gebruikersnaam in
MQTT_PASSWORD = "W8woord!"  # Vul hier je wachtwoord in

# ========== GLOBAL VARIABLES ==========

mqtt_data = {}  # Dit wordt gebruikt om de ontvangen MQTT-data op te slaan


# ========== MQTT CALLBACKS ==========

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.info("✅ Verbonden met MQTT-broker.")
    else:
        logging.error(f"❌ Verbinding mislukt met statuscode {rc}")
    client.subscribe(MQTT_TOPIC)

def on_message(client, userdata, msg):
    data = msg.payload.decode()
    logging.debug(f"📩 Ontvangen via MQTT: {data}")
    mqtt_data["received_data"] = data
    client.publish(MQTT_RESPONSE_TOPIC, f"Data ontvangen: {data}")
    logging.info(f"📤 Data teruggestuurd naar Arduino: {data}")

# ========== MQTT CLIENT ==========

mqtt_client = mqtt.Client()

# Stel gebruikersnaam en wachtwoord in voor authenticatie
mqtt_client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message

# Verbind met de MQTT-broker
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)

# Start de loop
mqtt_client.loop_start()

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

def save_user_to_mariadb(user_id, name):
    try:
        db = connect_db()
        cursor = db.cursor()

        sql = """INSERT INTO gebruiker_ids (id, naam) VALUES (%s, %s)"""

        cursor.execute(sql, (user_id, name))
        db.commit()

        cursor.close()
        db.close()
        print("✅ Gebruiker succesvol opgeslagen in MariaDB.")
    except mysql.connector.Error as err:
        print(f"❌ Fout bij opslaan van gebruiker: {err}")

def save_oefening_to_maiadb(id,oefening):
    try:
        db = connect_db()
        cursor = db.cursor()

        sql = """INSERT INTO oefeningen (id, oefening) VALUES (%s, %s)"""

        cursor.execute(sql, (id, oefening))
        db.commit()

        cursor.close()
        db.close()
        print("✅ oefening succesvol opgeslagen in MariaDB.")
    except mysql.connector.Error as err:
        print(f"❌ Fout bij opslaan van oefening: {err}")
# ========== FLASK SETUP ==========

app = Flask(__name__)

@app.route('/')
def index():
    logging.info("Rendering dashboard...")
    if mqtt_data:
        logging.info("Data naar HTML: %s", mqtt_data)
        try:
            decoded_data = json.loads(mqtt_data['received_data'])
            logging.info("Decoded data: %s", decoded_data)
        except json.JSONDecodeError as e:
            logging.error("Fout bij het decoderen van de JSON: %s", e)
            decoded_data = None
    else:
        logging.info("Geen data om naar HTML te sturen")
        decoded_data = None

    # Zorg ervoor dat altijd geldige data naar de template wordt gestuurd
    return render_template('dashboard.html', data=decoded_data if decoded_data else {})


@app.route('/latest_data', methods=['GET'])
def latest_data():
    if mqtt_data:
        try:
            decoded_data = json.loads(mqtt_data['received_data'])
            return jsonify({"data": decoded_data}), 200
        except json.JSONDecodeError as e:
            logging.error("Fout bij het decoderen van de JSON: %s", e)
            return jsonify({"data": None}), 500
    else:
        return jsonify({"data": None}), 200

# Flask-route voor login
@app.route("/login", methods=["GET", "POST"])
def login():
    if request.method == "POST":
        user_id = request.form["id"]
        user_name = request.form["name"]

        save_user_to_mariadb(user_id, user_name)
        flash("Gebruiker succesvol aangemaakt!", "success")

        return redirect(url_for("login"))


    return render_template("login.html")

# Flask-route voor toeveoegen oefening
@app.route("/oefening", methods=["GET", "POST"])
def oefening():
    if request.method == "POST":
        oefening_id = request.form["id"]
        new_oefening = request.form["oefening"]

        save_oefening_to_maiadb(oefening_id, new_oefening)
        flash("Oefening succesvol aangemaakt!", "success")

        return redirect(url_for("oefening"))


    return render_template("oefening.html")

@app.route("/database")
def get_database():
    conn = connect_db()
    if conn is None:
        return jsonify({"message": "Database connection failed"}), 500

    cursor = conn.cursor()
    try:
        cursor.execute("""
            SELECT gebruiker_ids.naam AS Gebruiker, oefeningen.oefening AS Oefening, oefeningen.id AS OefeningenID, gebruikers.aantal_herhaling AS Herhalingen, gebruikers.start_tijd AS Start, gebruikers.eind_tijd AS Eind
            FROM gebruikers
            JOIN gebruiker_ids ON gebruikers.paswoord = gebruiker_ids.id
            JOIN oefeningen ON gebruikers.oefening_id = oefeningen.id;
        """)
        rows = cursor.fetchall()
        data = [
            {
                "Gebruiker": row[0],
                "Oefening": row[1],
                "OefeningenID": row[2],  # Hier voeg je de OefeningenID toe
                "Herhalingen": row[3],
                "Start": row[4],
                "Eind": row[5]
            }
            for row in rows
        ]
        return render_template("database.html", data=data)
    except mysql.connector.Error as e:
        return jsonify({"message": f"Database error: {e}"}), 500
    finally:
        cursor.close()
        conn.close()


@app.route('/api/receive', methods=['GET', 'POST'])
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


# API-endpoint om gegevens op te halen
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

# API-endpoint om gegevens op te halen
@app.route('/api/oefeningen', methods=['GET'])
def get_oefeningen():
    # Controleer de API-key
    if request.headers.get('X-API-Key') != API_KEY:
        return jsonify({"message": "Invalid API key"}), 401

    conn = connect_db()
    if conn is None:
        return jsonify({"message": "Database connection failed"}), 500

    cursor = conn.cursor()
    try:
        # Haal gegevens op uit een tabel
        cursor.execute("SELECT id, oefening FROM oefeningen")
        rows = cursor.fetchall()
        data = [{"id": row[0], "oefening": row[1]} for row in rows]
        return jsonify(data), 200
    except mariadb.Error as e:
        return jsonify({"message": f"Database error: {e}"}), 500
    finally:
        cursor.close()
        conn.close()

# ========== MAIN ==========

if __name__ == "__main__":
    # Start de API-server
    app.run(port=5000, debug=True)
