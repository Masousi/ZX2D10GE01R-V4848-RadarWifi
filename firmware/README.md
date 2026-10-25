<!-- ⚫ AJOUTÉ (24/10/2026) : firmware prêt à flasher, sans avoir à compiler -->
# Firmware précompilé

Image complète (bootloader + table de partitions + application), à flasher à l'adresse `0x0`.

| Fichier | Contenu | Taille | SHA-256 |
|---|---|---|---|
<!-- | `ZX2D10GE01R_radar_wifi.bin` | **Radar Wi-Fi** | 1 083 968 o | `bb207490d212e90e0b8207f2984f1b04c581bd8b0dd01aec9f269018702f2f3f` | -->
<!-- ⚫ MODIFIÉ (25/10/2026) : image régénérée (vue 3D, portée 8 m, distance estimée) -->
<!-- | `ZX2D10GE01R_radar_wifi.bin` | **Radar Wi-Fi** (vue 3D + 2D) | 1 092 560 o | `ecc611fdc51dc47ba418b1c87ac14baf41735f1919a9e43479309cdbf930a883` | -->
<!-- ⚫ MODIFIÉ (25/10/2026) : image régénérée (configuration du Wi-Fi par QR code) -->
| `ZX2D10GE01R_radar_wifi.bin` | **Radar Wi-Fi** (vue 3D + 2D, Wi-Fi par QR code) | 1 211 840 o | `890be2e5b0a4878576bf870c5d9342038ba0c6335d7bb2de262a3bcd202fd35d` |

- Cible : ESP32-S3 N16R8 (flash 16 Mo, PSRAM octale 8 Mo), ESP-IDF v5.0.9.
<!-- - Compilé sans identifiants Wi-Fi (`main/wifi_credentials.h` vide) : renseigner le réseau puis recompiler. -->
<!-- ⚫ MODIFIÉ (25/10/2026) -->
- Compilé sans identifiants Wi-Fi : au premier démarrage le kit affiche un **QR code** ; le scanner, choisir la box
  sur la page qui s'ouvre et saisir le mot de passe (cf. README principal, § 3). Aucune compilation nécessaire.

## Flasher

```sh
esptool.py --chip esp32s3 -p COM4 -b 460800 write_flash 0x0 ZX2D10GE01R_radar_wifi.bin
```

Remplacer `COM4` par le port de la carte.
Ou avec l'outil web : https://espressif.github.io/esptool-js/ (adresse `0x0`).

## Régénérer

1. `idf.py build`
2. Dans `build/` :
   ```sh
   esptool.py --chip esp32s3 merge_bin -o ../firmware/ZX2D10GE01R_radar_wifi.bin @flash_args
   ```
