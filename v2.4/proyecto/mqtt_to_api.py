#!/usr/bin/env python3
import paho.mqtt.client as mqtt
import requests
import json

API_URL = "http://localhost:5004/api/sensors"

def on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode())
        topic_parts = msg.topic.split('/')
        if len(topic_parts) >= 2:
            payload['area_id'] = topic_parts[1]
        resp = requests.post(API_URL, json=payload, timeout=5)
        if resp.status_code == 200:
            print(f"Guardado: {payload.get('area_id')} - {payload.get('temperatura')}°C")
        else:
            print(f"Error {resp.status_code}: {resp.text}")
    except Exception as e:
        print(f"Error: {e}")

client = mqtt.Client()
client.on_message = on_message
client.connect("localhost", 1883, 60)
client.subscribe("cespedes/+/sensores")
print("Escuchando MQTT y enviando a API...")
client.loop_forever()
