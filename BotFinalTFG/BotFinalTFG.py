import cv2
import os
import time
import threading
import queue
import traceback
from datetime import datetime

from flask import Flask, request, jsonify
from telegram import (
    Update,
    ReplyKeyboardMarkup,
    InlineKeyboardMarkup,
    InlineKeyboardButton
)
from telegram.ext import (
    Application,
    CommandHandler,
    MessageHandler,
    CallbackQueryHandler,
    ContextTypes,
    filters
)

# ============================
# CONFIGURACION
# ============================
TELEGRAM_TOKEN = "8387354749:AAE-Cu5luL8kSsUJ-bEOPPjr0Bkn_I0xeGA"
USUARIOS_AUTORIZADOS = [1417557374, 7996748557]
CHAT_IDS_AVISO = [1417557374]

CAMERA_INDEX = 1
CARPETA_FOTOS = "fotos_temp"
os.makedirs(CARPETA_FOTOS, exist_ok=True)

HOST_FLASK = "0.0.0.0"
PUERTO_FLASK = 5000
DEVICE_RIEGO = "riego_esp32_1"

HEARTBEAT_TIMEOUT_S = 60
ultimo_heartbeat_nivel = time.time()

# ============================
# APP Y ESTADOS
# ============================
app_flask = Flask(__name__)

cola_eventos = queue.Queue()
cola_comandos_riego = []
cola_comandos_nivel = []

estado_sensores_nivel = {}
ultimo_estado_riego = {
    "device_id": DEVICE_RIEGO,
    "tipo": "",
    "detalle": "sin datos",
    "motor1_on": False,
    "motor2_on": False,
    "emergencia": False,
    "sin_luz_motores": False,
    "bomba_fallida": 0,
    "caudal1_lh": 0.0,
    "caudal2_lh": 0.0,
    "litros1": 0.0,
    "litros2": 0.0,
    "litros_total": 0.0,
}

# Estado de sesión de llenado
sesion_activa = False
sesion_fecha = ""
sesion_litros_inicio = 0.0
sesion_tokens = []  # e.g. ["llenado 120.000 L", "emergencia", "resetalarma", "lleno"]

# ============================
# LOG
# ============================
def log_area(area, msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] [{area}] {msg}", flush=True)


def ruta_to_area_in(path):
    if path == "/evento":
        return "HTTP-IN-NIVEL"
    if path.startswith("/riego/"):
        return "HTTP-IN-RIEGO"
    return "HTTP-IN"


def ruta_to_area_out(path):
    if path == "/evento":
        return "HTTP-OUT-NIVEL"
    if path.startswith("/riego/"):
        return "HTTP-OUT-RIEGO"
    return "HTTP-OUT"


def resumen_cola_riego():
    return [item["command"] for item in cola_comandos_riego]


def resumen_cola_nivel():
    return [item["command"] for item in cola_comandos_nivel]


@app_flask.before_request
def log_request_info():
    try:
        area = ruta_to_area_in(request.path)
        ip = request.headers.get("X-Forwarded-For", request.remote_addr)
        log_area(area, f"{request.method} {request.path} ip={ip}")

        args = dict(request.args)
        if args:
            log_area(area, f"args={args}")

        raw = request.get_data(as_text=True)
        if raw:
            log_area(area, f"body={raw}")
    except Exception as e:
        log_area("ERROR", f"before_request: {e}")


@app_flask.after_request
def log_response_info(response):
    try:
        area = ruta_to_area_out(request.path)
        body = response.get_data(as_text=True)
        if len(body) > 400:
            body = body[:400] + "...(truncado)"
        log_area(area, f"{request.method} {request.path} status={response.status_code} body={body}")
    except Exception as e:
        log_area("ERROR", f"after_request: {e}")
    return response


# ============================
# TIEMPO Y TEXTOS
# ============================
def sello_tiempo():
    return datetime.now().strftime("%d/%m/%Y %H:%M:%S")


def fecha_hoy():
    return datetime.now().strftime("%d/%m/%Y")


