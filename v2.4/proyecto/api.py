#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Céspedes.Agro v2.4 — API REST
===============================
Endpoints para áreas, lotes, sensores, cultivos, planificación
estructurada y planificación doméstica.

Ejecutar: python api.py
Puerto: 5004
"""

from flask import Flask, request, jsonify
from flask_cors import CORS
import json
import os
import sqlite3
from datetime import datetime

app = Flask(__name__)
CORS(app)

# ------------------------------------------------------------
# Archivos de datos
# ------------------------------------------------------------
DATA_FILE  = '/home/orangepi/proyecto/data.json'      # áreas y lotes (legado)
DB_FILE    = '/home/orangepi/proyecto/data.db'        # sensores + planificación
CROPS_FILE = '/home/orangepi/proyecto/cultivos.json'  # cultivos

# ------------------------------------------------------------
# INICIALIZAR BASE DE DATOS (sensores)
# ------------------------------------------------------------
def init_db():
    """Crea la tabla de sensor_readings si no existe."""
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('''
        CREATE TABLE IF NOT EXISTS sensor_readings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            area_id TEXT NOT NULL,
            timestamp TEXT NOT NULL,
            data TEXT NOT NULL
        )
    ''')
    c.execute('CREATE INDEX IF NOT EXISTS idx_area_time ON sensor_readings(area_id, timestamp)')
    conn.commit()
    conn.close()

init_db()


# ------------------------------------------------------------
# FUNCIONES AUXILIARES
# ------------------------------------------------------------
def load_data():
    """Carga data.json (áreas y lotes)."""
    if os.path.exists(DATA_FILE):
        with open(DATA_FILE, 'r') as f:
            return json.load(f)
    return {"areas": [], "lotes": {}}

def save_data(data):
    """Guarda data.json."""
    with open(DATA_FILE, 'w') as f:
        json.dump(data, f, indent=2)

def get_db():
    """Devuelve una conexión a la base de datos SQLite con row_factory."""
    conn = sqlite3.connect(DB_FILE)
    conn.row_factory = sqlite3.Row
    return conn


# ================================================================
# ENDPOINTS DE ÁREAS
# ================================================================
@app.route('/api/areas', methods=['GET'])
def get_areas():
    return jsonify(load_data().get('areas', []))

@app.route('/api/areas', methods=['POST'])
def save_areas():
    data = load_data()
    areas = request.get_json()
    if not isinstance(areas, list):
        return jsonify({"error": "Formato incorrecto"}), 400
    data['areas'] = areas
    save_data(data)
    return jsonify({"status": "ok"})


# ================================================================
# ENDPOINTS DE LOTES (Planificador – legado, mantenido por compatibilidad)
# ================================================================
@app.route('/api/lotes/<area_id>', methods=['GET'])
def get_lotes(area_id):
    data = load_data()
    return jsonify(data.get('lotes', {}).get(area_id, []))

@app.route('/api/lotes/<area_id>', methods=['POST'])
def save_lotes(area_id):
    data = load_data()
    lotes = request.get_json()
    if not isinstance(lotes, list):
        return jsonify({"error": "Formato incorrecto"}), 400
    if 'lotes' not in data:
        data['lotes'] = {}
    data['lotes'][area_id] = lotes
    save_data(data)
    return jsonify({"status": "ok"})


# ================================================================
# ENDPOINTS DE SENSORES (Histórico)
# ================================================================
@app.route('/api/sensors', methods=['POST'])
def save_sensor_data():
    payload = request.get_json()
    if not payload:
        return jsonify({"error": "Datos no válidos"}), 400
    area_id = payload.get('area_id')
    if not area_id:
        return jsonify({"error": "Se requiere area_id"}), 400
    data_json = json.dumps(payload)
    timestamp = datetime.now().isoformat()
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('INSERT INTO sensor_readings (area_id, timestamp, data) VALUES (?, ?, ?)',
              (area_id, timestamp, data_json))
    conn.commit()
    conn.close()
    return jsonify({"status": "ok"})

@app.route('/api/sensors/<area_id>', methods=['GET'])
def get_sensor_data(area_id):
    limit = request.args.get('limit', 100, type=int)
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()
    c.execute('SELECT timestamp, data FROM sensor_readings WHERE area_id=? ORDER BY timestamp DESC LIMIT ?',
              (area_id, limit))
    rows = c.fetchall()
    conn.close()
    result = []
    for r in rows:
        record = json.loads(r[1])
        record['timestamp'] = r[0]
        result.append(record)
    return jsonify(result)


# ================================================================
# ENDPOINTS DE CULTIVOS (Editor)
# ================================================================
@app.route('/api/cultivos', methods=['GET'])
def get_cultivos():
    if not os.path.exists(CROPS_FILE):
        return jsonify([])
    with open(CROPS_FILE, 'r') as f:
        return jsonify(json.load(f))

@app.route('/api/cultivos', methods=['POST'])
def add_cultivo():
    nuevo = request.get_json()
    if not nuevo or not nuevo.get('id') or not nuevo.get('nombre'):
        return jsonify({"error": "Se requiere id y nombre"}), 400
    cultivos = []
    if os.path.exists(CROPS_FILE):
        with open(CROPS_FILE, 'r') as f:
            cultivos = json.load(f)
    if any(c['id'] == nuevo['id'] or c['nombre'].lower() == nuevo['nombre'].lower() for c in cultivos):
        return jsonify({"error": "Ya existe un cultivo con ese ID o nombre"}), 400
    cultivos.append(nuevo)
    with open(CROPS_FILE, 'w') as f:
        json.dump(cultivos, f, indent=2)
    return jsonify({"status": "ok"}), 201

@app.route('/api/cultivos/<cultivo_id>', methods=['GET'])
def get_cultivo(cultivo_id):
    if not os.path.exists(CROPS_FILE):
        return jsonify({"error": "Archivo no encontrado"}), 404
    with open(CROPS_FILE, 'r') as f:
        cultivos = json.load(f)
    cultivo = next((c for c in cultivos if c['id'] == cultivo_id), None)
    if not cultivo:
        return jsonify({"error": "Cultivo no encontrado"}), 404
    return jsonify(cultivo)

@app.route('/api/cultivos/<cultivo_id>', methods=['PUT'])
def update_cultivo(cultivo_id):
    datos = request.get_json()
    if not os.path.exists(CROPS_FILE):
        return jsonify({"error": "Archivo no encontrado"}), 404
    with open(CROPS_FILE, 'r') as f:
        cultivos = json.load(f)
    idx = next((i for i, c in enumerate(cultivos) if c['id'] == cultivo_id), None)
    if idx is None:
        return jsonify({"error": "Cultivo no encontrado"}), 404
    cultivos[idx] = datos
    with open(CROPS_FILE, 'w') as f:
        json.dump(cultivos, f, indent=2)
    return jsonify({"status": "ok"})

@app.route('/api/cultivos/<cultivo_id>', methods=['DELETE'])
def delete_cultivo(cultivo_id):
    if not os.path.exists(CROPS_FILE):
        return jsonify({"error": "Archivo no encontrado"}), 404
    with open(CROPS_FILE, 'r') as f:
        cultivos = json.load(f)
    cultivos = [c for c in cultivos if c['id'] != cultivo_id]
    with open(CROPS_FILE, 'w') as f:
        json.dump(cultivos, f, indent=2)
    return jsonify({"status": "ok"})

@app.route('/api/cultivos/importar', methods=['POST'])
def importar_cultivos():
    nuevos = request.get_json()
    if not isinstance(nuevos, list):
        return jsonify({"error": "Se esperaba una lista de cultivos"}), 400
    if not os.path.exists(CROPS_FILE):
        with open(CROPS_FILE, 'w') as f:
            json.dump([], f)
    with open(CROPS_FILE, 'r') as f:
        cultivos = json.load(f)
    agregados = 0
    actualizados = 0
    for cultivo in nuevos:
        idx = next((i for i, c in enumerate(cultivos) if c.get('id') == cultivo.get('id')), None)
        if idx is None:
            cultivos.append(cultivo)
            agregados += 1
        else:
            cultivos[idx] = cultivo
            actualizados += 1
    with open(CROPS_FILE, 'w') as f:
        json.dump(cultivos, f, indent=2)
    return jsonify({
        "status": "ok",
        "agregados": agregados,
        "actualizados": actualizados,
        "total": len(cultivos)
    })


# ================================================================
# NUEVOS ENDPOINTS — Planificación de cultivos (v2.4)
# ================================================================
# Estas rutas gestionan la persistencia de las filas del
# planificador estructurado y los lotes del planificador
# doméstico en la base de datos SQLite.
# Sustituyen al localStorage del navegador.
# ================================================================


# ================================================================
# PLANIFICACIÓN ESTRUCTURADA
# ================================================================

@app.route('/api/planificacion/estructurada/<area_id>', methods=['GET'])
def get_planificacion_estructurada(area_id):
    """Obtiene todas las filas activas de un área estructurada."""
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute(
        'SELECT * FROM planificacion_estructurada WHERE area_id = ? AND activo = 1 ORDER BY fecha_siembra',
        (area_id,)
    )
    filas = [dict(row) for row in cursor.fetchall()]
    conn.close()
    return jsonify(filas)


@app.route('/api/planificacion/estructurada', methods=['POST'])
def post_planificacion_estructurada():
    """Guarda una nueva fila en el planificador estructurado."""
    data = request.get_json()
    if not data:
        return jsonify({'error': 'JSON requerido'}), 400

    for campo in ['id', 'area_id', 'crop_id', 'posturas', 'fecha_siembra']:
        if campo not in data:
            return jsonify({'error': f'Falta el campo: {campo}'}), 400

    conn = get_db()
    cursor = conn.cursor()
    try:
        cursor.execute('''
            INSERT INTO planificacion_estructurada
            (id, area_id, crop_id, posturas, fecha_siembra, tipo_siembra,
             valvula, hora_riego1, hora_riego2, riegos_por_dia, extra_agua,
             fecha_germinacion, fecha_crecimiento, fecha_cosecha_inicio,
             fecha_cosecha_fin, activo)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
        ''', (
            data['id'], data['area_id'], data['crop_id'], data['posturas'],
            data['fecha_siembra'], data.get('tipo_siembra', 'semilla'),
            data.get('valvula', 'A'), data.get('hora_riego1', '06:00'),
            data.get('hora_riego2'), data.get('riegos_por_dia', 1),
            data.get('extra_agua', 0), data.get('fecha_germinacion'),
            data.get('fecha_crecimiento'), data.get('fecha_cosecha_inicio'),
            data.get('fecha_cosecha_fin')
        ))
        conn.commit()
        conn.close()
        return jsonify({'mensaje': 'Fila guardada', 'id': data['id']}), 201
    except sqlite3.IntegrityError:
        conn.close()
        return jsonify({'error': 'Ya existe una fila con ese ID'}), 409


@app.route('/api/planificacion/estructurada/<fila_id>', methods=['PUT'])
def put_planificacion_estructurada(fila_id):
    """Actualiza una fila existente del planificador estructurado."""
    data = request.get_json()
    if not data:
        return jsonify({'error': 'JSON requerido'}), 400

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute('''
        UPDATE planificacion_estructurada SET
            crop_id = ?, posturas = ?, fecha_siembra = ?, tipo_siembra = ?,
            valvula = ?, hora_riego1 = ?, hora_riego2 = ?, riegos_por_dia = ?,
            extra_agua = ?, fecha_germinacion = ?, fecha_crecimiento = ?,
            fecha_cosecha_inicio = ?, fecha_cosecha_fin = ?
        WHERE id = ? AND area_id = ?
    ''', (
        data.get('crop_id'), data.get('posturas'), data.get('fecha_siembra'),
        data.get('tipo_siembra', 'semilla'), data.get('valvula', 'A'),
        data.get('hora_riego1', '06:00'), data.get('hora_riego2'),
        data.get('riegos_por_dia', 1), data.get('extra_agua', 0),
        data.get('fecha_germinacion'), data.get('fecha_crecimiento'),
        data.get('fecha_cosecha_inicio'), data.get('fecha_cosecha_fin'),
        fila_id, data.get('area_id')
    ))
    if cursor.rowcount == 0:
        conn.close()
        return jsonify({'error': 'Fila no encontrada'}), 404
    conn.commit()
    conn.close()
    return jsonify({'mensaje': 'Fila actualizada'}), 200


@app.route('/api/planificacion/estructurada/<fila_id>', methods=['DELETE'])
def delete_planificacion_estructurada(fila_id):
    """Elimina (desactiva) una fila del planificador estructurado."""
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute(
        'UPDATE planificacion_estructurada SET activo = 0 WHERE id = ?',
        (fila_id,)
    )
    if cursor.rowcount == 0:
        conn.close()
        return jsonify({'error': 'Fila no encontrada'}), 404
    conn.commit()
    conn.close()
    return jsonify({'mensaje': 'Fila eliminada'}), 200


# ================================================================
# PLANIFICACIÓN DOMÉSTICA
# ================================================================

@app.route('/api/planificacion/domestica/<area_id>', methods=['GET'])
def get_planificacion_domestica(area_id):
    """Obtiene todos los lotes activos de un área doméstica."""
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute(
        'SELECT * FROM planificacion_domestica WHERE area_id = ? AND activo = 1 ORDER BY fecha_inicio',
        (area_id,)
    )
    lotes = [dict(row) for row in cursor.fetchall()]
    conn.close()
    return jsonify(lotes)


@app.route('/api/planificacion/domestica', methods=['POST'])
def post_planificacion_domestica():
    """Guarda un nuevo lote en el planificador doméstico."""
    data = request.get_json()
    if not data:
        return jsonify({'error': 'JSON requerido'}), 400

    for campo in ['id', 'area_id', 'crop_id', 'macetas', 'fecha_inicio']:
        if campo not in data:
            return jsonify({'error': f'Falta el campo: {campo}'}), 400

    conn = get_db()
    cursor = conn.cursor()
    try:
        cursor.execute('''
            INSERT INTO planificacion_domestica
            (id, area_id, crop_id, macetas, personas, fecha_inicio,
             tipo_siembra, categoria, fecha_germinacion, fecha_crecimiento,
             fecha_cosecha_inicio, fecha_cosecha_fin, activo)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
        ''', (
            data['id'], data['area_id'], data['crop_id'], data['macetas'],
            data.get('personas', 1), data['fecha_inicio'],
            data.get('tipo_siembra', 'semilla'), data.get('categoria', 'Alimento'),
            data.get('fecha_germinacion'), data.get('fecha_crecimiento'),
            data.get('fecha_cosecha_inicio'), data.get('fecha_cosecha_fin')
        ))
        conn.commit()
        conn.close()
        return jsonify({'mensaje': 'Lote guardado', 'id': data['id']}), 201
    except sqlite3.IntegrityError:
        conn.close()
        return jsonify({'error': 'Ya existe un lote con ese ID'}), 409


@app.route('/api/planificacion/domestica/<lote_id>', methods=['PUT'])
def put_planificacion_domestica(lote_id):
    """Actualiza un lote existente del planificador doméstico."""
    data = request.get_json()
    if not data:
        return jsonify({'error': 'JSON requerido'}), 400

    conn = get_db()
    cursor = conn.cursor()
    cursor.execute('''
        UPDATE planificacion_domestica SET
            crop_id = ?, macetas = ?, personas = ?, fecha_inicio = ?,
            tipo_siembra = ?, categoria = ?, fecha_germinacion = ?,
            fecha_crecimiento = ?, fecha_cosecha_inicio = ?, fecha_cosecha_fin = ?
        WHERE id = ? AND area_id = ?
    ''', (
        data.get('crop_id'), data.get('macetas'), data.get('personas', 1),
        data.get('fecha_inicio'), data.get('tipo_siembra', 'semilla'),
        data.get('categoria', 'Alimento'), data.get('fecha_germinacion'),
        data.get('fecha_crecimiento'), data.get('fecha_cosecha_inicio'),
        data.get('fecha_cosecha_fin'), lote_id, data.get('area_id')
    ))
    if cursor.rowcount == 0:
        conn.close()
        return jsonify({'error': 'Lote no encontrado'}), 404
    conn.commit()
    conn.close()
    return jsonify({'mensaje': 'Lote actualizado'}), 200


@app.route('/api/planificacion/domestica/<lote_id>', methods=['DELETE'])
def delete_planificacion_domestica(lote_id):
    """Elimina (desactiva) un lote del planificador doméstico."""
    conn = get_db()
    cursor = conn.cursor()
    cursor.execute(
        'UPDATE planificacion_domestica SET activo = 0 WHERE id = ?',
        (lote_id,)
    )
    if cursor.rowcount == 0:
        conn.close()
        return jsonify({'error': 'Lote no encontrado'}), 404
    conn.commit()
    conn.close()
    return jsonify({'mensaje': 'Lote eliminado'}), 200


# ================================================================
# ARRANQUE DEL SERVIDOR
# ================================================================
if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5004, debug=False)
