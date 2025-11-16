# EE4216 Edge AI / Vision with ESP32-S3

We built a smart door lock that verifies a person’s identity using on‑device face recognition on an ESP32‑S3‑EYE and then commands a second ESP32‑S3 to actuate a servo (door latch). The camera node hosts a lightweight web server for live streaming, takes a capture when motion is triggered, and sends recognition results to the lock controller over TCP on the local network. A browser dashboard provides real‑time video, events, and manual controls. The system demonstrates a complete IoT stack: sensing, edge inference, networking, actuation, and data visualization.

## Project Objectives
1. Use ESP32-S3-EYE
2. Motion-triggered photo and send via TCP.
3. Real-time camera image streaming to browser in local area network.
4. Use pre-trained model for object detection.
5. Send detection results over TCP to another ESP32.
6. Store detection history to an online data server.

### With Additional features such as 
7. Sending alerts via Telegram
8. Radar system to locate area that trigger motion sensor 
9. Hosting a password-protected web UI for manual door control and live monitoring
10. Door lock control (servo + status NeoPixel LED)


In short, a smart door lock that unlock door using facial recognition and a browser dashboard that provides real‑time video, events, and manual controls

## Repository Layout
- `ESP32_s3_EYE/` - ESP-IDF project for the ESP32-S3-EYE camera, including recognition customizations, async networking helpers, and lab test scripts.
- `ESP32_s3_door_lock/ESP32Final.ino` - Arduino sketch for the ESP32-S3 controller that drives actuators, hosts the dashboard, and coordinates with the camera.
- `report/EE4216_Data_Analytics.ipynb` - Notebook that downloads the Supabase dataset and regenerates every chart/stat quoted in the report.


## System Architecture
1. **ESP32-S3-EYE edge vision node** detects faces, stabilizes recognition decisions, streams MJPEG, and captures RGB565 frames for motion events.
2. **ESP32-S3 controller node** manages the servo lock, NeoPixel status light, PIR wake-up, radar sweeps, and exposes a password-protected dashboard.
3. **Event distribution** relies on HTTP, Telegram bots, and Supabase REST endpoints so every detection results in an auditable record.
4. **Analytics + reporting** live in the Jupyter notebook, which queries the same Supabase tables to compute statistics (authorization rate, ultrasonic distances, hourly demand, etc.).

![alt text](/images/system.png)

## Data Flow / Sequence
1. PIR sensor detects heat emission and active ultrasonic sensor as radar to detect if someone is present. If someone is present, the servo will lock on in the direction of
the person and send ESP32-S3-EYE a GET request, which the EYE will send the captured photo to Telegram and Supabase.

2. Camera node captures frames, runs detection; if face is present, runs recognition.

3. If Facial recognition matches, Send authorized ESP32 via TCP and unlock the door lock. Else send denied to ESP32 via TCP. These logs will also be sent to Telegram and Supabase to store history.

4. ESP32 door lock acknowledges cmd and actuate, opening the door lock using servo.

5. Dashboard shows live stream + event log; user can Open/Close manually.
---

## ESP32-S3 Door Controller (`ESP32_s3_door_lock/ESP32Final.ino`)

### Responsibilities
- Hosts the **secure dashboard** (login form, session cookie, `/events` log viewer, manual open/close buttons, embedded `/stream` iframe) on a fixed IP so operators can monitor the lock locally.
- **Controls the hardware**: lock servo, NeoPixel status LED, PIR wake-up line, radar pan servo, and HC-SR04 ultrasonic sensor.
- Runs two FreeRTOS tasks:
  - `RadarTask` sweeps the ultrasonic beam, detects intruders within `DETECT_ON_CM`, and pushes events into a queue.
  - `NetTask` consumes those events to ping the camera `/motion` API, trigger `/capture`, and write structured rows to Supabase.
- **Processes ESP32-S3-EYE recognition webhooks** posted to `/detect` (`authorized,<similarity>` or `denied,0`), then unlocks/locks the servo, updates LEDs, appends ring-buffer events, notifies Telegram, and logs to Supabase.