def construir_texto_estado_riego(timestamp_texto, titulo="Riego"):
    return (
        f"{titulo}\n"
        f"🕒 Hora: {timestamp_texto}\n"
        f"- Motor 1: {'ON' if ultimo_estado_riego['motor1_on'] else 'OFF'}\n"
        f"- Motor 2: {'ON' if ultimo_estado_riego['motor2_on'] else 'OFF'}\n"
        f"- Emergencia: {'SI' if ultimo_estado_riego['emergencia'] else 'NO'}\n"
        f"- Sin luz motores: {'SI' if ultimo_estado_riego.get('sin_luz_motores', False) else 'NO'}\n"
        f"- Bomba fallida: {ultimo_estado_riego['bomba_fallida']}\n"
        f"- Caudal 1: {ultimo_estado_riego['caudal1_lh']:.2f} L/h\n"
        f"- Caudal 2: {ultimo_estado_riego['caudal2_lh']:.2f} L/h\n"
        f"- Litros total: {ultimo_estado_riego['litros_total']:.3f} L\n"
        f"- Detalle: {ultimo_estado_riego['detalle']}"
    )


# ============================
# TECLADOS
# ============================
def teclado_principal():
    return ReplyKeyboardMarkup(
        [
            ["Comenzar", "Riego"],
            ["Camara", "Finalizar"]
        ],
        resize_keyboard=True
    )


def teclado_riego():
    return InlineKeyboardMarkup([
        [
            InlineKeyboardButton("Motor 1 ON", callback_data="riego:M1ON"),
            InlineKeyboardButton("Motor 1 OFF", callback_data="riego:M1OFF")
        ],
        [
            InlineKeyboardButton("Motor 2 ON", callback_data="riego:M2ON"),
            InlineKeyboardButton("Motor 2 OFF", callback_data="riego:M2OFF")
        ],
        [
            InlineKeyboardButton("Encender ambos", callback_data="riego:ALLON"),
            InlineKeyboardButton("Apagar ambos", callback_data="riego:ALLOFF")
        ],
        [
            InlineKeyboardButton("Estado", callback_data="riego:STATUS"),
            InlineKeyboardButton("Litros totales", callback_data="riego:LITROS")
        ],
        [
            InlineKeyboardButton("Reset total", callback_data="riego:RESET_TOTAL"),
            InlineKeyboardButton("Reset alarmas", callback_data="riego:RESET_ALARMAS")
        ]
    ])


# ============================
# COLAS
# ============================
def encolar_comando_riego(command, origin, reason=""):
    item = {
        "command": command,
        "origin": origin,
        "reason": reason,
        "ts": sello_tiempo()
    }
    cola_comandos_riego.append(item)
    log_area(
        "QUEUE-RIEGO",
        f"ENQUEUE origin={origin} command={command} reason={reason} cola={resumen_cola_riego()}"
    )


def encolar_comando_nivel(command, origin, reason=""):
    item = {
        "command": command,
        "origin": origin,
        "reason": reason,
        "ts": sello_tiempo()
    }
    cola_comandos_nivel.append(item)
    log_area(
        "QUEUE-NIVEL",
        f"ENQUEUE origin={origin} command={command} reason={reason} cola={resumen_cola_nivel()}"
    )


def encolar_evento(origin, payload):
    item = {
        "origin": origin,
        "payload": payload,
        "ts": sello_tiempo()
    }
    cola_eventos.put(item)
    log_area("QUEUE-EVENTOS", f"ENQUEUE origin={origin} payload={payload}")


# ============================
# AUTORIZACION
# ============================
async def autorizado(update: Update):
    user_id = update.effective_user.id
    ok = user_id in USUARIOS_AUTORIZADOS
    log_area("AUTH", f"user_id={user_id} autorizado={ok}")
    return ok


