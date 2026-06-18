# Sistema Inalámbrico para el Control de una Balsa de Riego

Trabajo de Fin de Grado — Ingeniería de Telecomunicaciones  
Universidad de Málaga (ETSIT)  
Autor: Álvaro Ortega  

---

## Descripción

Sistema de control y supervisión remota de una balsa de riego mediante
dispositivos ESP32, un servidor Python con Flask y un bot de Telegram
como interfaz de usuario.

El sistema permite arrancar y parar bombas de riego, monitorizar el
caudal en tiempo real, detectar fallos automáticamente y recibir
alertas en el móvil cuando el depósito alcanza el nivel máximo.

---

## Arquitectura del sistema
ESP32-nivel ──POST/GET──▶ Servidor Flask + Bot Telegram ◀──GET── ESP32-riego

- **ESP32-nivel**: detecta el nivel del depósito y notifica al servidor.
- **ESP32-riego**: controla dos bombas mediante relés y mide el caudal
  con sensores Hall.
- **Servidor Flask**: actúa como cerebro del sistema, gestiona eventos
  y comandos, y comunica con el bot de Telegram.
- **Bot de Telegram**: interfaz de usuario para controlar el sistema
  y recibir alertas y fotos en tiempo real.

---

## Estructura del repositorio
├── servidor/
│ └── server.py # Servidor Flask + Bot Telegram
│
├── esp32_nivel/
│ └── esp32_nivel.ino # Firmware ESP32 sensor de nivel
│
├── esp32_riego/
│ └── esp32_riego.ino # Firmware ESP32 control de bombas
│
└── README.md

---

## Hardware utilizado

- 2x ESP32 con WiFi
- 2x Caudalímetros efecto Hall (1150 pulsos/litro)
- 2x Relés para control de bombas
- 1x Sensor analógico de nivel de agua
- 2x LEDs verde + rojo por bomba (indicación de estado)
- 1x Cámara USB conectada al servidor
- PC con Python para ejecutar el servidor

---

## Comunicación entre sistemas

| Origen | Destino | Protocolo | Ruta |
|--------|---------|-----------|------|
| ESP32-nivel | Servidor | HTTP POST | `/evento` |
| ESP32-riego | Servidor | HTTP POST | `/riego/evento` |
| ESP32-riego | Servidor | HTTP GET | `/riego/comando` |
| ESP32-nivel | Servidor | HTTP GET | `/nivel/comando` |
| Bot Telegram | Servidor | Telegram API | — |

---

## Funcionalidades principales

- Encendido y apagado remoto de bombas (individual o ambas)
- Medida de caudal instantáneo en L/h y litros acumulados totales
- Detección automática de fallos:
  - Fallo de caudal en una bomba (caída por debajo del umbral)
  - Falta de alimentación o fallo de caudalímetros (caudal 0 en ambas)
- Tiempo de gracia de 8 s al arranque de cada bomba
- Parada automática de bombas al detectar nivel máximo
- Alertas y fotos automáticas por Telegram
- Heartbeat del ESP32-nivel cada 30 s (alerta si se pierde señal)
- Sesión de llenado con resumen al finalizar
- Reset total (litros a 0 + alarmas) y reset solo de alarmas

---

## Dependencias Python

```bash
pip install flask python-telegram-bot opencv-python
```

---

## Configuración antes de usar

En `esp32_nivel.ino` y `esp32_riego.ino` ajusta:
```cpp
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_CONTRASEÑA";
const char* SERVER_EVENT_URL = "http://IP_DEL_SERVIDOR:5000/...";
```

En `server.py` ajusta:
```python
TELEGRAM_TOKEN = "TU_TOKEN_DE_TELEGRAM"
USUARIOS_AUTORIZADOS = [TU_CHAT_ID]
```