### HTTP Endpoints
| Path | Method(s) | Purpose |
| --- | --- | --- |
| `/login` | GET/POST | Presents the password form and issues the `session=` cookie on success (`UI_PASS` constant). |
| `/` | GET | Dashboard with live stream iframe and manual controls (requires session). |
| `/events` | GET | Renders the last 25 ring-buffer entries with timestamps, labels, similarity scores, and raw payloads. |
| `/open`, `/close` | GET/POST | Manual overrides that move the servo, change LEDs, log to Supabase, and send Telegram notifications. |
| `/detect` | POST | Called exclusively by the ESP32-S3-EYE; body contains the recognition verdict handled by `handleDetection()`. |

### Sensors, Actuators, and Alerts
- **PIR + Radar sweep**: the PIR gate wakes the radar subsystem to save power; radar then pans via `PAN_SERVO_PIN` while sampling distance with HC-SR04 (`TRIG_PIN`/`ECHO_PIN`).
- **Servo + NeoPixel**: `lockDoor()` and `unlockDoor()` pick servo angles (65 deg locked, 0 deg unlocked) while driving the RGB LED to red/green so status is visible at a glance.
- **Supabase + Telegram**: `uploadSupabaseLogTS()` posts JSON rows to `/rest/v1/detection_logs`, while `sendTelegramMessage()` (inside the sketch) reports every unlock/denial or ultrasonic hit with timestamps from the on-board NTP client.

## UI and local web browser

### Main page
![alt text](/images/mainpage.png)

### Login page 
![alt text](/images/loginpage.png)

### Datalog page 
![alt text](/images/datalog.png)

### Wrongpassword page 
![alt text](/images/wrongpassword.png)

### Note 
- Update the Wi-Fi credentials, static IP, Supabase keys, Telegram bot token/chat ID, and stream URLs at the top of `ESP32Final.ino` before flashing your board.
- The code currently calls `client.setInsecure()` for Supabase HTTPS writes. For production, replace this with proper CA validation.
- Radar thresholds, PIR debounce, and servo angles live near the constants section; tweak them if you change sensors or mechanical linkage.

---

## ESP32-S3-EYE Face Recognition Camera

### What Changed Compared to the Espressif Demo
- **Custom recognition app** (`components/who_app/who_recognition_app/MyRecognitionApp.hpp`) subclasses `WhoRecognitionAppLCD` to:
  - Force recognition runs whenever a face is detected (`detect_result_cb` sets the `RECOGNIZE` bit via `recognition_control.cpp`).
  - Keep a `1 s` stability window before accepting a new decision, inject similarity scores into the HUD, and periodically resend the outcome if the face remains in view.
  - Forward the decision asynchronously to the door controller by queueing `authorized,<similarity>` or `denied,0` payloads for `http://ESP32_Receiver_IP:ESP32_Receiver_Port/ESP32_Receiver_Path`.
- **Background network pipeline** (`main/net_sender.c`, `main/http_sender.c`, `main/telegram_sender.c`):
  - Runs on core 1 so the vision tasks stay responsive.
  - Supports two job types: "plain HTTP POST" (used for lock commands) and "RGB565 snapshot" (converted once to JPEG, then posted to Telegram and Supabase by `send_rgb565_image`).
  - Applies bounded queues, stack sizing for TLS, and heap ownership rules so camera frame buffers are returned immediately.
- **Dual HTTP services** (`main/http_streamer.c`):
  - `start_webserver()` exposes `/` (minimal HTML viewer) and `/stream` (multipart MJPEG) for quick checks.
  - `start_motion()` opens a second server on port `8080`/`32769` dedicated to `/motion`. The door controller (radar task) hits `/motion?ts=<unix>` after a PIR/ultrasonic trigger; the handler copies the latest RGB565 frame, enqueues it for Telegram + Supabase, and replies `OK`.
