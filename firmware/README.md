<!-- ⚫ AJOUTÉ (24/10/2026) : firmware prêt à flasher, sans avoir à compiler -->
# Firmware précompilé

Image complète (bootloader + table de partitions + application), à flasher à l'adresse `0x0`.

| Fichier | Contenu | Taille | SHA-256 |
|---|---|---|---|
| `ZX2D10GE01R_radar_wifi.bin` | **Radar Wi-Fi** | 1 083 968 o | `bb207490d212e90e0b8207f2984f1b04c581bd8b0dd01aec9f269018702f2f3f` |

- Cible : ESP32-S3 N16R8 (flash 16 Mo, PSRAM octale 8 Mo), ESP-IDF v5.0.9.
- Compilé sans identifiants Wi-Fi (`main/wifi_credentials.h` vide) : renseigner le réseau puis recompiler.

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