# ============================
# CAMARA
# ============================
def capturar_foto():
    log_area("CAMARA", "Intentando abrir camara")
    cap = cv2.VideoCapture(CAMERA_INDEX)

    if not cap.isOpened():
        log_area("CAMARA", "ERROR no se pudo abrir la camara USB")
        return None, "No se pudo abrir la camara USB"

    time.sleep(0.5)
    ok, frame = cap.read()
    cap.release()

    if not ok or frame is None:
        log_area("CAMARA", "ERROR no se pudo capturar imagen")
        return None, "No se pudo capturar la imagen"

    nombre_archivo = os.path.join(CARPETA_FOTOS, f"foto_{int(time.time())}.jpg")
    cv2.imwrite(nombre_archivo, frame)
    log_area("CAMARA", f"Foto guardada en {nombre_archivo}")
    return nombre_archivo, None


async def enviar_foto(chat_id, context, caption):
    log_area("BOT", f"Enviando foto a chat_id={chat_id} caption={caption}")
    ruta_foto, error = capturar_foto()

    if error:
        log_area("BOT", f"ERROR capturando foto: {error}")
        await context.bot.send_message(
            chat_id=chat_id,
            text=f"⚠️ No se pudo capturar la foto automaticamente: {error}"
        )
        return

    with open(ruta_foto, "rb") as foto:
        await context.bot.send_photo(
            chat_id=chat_id,
            photo=foto,
            caption=caption
        )
    log_area("BOT", f"Foto enviada a chat_id={chat_id}")


# ============================
# SESION
# ============================
def iniciar_sesion():
    global sesion_activa, sesion_fecha, sesion_litros_inicio, sesion_tokens
    sesion_activa = True
    sesion_fecha = fecha_hoy()
    sesion_litros_inicio = ultimo_estado_riego.get("litros_total", 0.0)
    sesion_tokens = []


def cerrar_sesion():
    global sesion_activa
    sesion_activa = False


def registrar_token(token):
    if sesion_activa:
        sesion_tokens.append(token)


def construir_resumen_sesion():
    if not sesion_fecha:
        return "No hay datos de sesión."
    litros_actual = ultimo_estado_riego.get("litros_total", 0.0)
    llenado = max(litros_actual - sesion_litros_inicio, 0.0)
    tokens = [f"llenado {llenado:.3f} L"] + sesion_tokens + ["fin"]
    cadena = "->".join(tokens)
    return f"dia {sesion_fecha}: {cadena}"

def finalizar_sesion_con_reset(razon="manual"):
    resumen = construir_resumen_sesion()
    cerrar_sesion()
    encolar_comando_riego("RESET_TOTAL", origin="SERVIDOR", reason=razon)
    encolar_comando_nivel("RESET_TOTAL", origin="SERVIDOR", reason=razon)
    log_area("SESION", f"Sesion cerrada por {razon}. Reset total encolado.")
    return resumen

async def comprobar_heartbeat_nivel(context: ContextTypes.DEFAULT_TYPE):
    global ultimo_heartbeat_nivel
    segundos_sin_senal = time.time() - ultimo_heartbeat_nivel
    if segundos_sin_senal > HEARTBEAT_TIMEOUT_S:
        log_area("NIVEL", f"SIN HEARTBEAT desde hace {int(segundos_sin_senal)} s")
        for chat_id in CHAT_IDS_AVISO:
            try:
                await context.bot.send_message(
                    chat_id=chat_id,
                    text=(
                        f"⚠️ Sin señal del sensor de nivel\n"
                        f"🕒 Última señal hace {int(segundos_sin_senal)} s\n"
                        f"Revisar alimentación del ESP32-nivel."
                    )
                )
            except Exception as e:
                log_area("ERROR", f"Envio alerta heartbeat: {e}")

# ============================
# BOT TELEGRAM
# ============================
async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    log_area("BOT", f"/start recibido user_id={update.effective_user.id}")

    if not await autorizado(update):
        await update.message.reply_text("❌ No autorizado")
        return

    chat_id = update.effective_chat.id
    if chat_id not in CHAT_IDS_AVISO:
        CHAT_IDS_AVISO.append(chat_id)
        log_area("BOT", f"chat_id añadido a avisos: {chat_id}")

    iniciar_sesion()
    marca = sello_tiempo()
    await update.message.reply_text(
        "🤖 Bot iniciado. Sesión de llenado comenzada.",
        reply_markup=teclado_principal()
    )
    await update.message.reply_text(
        construir_texto_estado_riego(marca, "Estado inicial del sistema"),
        reply_markup=teclado_principal()
    )


