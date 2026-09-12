# Setting up the browser demo (one-time, ~5 minutes)

Browser Wokwi cannot compile ESP-IDF, so the shareable project runs `sketch.ino` from
this folder — the Arduino-core port. It is honest about its two differences from the
real firmware: the queue is RAM (the NVS + reboot story lives in `main/` and the host
chaos suite), and it publishes real MQTT to `test.mosquitto.org` through the browser
gateway, falling back to clearly-labeled simulated acks if that public broker is down.
The MQTT client is hand-rolled in the sketch (a port of `test/mqtt_mini.c`), so no
libraries need installing — paste and run.

## Steps

1. Go to https://wokwi.com, **sign in** (saving needs an account), then
   **+ New Project → ESP32** (the plain Arduino ESP32 template — *not* an ESP-IDF one).
2. The editor opens with two tabs: `sketch.ino` and `diagram.json`.
3. Open this folder's [`sketch.ino`](sketch.ino) on GitHub, press **Raw**, select all,
   copy.
4. In Wokwi, click the `sketch.ino` tab, select all, **paste over it**.
   - Pasting over `sketch.ino` is correct *here*: this demo is a single file. (Project
     03's "own tabs" trap was about its *extra* files — there are none in this demo.)
5. **Leave `diagram.json` alone.** The template's ESP32 DevKit-C v4 + serial monitor is
   exactly right. The `diagram.json` in the repo root is for the CI runner — do not
   paste it into the browser project.
6. Press the green **play** button. After the compile, the serial monitor (bottom pane)
   prints the Wi-Fi join, then:

   ```text
   shell: commands: stat | out | in
   ```

   plus either `mqtt: up` (real broker traffic) or the labeled simulated-ack fallback.
7. Try it: click the **input strip at the very BOTTOM of the sim pane** (the narrow
   white bar — easy to miss), type `stat`, Enter. Then `out` — watch
   `saf-queue: depth=...` lines build. Then `in` — watch the drain end in
   `saf: drained, nothing lost`.
8. Name the project **mqtt-store-and-forward** (click the title in the top bar) and
   **Save**.
9. **Share → copy the link**, then open that link in a private/incognito window and
   check it loads *this* sketch and simulates — an unsaved edit or a private project
   shows the stock blink demo or a login wall.
10. Send the URL back: it goes in the README's "Run it in your browser" line and the
    repo homepage (`gh repo edit DwalloE/mqtt-store-and-forward --homepage <url>`).

If the compile fails in the browser (Arduino core versions move), copy the first error
line back and the sketch gets fixed — do not debug it in the browser editor.
