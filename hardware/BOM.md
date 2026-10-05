# Bill of materials

| Qty | Part | Notes |
|---|---|---|
| 1 | ESP32 DevKitC (ESP32-WROOM-32) | Any ESP32 board with GPIO34, 25, 26, 27 and 2 exposed |
| 1 | Capacitive soil moisture sensor v1.2 (or v2.0) | Analog output 0–3 V. Do not use resistive probes; they corrode within weeks |
| 1 | 5 V single-channel relay module (opto-isolated) | Or a logic-level N-MOSFET (e.g. IRLZ44N) with a flyback diode for DC pumps |
| 1 | 12 V DC submersible pump, 2–5 W | Or a 230 V pump through the relay; keep mains wiring away from the sensor side |
| 1 | 12 V 2 A power supply | Powers the pump; the ESP32 takes 5 V from a buck converter or USB |
| 1 | 5 V buck converter (12 V in) | If you power the ESP32 from the pump supply |
| 1 | Float switch (tank level) | Normally-open, closes when water is present |
| 1 | 1N4007 or Schottky diode | Across a DC pump if driven by a MOSFET |
| 1 | 100 µF electrolytic capacitor | Across the ESP32 5 V input to ride out pump start-up dips |
| – | Silicone tubing, jumper wires, enclosure | Keep the ESP32 and relay in a dry box |

Approximate cost: INR 900 – 1500 with a 12 V pump and supply.