async def manejar_texto(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global sesion_activa
    log_area("BOT", f"Texto recibido user_id={update.effective_user.id} texto={update.message.text}")

    if not await autorizado(update):
        await update.message.reply_text("❌ No autorizado")
        return

    texto = update.message.text

    if texto == "Camara":
        await update.message.reply_text("📸 Capturando foto...")
        ruta_foto, error = capturar_foto()

        if error:
            await update.message.reply_text(f"❌ Error: {error}")
            return

        with open(ruta_foto, "rb") as foto:
            await update.message.reply_photo(
                photo=foto,
                caption="📷 Foto de la camara USB"
            )

    elif texto == "Estado":
        encolar_comando_riego("STATUS", origin="TELEGRAM", reason="texto_estado")
        await update.message.reply_text(
            "✅ Comando enviado: Estado actual solicitado al ESP32.\n⏳ Esperando respuesta...",
            reply_markup=teclado_principal()
        )

    elif texto == "Riego":
        marca = sello_tiempo()
        await update.message.reply_text(
            construir_texto_estado_riego(marca, "Panel de riego"),
            reply_markup=teclado_riego()
        )

    elif texto == "Comenzar":
        iniciar_sesion()
        marca = sello_tiempo()
        await update.message.reply_text(
            "🚿 Nueva sesión de llenado iniciada.",
            reply_markup=teclado_principal()
        )
        await update.message.reply_text(
            construir_texto_estado_riego(marca, "Estado al comenzar"),
            reply_markup=teclado_principal()
        )

    elif texto == "Finalizar":
        if not sesion_activa:
            await update.message.reply_text(
                "La sesión ya estaba finalizada.",
                reply_markup=teclado_principal()
            )
        else:
            resumen = finalizar_sesion_con_reset(razon="manual")
            await update.message.reply_text(resumen, reply_markup=teclado_principal())

    else:
        await update.message.reply_text(
            "No entiendo ese comando",
            reply_markup=teclado_principal()
        )


async def manejar_boton(update: Update, context: ContextTypes.DEFAULT_TYPE):
    query = update.callback_query
    await query.answer()

    usuario_id = query.from_user.id
    data = query.data or ""

    log_area("TG", f"Callback recibido user_id={usuario_id} data={data}")

    if usuario_id not in USUARIOS_AUTORIZADOS:
        await query.edit_message_text("❌ No autorizado")
        return

    if data.startswith("riego:"):
        comando = data.split(":", 1)[1]

        if comando == "RESET_TOTAL":
            encolar_comando_riego("RESET_TOTAL", origin="TELEGRAM", reason="boton_reset_total")
            encolar_comando_nivel("RESET_TOTAL", origin="TELEGRAM", reason="boton_reset_total")
            texto = "✅ Comando en cola: RESET_TOTAL (riego + nivel)"
            registrar_token("reset_total")
        elif comando == "RESET_ALARMAS":
            encolar_comando_riego("RESET_ALARMAS", origin="TELEGRAM", reason="boton_reset_alarmas")
            texto = "✅ Comando en cola: RESET_ALARMAS"
            registrar_token("resetalarma")
        elif comando == "STATUS":
            encolar_comando_riego("STATUS", origin="TELEGRAM", reason="boton_status")
            texto = "✅ Comando en cola: STATUS\n⏳ Esperando estado actual del ESP32..."
        elif comando == "LITROS":
            encolar_comando_riego("LITROS", origin="TELEGRAM", reason="boton_litros")
            texto = "✅ Comando en cola: LITROS_TOTALES\n⏳ Esperando litros actuales del ESP32..."
        else:
            if comando in ("M1ON", "M2ON", "ALLON") and not sesion_activa:
                iniciar_sesion()
                log_area("SESION", f"Sesion iniciada automaticamente por comando={comando}")
            encolar_comando_riego(comando, origin="TELEGRAM", reason="boton_telegram")
            texto = f"✅ Comando en cola: {comando}"

        await query.edit_message_text(texto, reply_markup=teclado_riego())


# ============================
# EVENTOS NIVEL
# ============================
async def procesar_evento_nivel(evento, context):
    global sesion_activa, ultimo_heartbeat_nivel
    estado = evento.get("estado", "").lower().strip()
    sensor = evento.get("sensor", "desconocido")

    if estado == "heartbeat":
        ultimo_heartbeat_nivel = time.time()
        log_area("NIVEL", f"Heartbeat recibido de sensor={sensor}")
        return
    
    if sensor not in estado_sensores_nivel:
        estado_sensores_nivel[sensor] = False

    activo_prev = estado_sensores_nivel[sensor]
    log_area("NIVEL", f"sensor={sensor} estado={estado} activo_prev={activo_prev}")

    if estado == "lleno":
        if not estado_sensores_nivel[sensor]:
            estado_sensores_nivel[sensor] = True

            encolar_comando_riego("ALLOFF", origin="BALSA", reason=f"{sensor}_lleno")
            encolar_comando_riego("STATUS", origin="BALSA", reason=f"{sensor}_lleno")

            registrar_token("lleno")

            marca = sello_tiempo()
            texto = (
                f"🚨 Nivel máximo detectado en {sensor}\n"
                f"🕒 Hora: {marca}\n"
                f"⛔ Orden prioritaria enviada: parada de los dos motores.\n"
                f"📊 Se solicitará estado final del sistema."
            )

            for chat_id in CHAT_IDS_AVISO:
                try:
                    await context.bot.send_message(chat_id=chat_id, text=texto)
                    await enviar_foto(chat_id, context, "📷 Foto automática por nivel máximo detectado")
                except Exception as e:
                    log_area("ERROR", f"Envio aviso nivel Telegram: {e}")

            # Resumen de sesión al llegar a lleno, si la sesión está activa
            if sesion_activa:
                resumen = finalizar_sesion_con_reset(razon="nivel_lleno")
                for chat_id in CHAT_IDS_AVISO:
                    try:
                        await context.bot.send_message(chat_id=chat_id, text=resumen)
                    except Exception as e:
                        log_area("ERROR", f"Envio resumen por lleno: {e}")
        else:
            log_area("NIVEL", f"IGNORADO sensor={sensor} sigue_lleno activo_prev=True")

    elif estado in ["nolleno", "no_lleno", "libre", "vacio", "normal"]:
        if estado_sensores_nivel[sensor]:
            estado_sensores_nivel[sensor] = False
            log_area("NIVEL", f"RESET sensor={sensor} ya_no_esta_lleno")
        else:
            log_area("NIVEL", f"RESET_IGNORADO sensor={sensor} ya_estaba_inactivo")
    else:
        log_area("NIVEL", f"ESTADO_DESCONOCIDO sensor={sensor} estado={estado}")


# ============================
# EVENTOS RIEGO
# ============================
async def procesar_evento_riego(evento, context):
    global sesion_activa
    log_area("RIEGO", f"Evento recibido para procesar: {evento}")

    tipo = evento.get("tipo", "").lower()
    detalle = evento.get("detalle", "")

    ultimo_estado_riego["device_id"] = evento.get("device_id", DEVICE_RIEGO)
    ultimo_estado_riego["tipo"] = tipo
    ultimo_estado_riego["detalle"] = detalle
    ultimo_estado_riego["motor1_on"] = evento.get("motor1_on", False)
    ultimo_estado_riego["motor2_on"] = evento.get("motor2_on", False)
    ultimo_estado_riego["emergencia"] = evento.get("emergencia", False)
    ultimo_estado_riego["sin_luz_motores"] = evento.get("sin_luz_motores", False)
    ultimo_estado_riego["bomba_fallida"] = int(evento.get("bomba_fallida", 0))
    ultimo_estado_riego["caudal1_lh"] = float(evento.get("caudal1_lh", 0.0))
    ultimo_estado_riego["caudal2_lh"] = float(evento.get("caudal2_lh", 0.0))
    ultimo_estado_riego["litros1"] = float(evento.get("litros1", 0.0))
    ultimo_estado_riego["litros2"] = float(evento.get("litros2", 0.0))
    ultimo_estado_riego["litros_total"] = float(evento.get("litros_total", 0.0))

    log_area(
        "RIEGO",
        f"tipo={tipo} detalle={detalle} motor1_on={ultimo_estado_riego['motor1_on']} "
        f"motor2_on={ultimo_estado_riego['motor2_on']} emergencia={ultimo_estado_riego['emergencia']} "
        f"sin_luz_motores={ultimo_estado_riego['sin_luz_motores']} "
        f"bomba_fallida={ultimo_estado_riego['bomba_fallida']} "
        f"caudal1={ultimo_estado_riego['caudal1_lh']:.2f} caudal2={ultimo_estado_riego['caudal2_lh']:.2f} "
        f"litros_total={ultimo_estado_riego['litros_total']:.3f}"
    )

    marca = sello_tiempo()

    if ultimo_estado_riego["sin_luz_motores"]:
        texto = construir_texto_estado_riego(marca, "⚠️ Posible falta de alimentación en motores")
        registrar_token("emergencia")
        for chat_id in CHAT_IDS_AVISO:
            try:
                await context.bot.send_message(chat_id=chat_id, text=texto)
            except Exception as e:
                log_area("ERROR", f"Envio sin luz Telegram: {e}")
        return

    if tipo == "emergencia":
        texto = construir_texto_estado_riego(marca, "🚨 Emergencia en riego")
        registrar_token("emergencia")
        for chat_id in CHAT_IDS_AVISO:
            try:
                await context.bot.send_message(chat_id=chat_id, text=texto)
            except Exception as e:
                log_area("ERROR", f"Envio emergencia Telegram: {e}")

    elif tipo == "status":
        texto = construir_texto_estado_riego(marca, "📊 Estado solicitado")
        for chat_id in CHAT_IDS_AVISO:
            try:
                await context.bot.send_message(chat_id=chat_id, text=texto)
                await enviar_foto(chat_id, context, "📷 Foto automática junto al estado solicitado")
            except Exception as e:
                log_area("ERROR", f"Envio status Telegram: {e}")

    elif tipo == "estado":
        texto = construir_texto_estado_riego(marca, "⏱️ Estado periódico (60 s)")
        for chat_id in CHAT_IDS_AVISO:
            try:
                await context.bot.send_message(chat_id=chat_id, text=texto)
                await enviar_foto(chat_id, context, "📷 Foto automática del estado periódico")
            except Exception as e:
                log_area("ERROR", f"Envio estado periodico Telegram: {e}")

    elif tipo == "litros":
        texto = (
            f"💧 Litros acumulados totales\n"
            f"🕒 Hora: {marca}\n"
            f"- Total: {ultimo_estado_riego['litros_total']:.3f} L"
        )
        for chat_id in CHAT_IDS_AVISO:
            try:
                await context.bot.send_message(chat_id=chat_id, text=texto)
            except Exception as e:
                log_area("ERROR", f"Envio litros Telegram: {e}")

    elif tipo == "motor":
        if not ultimo_estado_riego["motor1_on"] and not ultimo_estado_riego["motor2_on"]:
            texto = construir_texto_estado_riego(marca, "✅ Estado final (motores apagados)")
            for chat_id in CHAT_IDS_AVISO:
                try:
                    await context.bot.send_message(chat_id=chat_id, text=texto)
                    await enviar_foto(chat_id, context, "📷 Foto del estado final")
                except Exception as e:
                    log_area("ERROR", f"Envio estado final Telegram: {e}")

    elif tipo == "sistema":
        if "reset_alarmas" in detalle.lower():
            registrar_token("resetalarma")


# ============================
# REVISION COLA
# ============================
async def revisar_eventos(context: ContextTypes.DEFAULT_TYPE):
    while not cola_eventos.empty():
        item = cola_eventos.get()
        origin = item.get("origin", "DESCONOCIDO")
        evento = item.get("payload", {})

        log_area("QUEUE-EVENTOS", f"DEQUEUE origin={origin} payload={evento}")

        if origin == "BALSA":
            await procesar_evento_nivel(evento, context)
        elif origin == "RIEGO":
            await procesar_evento_riego(evento, context)
        else:
            log_area("WARN", f"Evento con origen no reconocido: {item}")


# ============================
# FLASK ROUTES
# ============================
@app_flask.route("/", methods=["GET"])
def inicio():
    return "Servidor bot + camara + ESP32 OK", 200


@app_flask.route("/evento", methods=["POST"])
def evento():
    data = request.get_json(silent=True) or {}
    encolar_evento("BALSA", data)
    return jsonify({"ok": True, "mensaje": "Evento nivel recibido"}), 200


@app_flask.route("/riego/evento", methods=["POST"])
def evento_riego():
    data = request.get_json(silent=True) or {}
    encolar_evento("RIEGO", data)
    return jsonify({"ok": True, "mensaje": "Evento riego recibido"}), 200


@app_flask.route("/riego/comando", methods=["GET"])
def comando_riego():
    device_id = request.args.get("device_id", "")
    log_area("RIEGO", f"Poll comando desde device_id={device_id}")

    if device_id != DEVICE_RIEGO:
        log_area("QUEUE-RIEGO", f"REJECT destino={device_id} motivo=device_id_incorrecto")
        return jsonify({"ok": False, "command": ""}), 200

    if cola_comandos_riego:
        item = cola_comandos_riego.pop(0)
        log_area(
            "QUEUE-RIEGO",
            f"DEQUEUE destino={device_id} origin={item['origin']} command={item['command']} "
            f"reason={item['reason']} cola={resumen_cola_riego()}"
        )
        return jsonify({"ok": True, "command": item["command"]}), 200

    log_area("QUEUE-RIEGO", f"EMPTY destino={device_id}")
    return jsonify({"ok": False, "command": ""}), 200


@app_flask.route("/nivel/comando", methods=["GET"])
def comando_nivel():
    device_id = request.args.get("device_id", "")
    log_area("NIVEL", f"Poll comando desde device_id={device_id}")

    if cola_comandos_nivel:
        item = cola_comandos_nivel.pop(0)
        log_area(
            "QUEUE-NIVEL",
            f"DEQUEUE destino={device_id} origin={item['origin']} command={item['command']} "
            f"reason={item['reason']} cola={resumen_cola_nivel()}"
        )
        return jsonify({"ok": True, "command": item["command"]}), 200

    log_area("QUEUE-NIVEL", f"EMPTY destino={device_id}")
    return jsonify({"ok": False, "command": ""}), 200


def ejecutar_flask():
    log_area("FLASK", f"Iniciando Flask en http://{HOST_FLASK}:{PUERTO_FLASK}")
    app_flask.run(host=HOST_FLASK, port=PUERTO_FLASK, debug=False, use_reloader=False)


# ============================
# ERRORES
# ============================
async def error_handler(update: object, context: ContextTypes.DEFAULT_TYPE):
    log_area("ERROR", f"Update causó error: {context.error}")
    tb = "".join(traceback.format_exception(None, context.error, context.error.__traceback__))
    log_area("ERROR", tb)


# ============================
# MAIN
# ============================
def main():
    log_area("MAIN", "Arrancando bot de Telegram")
    app = Application.builder().token(TELEGRAM_TOKEN).build()

    app.add_handler(CommandHandler("start", start))
    app.add_handler(MessageHandler(filters.TEXT & ~filters.COMMAND, manejar_texto))
    app.add_handler(CallbackQueryHandler(manejar_boton))
    app.add_error_handler(error_handler)

    app.job_queue.run_repeating(revisar_eventos, interval=1, first=1)
    app.job_queue.run_repeating(comprobar_heartbeat_nivel, interval=30, first=30)

    hilo_flask = threading.Thread(target=ejecutar_flask, daemon=True)
    hilo_flask.start()

    log_area("MAIN", "Bot iniciado. Esperando mensajes...")
    app.run_polling()


if __name__ == "__main__":
    main()