- **Robust connectivity and timing** (`main/wifi_connect.c`, `main/app_main.cpp`):
  - Supports a static-IP STA profile for the direct ESP32<->ESP32 link plus a WPA2-Enterprise profile for `NUS_STU`.
  - Initializes Wi-Fi/NVS/netif/event loop in a separate `init` task, syncs NTP against `sg.pool.ntp.org`, and disables the on-board LED to reduce power.
  - `recognition_control.*` exposes the event group so remote commands (e.g., from the web UI) can request enrollment or wiping of the local face database.
- **Credentials centralization** (`main/credentials.c/.h`):
  - Stores Wi-Fi, Supabase, Telegram, and MQTT constants in one place so you only edit a single file before flashing.
  - Includes PEM strings for certificate pinning (Telegram API) and CA validation.
- **PC-side testing** (`ESP32_s3_EYE/Testing/receive_esp.py`):
  - Standalone Python script that listens on TCP/5050, decodes the JPEG payload emitted by the firmware, and displays it via OpenCV for latency or quality debugging.


### Runtime Endpoints
| Endpoint | Purpose | Notes |
| --- | --- | --- |
| `http://<camera-ip>/` | Static HTML page that embeds the MJPEG stream. | Served by `start_webserver()` for fast diagnostics. |
| `http://<camera-ip>/stream` | Binary MJPEG stream. | Works with any client that can parse `multipart/x-mixed-replace`. |
| `http://<camera-ip>:8080/motion?ts=<timestamp>` | Captures the most recent RGB565 frame and queues it for Telegram + Supabase. | Called by the door controller's radar task after PIR/ultrasonic trips. |

### Event and Data Flow
1. **Recognition loop** - `frame_cap_pipeline.cpp` feeds the detector, `MyRecognitionApp` throttles results, and `net_sender` POSTs the outcome to the door controller (`/detect`), prompting unlock/lock decisions.
2. **Motion snapshots** - The door controller requests `/motion`; `motion_get_handler` clones the frame and hands it to `net_send_telegram_rgb565_take`, which uploads a JPEG to Telegram (`sendPhoto`) and Supabase object storage (`camera-images` bucket).
3. **Logging** - Both embedded firms log structured events to Supabase's `detection_logs` table. The same keyspace powers the analytics notebook and closes the loop between firmware and BI tooling.

---

## EE4216 Data Analytics Notebook
The notebook in `report/EE4216_Data_Analytics.ipynb` recreates the EE4216 presentation visuals directly from the Supabase REST API (`/rest/v1/detection_logs?select=*`). It uses `pandas`, `matplotlib`, `seaborn`, and `requests`.


### What the Notebook Produces
1. **Event distribution (`1_event_type_distribution.png`)** - Bar chart of how often each `object` value (`authorized`, `denied`, `ultrasonic-motion`, ...) appears in `detection_logs`.
2. **Ultrasonic range histogram (`2_distance_distribution.png`)** - Shows the measured `confidence` values for `ultrasonic-motion` rows (mean/median overlays to validate sensor placement).
3. **Authorization pie chart (`3_authorization_pie.png`)** - Split of successful vs failed unlock attempts.
4. **Hourly load (`4_hourly_distribution.png`)** - Counts events by `created_at.hour` to highlight rush hours.
5. **Summary stats (stdout)** - Prints the date range, per-type counts, authorization ratios, and ultrasonic min/median/max to the notebook output.

Every figure is saved at 300 dpi (with `bbox_inches='tight'`) so you can copy them back into the report without regeneration artifacts.


## Project prototype
![alt text](/images/prototype.png)

![alt text](/images/prototype2.png)


## Conclusion
This project successfully achieved real-time, on-device face recognition using the ESP32-S3-EYE. Integration with a second ESP32-S3 for actuation and a web dashboard for a complete end-to-end IoT system to create a low-latency, privacy-preserving smart lock.

Through this project, we gained practical experience in deploying a pre-built face detection and recognition library, managing constrained resources under FreeRTOS, and designing reliable inter-device communication over TCP. Task synchronization and multitasking also highlighted the trade-off between system responsiveness and resource limitations.

Future work could include integrating liveness detection to prevent spoofing, adding IR illumination for low-light operation, implementing encrypted peer-to-peer communication and incorporating data processing directly onto the dashboard.