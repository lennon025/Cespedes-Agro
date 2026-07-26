#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Céspedes.Agro v2.4 — Script de Riego Automático
=================================================
Este script:
  1. Crea las tablas necesarias en data.db (si no existen).
  2. Entra en un bucle infinito que, cada 60 segundos:
     - Consulta las filas y lotes activos de planificación.
     - Compara la hora actual con las horas de riego programadas.
     - Calcula el volumen de agua necesario según la fase del cultivo.
     - Publica comandos MQTT para abrir/cerrar electroválvulas.
     - Registra cada riego en la tabla log_riego.

Ejecución:
  python riego.py          → primer plano (pruebas)
  sudo systemctl start riego_auto.service  → como servicio (producción)

Dependencias:
  pip install paho-mqtt
"""

import sqlite3
import os
import time
import json
from datetime import datetime, timedelta

# Intentar importar paho-mqtt; si no está, avisar.
try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERROR: paho-mqtt no está instalado. Ejecuta:")
    print("  pip install paho-mqtt")
    exit(1)

# ------------------------------------------------------------
# CONFIGURACIÓN
# ------------------------------------------------------------
DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data.db')
MQTT_BROKER = "192.168.1.71"
MQTT_PORT = 1883
CAUDAL_NOMINAL_L_MIN = 1.5      # caudal típico del YF-S201 en L/min (ajustable)
MARGEN_MINUTOS = 1              # margen para considerar que "es la hora" (±1 min)
INTERVALO_VERIFICACION = 60     # segundos entre cada ciclo de verificación

# Para evitar regar dos veces el mismo día, guardamos en memoria los riegos ya hechos.
riegos_realizados_hoy = set()


# ------------------------------------------------------------
# BASE DE DATOS
# ------------------------------------------------------------
def crear_tablas():
    """Crea las tablas de planificación y log si no existen."""
    conexion = sqlite3.connect(DB_PATH)
    cursor = conexion.cursor()

    # Tabla: planificacion_estructurada
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS planificacion_estructurada (
            id TEXT PRIMARY KEY, area_id TEXT NOT NULL, crop_id TEXT NOT NULL,
            posturas INTEGER NOT NULL, fecha_siembra TEXT NOT NULL,
            tipo_siembra TEXT DEFAULT 'semilla', valvula TEXT DEFAULT 'A',
            hora_riego1 TEXT DEFAULT '06:00', hora_riego2 TEXT DEFAULT NULL,
            riegos_por_dia INTEGER DEFAULT 1, extra_agua REAL DEFAULT 0,
            fecha_germinacion TEXT, fecha_crecimiento TEXT,
            fecha_cosecha_inicio TEXT, fecha_cosecha_fin TEXT,
            activo INTEGER DEFAULT 1
        )
    ''')

    # Tabla: planificacion_domestica
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS planificacion_domestica (
            id TEXT PRIMARY KEY, area_id TEXT NOT NULL, crop_id TEXT NOT NULL,
            macetas INTEGER NOT NULL, personas INTEGER DEFAULT 1,
            fecha_inicio TEXT NOT NULL, tipo_siembra TEXT DEFAULT 'semilla',
            categoria TEXT DEFAULT 'Alimento', fecha_germinacion TEXT,
            fecha_crecimiento TEXT, fecha_cosecha_inicio TEXT,
            fecha_cosecha_fin TEXT, activo INTEGER DEFAULT 1
        )
    ''')

    # Tabla: log_riego
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS log_riego (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            area_id TEXT NOT NULL, valvula TEXT,
            fecha_hora TEXT NOT NULL, litros_aplicados REAL,
            duracion_segundos INTEGER, origen TEXT DEFAULT 'automatico'
        )
    ''')

    conexion.commit()
    conexion.close()
    print("✅ Tablas verificadas/creadas correctamente.")


def get_db():
    """Devuelve una conexión a la base de datos con row_factory."""
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


# ------------------------------------------------------------
# CONEXIÓN MQTT
# ------------------------------------------------------------
def conectar_mqtt():
    """Establece y devuelve un cliente MQTT conectado al broker."""
    cliente = mqtt.Client()
    try:
        cliente.connect(MQTT_BROKER, MQTT_PORT, 60)
        print(f"✅ Conectado al broker MQTT en {MQTT_BROKER}:{MQTT_PORT}")
        return cliente
    except Exception as e:
        print(f"❌ Error conectando al broker MQTT: {e}")
        return None


def publicar_actuador(cliente, area_id, tipo, estado):
    """Publica un comando de actuador en el tópico MQTT correspondiente."""
    topico = f"cespedes/{area_id}/actuadores"
    mensaje = json.dumps({"tipo": tipo, "estado": estado})
    if cliente:
        cliente.publish(topico, mensaje)
        accion = "ABIERTA" if estado == 1 else "CERRADA"
        print(f"   📤 MQTT → {topico} : {tipo} = {accion}")


# ------------------------------------------------------------
# CÁLCULO DE AGUA
# ------------------------------------------------------------
def calcular_agua_por_planta(crop_id, fecha_siembra, dias_desde_siembra):
    """
    Calcula el agua diaria por planta según la fase de riego actual.
    Busca el cultivo en la base de datos de cultivos (cultivos.json).
    """
    cultivos_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'cultivos.json')
    try:
        with open(cultivos_path, 'r') as f:
            cultivos = json.load(f)
    except:
        return 0

    cultivo = next((c for c in cultivos if c['id'] == crop_id), None)
    if not cultivo:
        return 0

    agua_base = cultivo.get('aguaBase', 0)
    fases = cultivo.get('fasesRiego', [])
    coef = 1
    for fase in fases:
        if dias_desde_siembra <= fase['hastaDia']:
            coef = fase['coef']
            break

    return agua_base * coef


# ------------------------------------------------------------
# LÓGICA PRINCIPAL DE RIEGO
# ------------------------------------------------------------
def ejecutar_ciclo_riego(cliente_mqtt):
    """
    Verifica todas las filas y lotes activos para ver si toca regar ahora.
    Si encuentra alguno, calcula el agua, abre la válvula, espera y cierra.
    """
    ahora = datetime.now()
    hora_actual = ahora.strftime("%H:%M")
    hoy = ahora.strftime("%Y-%m-%d")

    conn = get_db()
    cursor = conn.cursor()

    # --------------------------------------------------------
    # 1. Verificar planificación ESTRUCTURADA
    # --------------------------------------------------------
    cursor.execute('''
        SELECT * FROM planificacion_estructurada
        WHERE activo = 1
    ''')
    filas_estructuradas = [dict(row) for row in cursor.fetchall()]

    for fila in filas_estructuradas:
        # Verificar hora de riego 1
        if _es_hora_de_regar(hora_actual, fila['hora_riego1']):
            _regar_fila_estructurada(fila, 1, cliente_mqtt, hoy)
        # Verificar hora de riego 2 (si existe)
        if fila['hora_riego2'] and _es_hora_de_regar(hora_actual, fila['hora_riego2']):
            _regar_fila_estructurada(fila, 2, cliente_mqtt, hoy)

    # --------------------------------------------------------
    # 2. Verificar planificación DOMÉSTICA
    #    (Asumimos un riego diario a las 06:00 por defecto)
    # --------------------------------------------------------
    cursor.execute('''
        SELECT * FROM planificacion_domestica
        WHERE activo = 1
    ''')
    lotes_domesticos = [dict(row) for row in cursor.fetchall()]

    for lote in lotes_domesticos:
        # El doméstico no tiene hora de riego guardada → se riega a las 06:00
        if _es_hora_de_regar(hora_actual, "06:00"):
            _regar_lote_domestico(lote, cliente_mqtt, hoy)

    conn.close()


def _es_hora_de_regar(hora_actual, hora_programada):
    """
    Devuelve True si la hora actual está dentro del margen de ±MARGEN_MINUTOS
    respecto a la hora programada.
    """
    try:
        h_act = datetime.strptime(hora_actual, "%H:%M")
        h_prog = datetime.strptime(hora_programada, "%H:%M")
        diferencia = abs((h_act - h_prog).total_seconds()) / 60
        return diferencia <= MARGEN_MINUTOS
    except:
        return False


def _regar_fila_estructurada(fila, num_riego, cliente, hoy):
    """Ejecuta el riego de una fila estructurada."""
    clave = f"E-{fila['id']}-{num_riego}-{hoy}"
    if clave in riegos_realizados_hoy:
        return  # Ya se regó hoy

    dias = (datetime.strptime(hoy, "%Y-%m-%d") - datetime.strptime(fila['fecha_siembra'], "%Y-%m-%d")).days
    if dias < 0:
        return  # Aún no ha sido sembrada

    agua_por_planta = calcular_agua_por_planta(fila['crop_id'], fila['fecha_siembra'], dias)
    agua_total = agua_por_planta * fila['posturas']
    if agua_total <= 0:
        return

    duracion_segundos = int((agua_total / CAUDAL_NOMINAL_L_MIN) * 60)

    print(f"\n💧 {hoy} {datetime.now().strftime('%H:%M:%S')} — Riego estructurado")
    print(f"   Área: {fila['area_id']} | Cultivo: {fila['crop_id']} | Válvula: {fila['valvula']}")
    print(f"   Agua por planta: {agua_por_planta:.1f} L | Total: {agua_total:.1f} L | Duración: {duracion_segundos}s")

    # Abrir válvula
    publicar_actuador(cliente, fila['area_id'], f"valvula{fila['valvula']}", 1)

    # Esperar el tiempo necesario (simulado: si no hay ESP32, el comando igual se envía)
    time.sleep(min(duracion_segundos, 300))  # máximo 5 minutos por seguridad en simulación

    # Cerrar válvula
    publicar_actuador(cliente, fila['area_id'], f"valvula{fila['valvula']}", 0)

    # Registrar en log
    _registrar_riego(fila['area_id'], fila['valvula'], agua_total, duracion_segundos)
    riegos_realizados_hoy.add(clave)


def _regar_lote_domestico(lote, cliente, hoy):
    """Ejecuta el riego de un lote doméstico."""
    clave = f"D-{lote['id']}-{hoy}"
    if clave in riegos_realizados_hoy:
        return

    dias = (datetime.strptime(hoy, "%Y-%m-%d") - datetime.strptime(lote['fecha_inicio'], "%Y-%m-%d")).days
    if dias < 0:
        return

    agua_por_planta = calcular_agua_por_planta(lote['crop_id'], lote['fecha_inicio'], dias)
    agua_total = agua_por_planta * lote['macetas']
    if agua_total <= 0:
        return

    duracion_segundos = int((agua_total / CAUDAL_NOMINAL_L_MIN) * 60)
    valvula = "A"  # El doméstico usa la válvula A por defecto

    print(f"\n💧 {hoy} {datetime.now().strftime('%H:%M:%S')} — Riego doméstico")
    print(f"   Área: {lote['area_id']} | Cultivo: {lote['crop_id']} | Macetas: {lote['macetas']}")
    print(f"   Agua por planta: {agua_por_planta:.1f} L | Total: {agua_total:.1f} L | Duración: {duracion_segundos}s")

    # Abrir válvula A
    publicar_actuador(cliente, lote['area_id'], "valvulaA", 1)
    time.sleep(min(duracion_segundos, 300))
    publicar_actuador(cliente, lote['area_id'], "valvulaA", 0)

    _registrar_riego(lote['area_id'], valvula, agua_total, duracion_segundos)
    riegos_realizados_hoy.add(clave)


def _registrar_riego(area_id, valvula, litros, duracion):
    """Inserta un registro en la tabla log_riego."""
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute('''
        INSERT INTO log_riego (area_id, valvula, fecha_hora, litros_aplicados, duracion_segundos, origen)
        VALUES (?, ?, ?, ?, ?, 'automatico')
    ''', (area_id, valvula, datetime.now().isoformat(), litros, duracion))
    conn.commit()
    conn.close()
    print(f"   📝 Riego registrado en log_riego.")


# ------------------------------------------------------------
# BUCLE PRINCIPAL
# ------------------------------------------------------------
def main():
    print("=" * 60)
    print("🌱 Céspedes.Agro v2.4 — Sistema de Riego Automático")
    print("=" * 60)

    # 1. Crear tablas si no existen
    crear_tablas()

    # 2. Conectar al broker MQTT
    cliente_mqtt = conectar_mqtt()
    if not cliente_mqtt:
        print("❌ No se pudo conectar a MQTT. El riego automático no funcionará.")
        print("   Verifica que Mosquitto esté corriendo en el broker.")
        # Continuamos igual para que al menos el log muestre el intento.

    # 3. Bucle infinito
    print(f"\n🔄 Iniciando bucle de verificación cada {INTERVALO_VERIFICACION} segundos...")
    print("   (Ctrl+C para detener)\n")

    try:
        while True:
            ahora = datetime.now()
            print(f"⏰ [{ahora.strftime('%Y-%m-%d %H:%M:%S')}] Verificando riegos...")

            if cliente_mqtt:
                ejecutar_ciclo_riego(cliente_mqtt)
            else:
                print("   ⚠️ Sin conexión MQTT. Saltando ciclo.")

            # Limpiar el set de riegos realizados al cambiar de día
            if ahora.strftime("%Y-%m-%d") != datetime.now().strftime("%Y-%m-%d"):
                riegos_realizados_hoy.clear()

            time.sleep(INTERVALO_VERIFICACION)

    except KeyboardInterrupt:
        print("\n🛑 Riego automático detenido por el usuario.")

    finally:
        if cliente_mqtt:
            cliente_mqtt.disconnect()


# ------------------------------------------------------------
# PUNTO DE ENTRADA
# ------------------------------------------------------------
if __name__ == '__main__':
    main()
