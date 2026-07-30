# Temperatursensor mit Xiao nRF52840 und SHT40

Der Xiao nRF52840 von Seeed Studio ist ein stromsparender Mikrocontroller mit BLE-Funkmodul, der sich durch sein kompaktes Design gut für Smart-Home-Projekte eignet.

## Inspiration

Ich habe das [Video](https://youtu.be/6gioxm1IDwU?si=S7Y5fXOx5UpZ-Sa2) von Microamp Home gesehen, in dem er mit dem Xiao dieses Projekt realisiert und dank des stromsparenden MCUs erstaunliche Laufzeiten erreicht. Da ich für mein Smart Home bereits einen Ikea-Alpstuga-Luftqualitätssensor besitze, wollte ich diese Messwerte mit der Außenluft vergleichen. Der Wetterbericht in meiner Gegend ist mir zu ungenau, deshalb wollte ich dieses Projekt selbst bauen und außerhalb meiner Wohnung befestigen.

## Änderungen

Was wurde im Vergleich zum Original / zur Inspiration verändert oder verbessert?

- **Änderung 1:** Polling-Intervall von 5 Minuten auf 30 Sekunden reduziert, damit es zum Alpstuga-Sensor von Ikea passt. Diese Änderung erhöht zwar den Energieverbrauch, dafür sehen die Graphen in Home Assistant deutlich schöner aus – das häufigere Aufladen der LiPo-Zelle ist mir das wert.
- **Änderung 2:** Messwerte werden erst nach einem kurzen Delay gesendet. Im Originalcode werden sofort nach dem Einschalten des Xiao Messwerte an Home Assistant übertragen, wobei für 1–2 Sekunden fälschlicherweise -20 °C gesendet werden. Um diesen Fehler zu vermeiden, der die Daten verunreinigt, habe ich einen kurzen Delay (X Sekunden) nach der Sensorinitialisierung hinzugefügt.
- **Änderung 3:** Im Originalcode werden Temperatur und Luftfeuchtigkeit auf ganze Zahlen gerundet. Diese Rundung wurde entfernt, sodass die Messwerte mit Nachkommastellen übertragen werden.

## Platine

Eine eigenständige PCB wäre etwas overkill, deshalb habe ich eine Lochrasterplatine verwendet.

## Bauteilliste

| Bauteil | Beschreibung | Menge | Link/Quelle |
|---|---|---|---|
| Seeed Studio Xiao nRF52840 | Mikrocontroller-Board (BLE, Bluetooth Low Energy) | 1 | [XIAO kaufen](https://www.seeedstudio.com/Seeed-XIAO-BLE-nRF52840-p-5201.html) |
| SHT40 Breakout Board | Temperatur- und Feuchtigkeitssensor (I2C) | 1 | [SHT40 kaufen](https://de.aliexpress.com/item/1005008842184215.html) |
| LiPo-Zelle | Akku zur Stromversorgung (z. B. 3.7V, 500mAh) | 1 | [Akku kaufen](https://de.aliexpress.com/item/1005008218024646.html) |
