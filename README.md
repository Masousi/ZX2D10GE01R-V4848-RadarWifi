<!-- ⚫ MODIFIÉ (24/10/2026) : titre du projet Radar Wi-Fi (ancien titre : "# ZX2D10GE01R") -->
# ZX2D10GE01R — Radar Wi-Fi
ZX2D10GE01R base on https://github.com/espressif/esp-idf/tree/release/v5.0

<!-- ⚫ MODIFIÉ (24/10/2026) : projet nettoyé, démos supprimées, main minimal -->
<!-- ⚫ MODIFIÉ (24/10/2026) : la base minimale (écran LVGL vide) accueille désormais le radar Wi-Fi -->
> **Origine :** fork de [wireless-tag-com/ZX2D10GE01R-V4848](https://github.com/wireless-tag-com/ZX2D10GE01R-V4848).
> Base : `main/main.c` initialise tous les périphériques de la carte (écran, encodeur, LED, moteur) ;
> l'application est un **radar de présence Wi-Fi CSI** avec écran radar et silhouettes humaines.

**Démarrage rapide :** flasher `firmware/ZX2D10GE01R_radar_wifi.bin` (cf. [firmware/README.md](firmware/README.md)),
ou renseigner le Wi-Fi dans `main/wifi_credentials.h` puis `idf.py -p COM4 build flash monitor`.

<!-- ⚫ AJOUTÉ (24/10/2026) : firmware Radar Wi-Fi -->
## Radar Wi-Fi (CSI)

Le firmware transforme la carte en **radar de présence Wi-Fi** : il analyse le *Channel State Information*
(amplitude des 51 sous-porteuses OFDM utiles de chaque trame reçue) et détecte les perturbations causées
par un corps humain. Aucune caméra, aucun micro. Inspiré de
[ESPectre](https://github.com/francescopace/espectre) (variance glissante), WaveSight (auto-calibrage, sorties LED)
et de l'exemple Espressif [esp-csi](https://github.com/espressif/esp-csi) (ping de la passerelle).

### Écran

| État | Écran | Anneau de LED |
|---|---|---|
| CONNEXION | message Wi-Fi | LED ambre qui tourne |
| CALIBRAGE | « Quittez la pièce N s » puis « Mesure du bruit N % » | remplissage ambre |
| PIÈCE VIDE | radar seul, balayage vert | comète verte |
| PRÉSENCE | **silhouette humaine** bleue, debout, qui respire | bleu qui respire |
| MOUVEMENT | **silhouette rouge qui marche** + 3 échos (silhouettes plus sombres aux positions précédentes) | rouge, nb de LED ∝ intensité |
| PAS DE SIGNAL | plus de paquet CSI depuis 3 s | magenta clignotant |

Les silhouettes s'illuminent au passage du balayage. En bas : courbe du score (ligne sombre = seuil), RSSI,
paquets CSI/s, sensibilité, réseau et canal.

> ⚠️ **La position de la silhouette est indicative.** Une seule carte ESP32 (un seul lien, une antenne) détecte
> *qu'il y a* présence / mouvement et *son intensité*, pas *où*. L'angle affiché est le barycentre des bandes de
> sous-porteuses les plus perturbées (signature spectrale) et la distance au centre l'intensité.
> Localiser ou compter des personnes demande plusieurs nœuds et un modèle entraîné (cf. RuView).

**Encodeur :** rotation = sensibilité 1..10 (10 = plus sensible) ; appui = recalibrage (quitter la pièce).

### Mise en service

1. **Wi-Fi** : copier `main/wifi_credentials.example.h` en `main/wifi_credentials.h` (ignoré par git, créé vide
   au premier build) et renseigner le réseau **de la pièce surveillée**.
   - Renseigné → **mode connecté** : connexion + ping de la passerelle à 100 Hz → ~100 échantillons CSI/s.
   - SSID vide → **mode passif** (secours) : écoute du point d'accès le plus fort sans s'y connecter.
     Mesuré : **0 à 8 paquets CSI/s** seulement (les balises sont en 1 Mb/s DSSS, sans CSI ; seules les trames
     de données OFDM du point d'accès comptent) → détection lente et peu fiable.
2. `idf.py -p COM4 build flash monitor`.
   <!-- ⚫ AJOUTÉ (24/10/2026) -->
   Le firmware précompilé `firmware/ZX2D10GE01R_radar_wifi.bin` est construit **sans identifiants** (mode passif) :
   pour le mode connecté il faut recompiler avec `main/wifi_credentials.h` renseigné.
3. Au démarrage : **quitter la pièce** pendant le calibrage (10 s d'attente + 10 s de mesure).
4. Placer la carte et le point d'accès de part et d'autre de la zone surveillée (1 à 5 m), à hauteur de buste.
   La zone la plus sensible est le trajet direct entre les deux.

Réglages (`idf.py menuconfig` → *Radar Wi-Fi*) : fréquence du ping, durées de calibrage, sensibilité par défaut,
maintien de la présence (20 s), luminosité des LED.

### Algorithme (`main/radar_dsp.c`)

1. amplitudes des sous-porteuses LLTF (indices 2..26 et 38..63) divisées par leur moyenne : supprime le gain
   automatique (AGC) et ne garde que la *forme* du spectre ;
2. variance temporelle de chaque sous-porteuse sur 40 paquets, moyennée → `metric` ;
3. calibrage pièce vide : `seuil = p95(bruit) × (1,2 + 0,25 × (10 − sensibilité))` ;
4. `score = metric / seuil` : MOUVEMENT si ≥ 1 (sortie sous 0,7 pendant 0,5 s), PRÉSENCE tant qu'un mouvement
   date de moins de 20 s ou que le score reste ≥ 0,45 (micro-mouvements), sinon VIDE.

Journal série (1/s) : `RADAR_DSP: MOUVEMENT metric=0.912 seuil=0.240 score=3.80 pps=98 rssi=-48 angle=127`.

<!-- ⚫ AJOUTÉ (24/10/2026) : journal réellement relevé sur la carte (mode passif, sans identifiants) -->
Démarrage relevé sur la carte (mode passif) :

```
RADAR_WIFI: pas d'identifiants (main/wifi_credentials.h) : mode passif
MAIN: Peripheriques initialises
RADAR_WIFI: mode passif : <SSID> canal 11 (-34 dBm) e4:9e:12:..:..:..
RADAR_DSP: CALIB metric=0.000 seuil=0.000 score=0.00 pps=3 rssi=-41 angle=0
RADAR_DSP: calibrage : quittez la pièce (10 s), mesure 10 s
```

En mode connecté, `pps` doit approcher `CONFIG_RADAR_PING_HZ` (100) ; à moins de ~20 la détection est lente.

### Limites

- Sensible aux interférences et à tout ce qui bouge (ventilateur, rideau, animal) : recalibrer si la pièce change.
- La respiration d'une personne immobile n'est pas détectée de façon fiable : la présence repose sur les
  micro-mouvements et le délai de maintien.
- Ce n'est pas un radar à micro-ondes (type HLK-LD2410) : précision et portée inférieures.
- <!-- ⚫ AJOUTÉ (24/10/2026) --> Wi-Fi 2,4 GHz uniquement (ESP32-S3) : la box doit diffuser en 2,4 GHz.
- <!-- ⚫ AJOUTÉ (24/10/2026) --> Fonctionnement validé sur la carte : démarrage, scan, réception CSI, calibrage.
  La qualité de détection en mode connecté reste à régler pièce par pièce (sensibilité à l'encodeur).

| Fichier | Rôle |
|---|---|
| `main/radar.h` | types et API partagés |
| `main/radar_wifi.c` | Wi-Fi (connecté + ping / passif), callback CSI filtré sur le BSSID |
| `main/radar_dsp.c` | variance glissante, calibrage, machine d'états |
| `main/radar_ui.c` | écran radar, silhouettes humaines animées, courbe, encodeur |
| `main/radar_led.c` | anneau de LED |
| `main/font_fr_20.c`, `font_fr_28.c` | polices Montserrat avec accents (lv_font_conv) |

## Matériel / environnement

- ESP32-S3 N16R8 : flash 16 Mo, PSRAM **octale** 8 Mo, écran RGB 480×480 (frame buffer en PSRAM, `fb_in_psram`).
- Encodeur : rotation sur GPIO 5/6 (MT8901), bouton-poussoir sur GPIO 3 (actif bas), anneau de **13 LED WS2812** sur GPIO 4.
- Moteur sur GPIO 7, broches libres du connecteur : GPIO 1, 2, 20, 19 (`EXT_PIN_0..3`).
- ESP-IDF **v5.0.9**.

## Ce qu'initialise `main.c`

| Périphérique | Initialisation | État au démarrage |
|---|---|---|
| Écran RGB 480×480 + rétroéclairage | `screen_init()` (`main/screen.c`) : config du contrôleur en SPI 9 bits, panneau RGB double buffer, LVGL (`full_refresh`) | écran LVGL vide, rétroéclairage allumé |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> NVS | `nvs_flash_init()` (effacée si incompatible) | requise par le Wi-Fi (calibrage RF) |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> Radar | `radar_dsp_start()`, `radar_wifi_start()` (cœur 0), `radar_led_start()`, `radar_ui_init()` (tâche LVGL, cœur 1) | écran radar, calibrage automatique |
| Encodeur + bouton | `mt8901_init()` / `button_attch()` déclarés comme encodeur LVGL, groupe LVGL par défaut | les widgets créés sont pilotables à l'encodeur |
| Anneau de LED | `ws2812_init()` (RMT) | éteint (puis piloté par `radar_led.c`) |
| Moteur | GPIO en sortie | à l'arrêt (niveau 0) |
| Tick LVGL | `esp_timer` périodique | — |

Les broches `EXT_PIN_0..3` ne sont pas configurées.

## Compiler, flasher

```sh
idf.py -p COM4 build flash monitor
```

Dans VS Code, sans terminal : `Ctrl+Shift+P` → *Tasks: Run Task* → **ESP: Build + Flash + Monitor**
(tâches dans `.vscode/tasks.json`, fichier local non versionné). Quitter le moniteur : `Ctrl+]`.

Journal série attendu :

```
esp_psram: Found 8MB PSRAM device
cpu_start: cpu freq: 240000000 Hz
MAIN: Peripheriques initialises
RADAR_DSP: ...            (une ligne par seconde, cf. « Radar Wi-Fi »)
```

### Dépannage

| Symptôme | Cause probable | Solution |
|---|---|---|
| Écran noir + `no mem for frame buffer` + redémarrages | PSRAM désactivée (`sdkconfig` régénéré) | supprimer `sdkconfig`, puis `idf.py build` (reprend `sdkconfig.defaults`) |
| `SPI Flash Size : 2MB` dans le journal | flash mal configurée | *Serial flasher config → Flash size* = 16 MB |
| Erreur de build « app partition is too small » | table de partitions par défaut | *Partition Table* → `partitions_lvgl.csv` |
| Écran noir, aucune erreur | tas LVGL fixe saturé (`LV_MEM_CUSTOM` désactivé) | activer `LV_MEM_CUSTOM` (*LVGL → Memory settings*) |
| L'encodeur ne fait rien | bouton déclaré actif haut | `button_attch(BTN_PIN, 0, 10)` dans `main/screen.c` (déjà corrigé) |
| Écran noir dès la première animation | `full_refresh` désactivé alors que les buffers LVGL sont les frame buffers du panneau | `disp_drv.full_refresh = 1` dans `main/screen.c` (déjà corrigé) |
| `Rendering this widget needs LV_COLOR_SCREEN_TRANSP 1` | widgets semi-transparents | `LV_COLOR_SCREEN_TRANSP` (activé dans `sdkconfig.defaults`) |
| La carte redémarre quand les LED s'allument | 13 LED à 100 % ≈ 0,8 A : alimentation USB insuffisante | port/chargeur USB plus puissant, ou luminosité plus basse |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `stack overflow in task radar_scan` + redémarrages | pile de la tâche de scan trop petite | corrigé (liste des AP en `static`, pile 6 Ko) |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `pps` entre 0 et 8, reste en CALIBRAGE longtemps | mode passif (pas d'identifiants) | renseigner `main/wifi_credentials.h` |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> Reste sur « Connexion Wi-Fi... » | SSID / mot de passe faux, ou réseau 5 GHz seulement | vérifier `main/wifi_credentials.h` (raison de déconnexion dans le journal) |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> MOUVEMENT permanent | seuil trop bas ou calibrage avec quelqu'un dans la pièce | tourner l'encodeur (sensibilité −), ou appui = recalibrer pièce vide |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `Could not open COM4, the port is busy` | moniteur série ou autre flash en cours | fermer le moniteur / attendre la fin de l'autre flash |

## Compiler depuis VS Code (extension ESP-IDF)

- Version d'ESP-IDF : **v5.0.9** (*ESP-IDF: Select Current ESP-IDF Version*), cible **esp32s3**
  (*ESP-IDF: Set Espressif Device Target*). ⚠️ Changer la cible régénère `sdkconfig` : `sdkconfig.defaults`
  remet les valeurs indispensables.
- Port **COM4** (CH343), méthode de flash **UART**.
- Dans un espace de travail multi-dossiers : *ESP-IDF: Pick a Workspace Folder* → ce projet, avant Build/Flash.
- **Un seul build à la fois** : deux builds simultanés (extension + terminal) sur le même dossier `build/`
  provoquent des erreurs `ninja: failed recompaction: Permission denied` ou des `.obj` introuvables.
- Le moniteur série occupe COM4 : le fermer (`Ctrl+]`) avant un flash lancé ailleurs.

## Organisation du code

| Fichier | Rôle |
|---|---|
| `main/main.c` | initialisation des périphériques, tâche LVGL, démarrage du radar |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `main/radar*.c`, `radar.h` | radar Wi-Fi (cf. tableau de la section « Radar Wi-Fi ») |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `main/Kconfig.projbuild` | menu *Radar Wi-Fi* de menuconfig |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `main/wifi_credentials.example.h` | modèle d'identifiants (copie locale `wifi_credentials.h` non versionnée) |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> `firmware/` | image complète prête à flasher à `0x0` |
| `main/screen.c` | panneau RGB 480×480 (`full_refresh`), encodeur LVGL (rotation + bouton) |
| `main/board.h` | brochage de la carte |
| `components/mt8901` | lecture de la rotation (compteur d'impulsions PCNT, pilote `driver/pulse_cnt.h`) |
| `components/button` | bouton-poussoir (anti-rebond) |
| `components/ws2812` | pilote des LED WS2812 via le périphérique RMT |
| `components/lvgl` | LVGL 8.3.3 |
| `sdkconfig.defaults` | valeurs indispensables réappliquées si `sdkconfig` est régénéré |

## ⚠️ Configuration indispensable (`sdkconfig.defaults`)

L'écran a besoin de la **PSRAM** pour son frame buffer. Sans elle, le firmware plante au démarrage
(`lcd_panel.rgb: no mem for frame buffer` puis Guru Meditation) : **écran noir**.

`idf.py set-target` (ou *ESP-IDF: Set Espressif Device Target* dans VS Code) régénère `sdkconfig`.
Les valeurs indispensables (PSRAM octale 80 MHz, flash 16 Mo, `partitions_lvgl.csv`, CPU 240 MHz)
sont donc reprises dans `sdkconfig.defaults` et réappliquées automatiquement.

<!-- ⚫ AJOUTÉ (24/10/2026) -->
Radar : `CONFIG_ESP32_WIFI_CSI_ENABLED=y` (sans lui, aucun paquet CSI : l'écran reste sur « CONNEXION »).
