<!-- ⚫ MODIFIÉ (24/10/2026) : titre du projet Radar Wi-Fi (ancien titre : "# ZX2D10GE01R") -->
<!-- ⚫ MODIFIÉ (25/10/2026) : README réorganisé en guide utilisable par tous (installation, utilisation, dépannage) -->
# ZX2D10GE01R — Radar Wi-Fi

Transforme le bouton à écran rond **ZX2D10GE01R-V4848** (ESP32-S3) en **radar de présence Wi-Fi** : il détecte
les personnes qui bougent dans une pièce en analysant les perturbations du signal Wi-Fi, **sans caméra ni micro**,
et les affiche sous forme de **silhouettes humaines** sur une vue 3D (le kit au centre) ou un radar 2D.

<!-- ⚫ MODIFIÉ (24/10/2026) : projet nettoyé, démos supprimées, main minimal -->
<!-- ⚫ MODIFIÉ (24/10/2026) : la base minimale (écran LVGL vide) accueille désormais le radar Wi-Fi -->
> **Origine :** fork de [wireless-tag-com/ZX2D10GE01R-V4848](https://github.com/wireless-tag-com/ZX2D10GE01R-V4848)
> (ESP-IDF v5.0). Inspiré de [ESPectre](https://github.com/francescopace/espectre) (variance glissante),
> WaveSight (auto-calibrage, sorties LED) et de l'exemple Espressif [esp-csi](https://github.com/espressif/esp-csi).

**Sommaire :** [Matériel](#1-matériel-nécessaire) ·
[Flasher](#2-flasher-le-firmware-sans-rien-installer) ·
[Wi-Fi par QR code](#3-connecter-le-radar-à-votre-wi-fi-qr-code) ·
[Compiler soi-même](#3-bis-compiler-soi-même-facultatif) ·
[Installation dans la pièce](#4-installation-dans-la-pièce) · [Utilisation](#5-utilisation) ·
[Portée et précision](#6-portée-et-précision) · [Réglages](#7-réglages) · [Dépannage](#8-dépannage) ·
[Fonctionnement](#9-comment-ça-marche) · [Code](#10-organisation-du-code)

---

## 1. Matériel nécessaire

| Élément | Détail |
|---|---|
| Kit **ZX2D10GE01R-V4848** | ESP32-S3 N16R8 (flash 16 Mo, PSRAM octale 8 Mo), écran rond 480×480, encodeur, 13 LED |
| Câble USB-C **données** | un câble « charge seule » ne permet pas de flasher |
| Box / point d'accès **2,4 GHz** | celle de la maison ; l'ESP32-S3 ne capte pas le 5 GHz |
| PC Windows, Linux ou macOS | pour flasher (navigateur Chrome/Edge suffit pour l'essai rapide) |

Le port USB apparaît comme `COMx` sous Windows (pilote CH343), `/dev/ttyACM0` ou `/dev/ttyUSB0` sous Linux,
`/dev/cu.usbmodem…` sous macOS. Les exemples ci-dessous utilisent `COM4` : remplacez-le par votre port.

<!-- ⚫ MODIFIÉ (25/10/2026) : le Wi-Fi se configure par QR code : le firmware précompilé suffit -->
## 2. Flasher le firmware (sans rien installer)

<!-- ⚫ AJOUTÉ (24/10/2026) -->
Le firmware précompilé `firmware/ZX2D10GE01R_radar_wifi.bin` est construit **sans identifiants Wi-Fi** :
au premier démarrage il affiche un **QR code** pour lui indiquer votre Wi-Fi (§ 3). Inutile de compiler.

1. Ouvrir https://espressif.github.io/esptool-js/ dans Chrome ou Edge.
2. *Connect* → choisir le port du kit.
3. Adresse **`0x0`**, fichier `firmware/ZX2D10GE01R_radar_wifi.bin` → *Program*.
4. Débrancher / rebrancher le kit.

Ou en ligne de commande (`pip install esptool`) :

```sh
esptool.py --chip esp32s3 -p COM4 -b 460800 write_flash 0x0 firmware/ZX2D10GE01R_radar_wifi.bin
```

Détails et empreinte SHA-256 : [firmware/README.md](firmware/README.md).

<!-- ⚫ AJOUTÉ (25/10/2026) : portail de configuration Wi-Fi + QR code (comme RadarAvion / AnimalDeCompanie) -->
## 3. Connecter le radar à votre Wi-Fi (QR code)

Sans réseau enregistré, le kit crée son propre Wi-Fi **`RadarWifi_Config`** (ouvert) et affiche l'écran
**Configuration Wi-Fi** avec un QR code :

1. Scanner le QR code avec l'appareil photo du téléphone → « Rejoindre le réseau RadarWifi_Config ».
2. La page de configuration s'ouvre toute seule (sinon : ouvrir **http://192.168.4.1** dans le navigateur).
3. Choisir la box **de la pièce à surveiller** (2,4 GHz) dans la liste, saisir le mot de passe,
   *Enregistrer et redémarrer*.
4. Le kit redémarre, se connecte à la box et lance le calibrage (§ 4). Le téléphone peut quitter `RadarWifi_Config`.

Le réseau est enregistré dans la mémoire du kit (NVS) : il survit aux coupures de courant. Le mot de passe
n'est envoyé nulle part.

| Situation | Ce que fait le kit |
|---|---|
| premier démarrage, aucun réseau | écran QR code |
| réseau enregistré mais connexion impossible pendant 30 s (mot de passe faux, box éteinte) | écran QR code avec « Connexion à « … » impossible » ; sans visite pendant 5 min, nouvel essai automatique |
| changer de réseau | **maintenir l'encodeur 4 s** (« Configuration Wi-Fi... ») → écran QR code |
| sur l'écran QR code, **appui court** | réseau enregistré : nouvel essai ; sinon : continuer **sans Wi-Fi** (mode passif, détection lente) |

## 3 bis. Compiler soi-même (facultatif)

### 3.1 Installer ESP-IDF v5.0.9

Suivre le guide officiel : https://docs.espressif.com/projects/esp-idf/en/v5.0.9/esp32s3/get-started/
(Windows : *ESP-IDF Tools Installer* ; Linux / macOS : `git clone -b v5.0.9 --recursive` puis `./install.sh esp32s3`).
Une autre version 5.0.x convient a priori ; les versions 5.1+ n'ont pas été testées.

### 3.2 Récupérer le code et, en option, pré-renseigner le Wi-Fi

```sh
git clone https://github.com/Masousi/ZX2D10GE01R-V4848-RadarWifi.git
cd ZX2D10GE01R-V4848-RadarWifi
```

<!-- ⚫ MODIFIÉ (25/10/2026) : facultatif depuis le portail QR code -->
Facultatif (le QR code du § 3 suffit) : pour qu'un kit flashé se connecte sans passer par le QR code,
copier `main/wifi_credentials.example.h` en `main/wifi_credentials.h` et l'éditer :

```c
#define RADAR_WIFI_SSID     "NomDeVotreBox"
#define RADAR_WIFI_PASSWORD "MotDePasse"
```

Ce fichier est **ignoré par git** : le mot de passe ne part pas sur GitHub. S'il est absent, il est créé vide au
premier build (= écran QR code). Un réseau enregistré par le QR code reste prioritaire sur ce fichier.

### 3.3 Compiler et flasher

Dans un terminal ESP-IDF (*ESP-IDF CMD / PowerShell* sous Windows, ou après `. ./export.sh`) :

```sh
idf.py -p COM4 build flash monitor
```

Le premier build prend 10 à 20 min. Quitter le moniteur : `Ctrl+]`.

**VS Code (extension Espressif)** : *ESP-IDF: Select Current ESP-IDF Version* → v5.0.9, cible **esp32s3**,
méthode de flash **UART**, puis *Build, Flash and Monitor*. ⚠️ Changer la cible régénère `sdkconfig` :
`sdkconfig.defaults` remet automatiquement les valeurs indispensables (PSRAM, flash 16 Mo, partitions, CSI).
Dans un espace de travail multi-dossiers : *ESP-IDF: Pick a Workspace Folder* → ce projet, avant Build/Flash.

Journal série attendu en mode connecté (**exemple** : adresses et valeurs indicatives, varient selon la pièce) :

```
esp_psram: Found 8MB PSRAM device
RADAR_WIFI: mode connecté : NomDeVotreBox (enregistré), ping 100 Hz
MAIN: Peripheriques initialises
RADAR_UI: vue 3D
RADAR_WIFI: connecté à NomDeVotreBox canal 6, IP 192.168.1.42, passerelle 192.168.1.254, AP ..:..
RADAR_DSP: calibrage : quittez la pièce (10 s), mesure 10 s
RADAR_DSP: calibré : bruit p50=0.081 p95=0.124 (200 mesures)
RADAR_DSP: VIDE metric=0.090 seuil=0.273 score=0.33 pps=97 rssi=-48 angle=127 dist=8.0m ap=2.6m
RADAR_UI: vue 3D : 11 images/s
```

<!-- ⚫ MODIFIÉ (25/10/2026) --> Chaque seconde, une ligne `RADAR_DSP` donne l'état, le score, les paquets/s
(`pps`) et les distances estimées (`dist`, `ap`) ; `RADAR_UI` donne les images/s toutes les 10 s.
**`pps` doit approcher 100** en mode connecté ; en dessous de ~20, la détection est lente.

<!-- ⚫ AJOUTÉ (24/10/2026) : journal réellement relevé sur la carte (mode passif, sans identifiants) -->
<!-- ⚫ MODIFIÉ (25/10/2026) : sans identifiants le kit ouvre désormais le portail ; le passif se choisit sur l'écran QR -->
Sans réseau enregistré (écran QR code) :

```
RADAR_PORTAL: Portail de configuration : Aucun réseau Wi-Fi enregistré
RADAR_PORTAL: Point d'accès 'RadarWifi_Config' prêt : http://192.168.4.1
```

Mode passif (appui court sur l'écran QR code), relevé sur la carte :

```
RADAR_WIFI: mode passif demandé
MAIN: Peripheriques initialises
RADAR_WIFI: mode passif : <SSID> canal 11 (-34 dBm) e4:9e:12:..:..:..
RADAR_DSP: CALIB metric=0.000 seuil=0.000 score=0.00 pps=3 rssi=-41 angle=0
RADAR_DSP: calibrage : quittez la pièce (10 s), mesure 10 s
```

## 4. Installation dans la pièce

1. Placer le kit et la box **de part et d'autre de la zone à surveiller**, 1 à 5 m l'un de l'autre,
   idéalement à hauteur de buste. La zone la plus sensible est le **trajet direct entre les deux**.
2. Alimenter le kit. Écran « CONNEXION » puis « CALIBRAGE » (écran QR code si le Wi-Fi n'est pas encore
   configuré : § 3).
3. **Quitter la pièce** pendant le calibrage : 10 s d'attente (« Quittez la pièce N s ») puis 10 s de mesure
   du bruit. Rien ne doit bouger (ventilateur, rideau, animal).
4. Revenir : la silhouette apparaît.

Si le kit est déplacé ou la pièce modifiée : **appui court** sur l'encodeur = nouveau calibrage.

## 5. Utilisation

### Écran

| État | Écran | Anneau de LED |
|---|---|---|
| CONNEXION | message Wi-Fi | LED ambre qui tourne |
| CALIBRAGE | « Quittez la pièce N s » puis « Mesure du bruit N % » | remplissage ambre |
| PIÈCE VIDE | radar seul, balayage vert | comète verte |
| PRÉSENCE | **silhouette humaine** bleue, debout, qui respire | bleu qui respire |
| MOUVEMENT | **silhouette rouge qui marche** + 3 échos (silhouettes plus sombres aux positions précédentes) | rouge, nb de LED ∝ intensité |
| PAS DE SIGNAL | plus de paquet CSI depuis 3 s | magenta clignotant |

Les silhouettes s'illuminent au passage du balayage. En haut : état, RSSI (force du signal), paquets/s,
sensibilité. En bas : courbe du score (ligne sombre = seuil de détection), réseau et canal.

<!-- ⚫ AJOUTÉ (25/10/2026) : vue 3D, portée et distance estimée -->
### Vue 3D (par défaut) et vue 2D

- **Vue 3D** : le **kit au centre** d'un sol en perspective, anneaux de distance tous les quarts de la portée
  (2, 4, 6, 8 m par défaut), onde qui part du kit, balayage au sol. Les silhouettes sont **debout sur le sol**,
  plus petites quand elles sont loin, avec leur ombre et leur **distance estimée** au-dessus (« ~3,5 m »).
  La box est placée à sa distance estimée d'après le RSSI (direction inconnue : dessinée en arrière-gauche).
- **Vue 2D** : radar rond, anneaux gradués en mètres.

### Encodeur

| Geste | Action |
|---|---|
| Rotation | sensibilité 1..10 (10 = plus sensible), affichée 1,5 s |
| Appui court | recalibrage (quitter la pièce) |
| <!-- ⚫ MODIFIÉ (25/10/2026) --> Appui long (1,5 à 4 s, au relâchement) | bascule vue 3D ↔ vue 2D (« Relâchez : vue 2D/3D ») |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> Appui maintenu 4 s | configuration Wi-Fi : écran QR code (§ 3) |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> Appui court sur l'écran QR code | réessayer le réseau enregistré, ou continuer sans Wi-Fi (passif) |

Trop de fausses détections → baisser la sensibilité ; personne détectée trop tard → l'augmenter.

## 6. Portée et précision

Portée **physique** de détection d'une personne (ordre de grandeur, box 2,4 GHz domestique) :

| Situation | Portée typique |
|---|---|
| même pièce, vue directe kit ↔ box | 8 à 10 m autour du trajet kit ↔ box |
| à travers 1 à 2 cloisons | ~5 m |
| murs porteurs / béton, autre étage | 1 à 3 m, peu fiable |

La portée **affichée** (`CONFIG_RADAR_RANGE_M`, 8 m par défaut, 2..20) n'est qu'une échelle : l'augmenter
agrandit les anneaux, pas la portée physique. La zone sensible n'est pas un cercle autour du kit mais une
ellipse autour du **trajet kit ↔ box** (zones de Fresnel).

| Mode | Paquets CSI/s | Détection |
|---|---|---|
| **Connecté** (Wi-Fi renseigné, ping de la box à 100 Hz) | ~100 | réactive (< 1 s) |
| **Passif** (sans identifiants) | **0 à 8 mesurés** : les balises sont émises en 1 Mb/s (DSSS), sans CSI | lente, peu fiable |

> ⚠️ **La position de la silhouette est indicative.** Une seule carte ESP32 (un seul lien, une antenne) détecte
> *qu'il y a* présence / mouvement et *son intensité*, pas *où*.
> - **Distance** (« ~3,5 m ») = **estimation** : un score juste au seuil place la personne au bord de la portée,
>   un score 10× au-dessus la place contre le kit (interpolation logarithmique), mise à jour pendant un mouvement.
>   Une forte perturbation peut venir d'une personne proche *ou* d'un grand mouvement plus loin.
> - **Direction** = barycentre des bandes de sous-porteuses les plus perturbées (signature spectrale).
> - Localiser ou compter des personnes demande plusieurs nœuds et un modèle entraîné (cf. RuView).

### Limites

- Sensible aux interférences et à tout ce qui bouge (ventilateur, rideau, animal) : recalibrer si la pièce change.
- La respiration d'une personne immobile n'est pas détectée de façon fiable : la présence repose sur les
  micro-mouvements et le délai de maintien (20 s).
- Ce n'est pas un radar à micro-ondes (type HLK-LD2410) : précision et portée inférieures.
- <!-- ⚫ AJOUTÉ (24/10/2026) --> Wi-Fi 2,4 GHz uniquement (ESP32-S3) : la box doit diffuser en 2,4 GHz.
- <!-- ⚫ AJOUTÉ (24/10/2026) --> Fonctionnement validé sur la carte : démarrage, scan, réception CSI, calibrage,
  vues 2D/3D (~11 images/s). La qualité de détection en mode connecté se règle pièce par pièce (sensibilité).

## 7. Réglages

`idf.py menuconfig` → **Radar Wi-Fi** (ou *SDK Configuration Editor* dans VS Code) :

| Option | Défaut | Rôle |
|---|---|---|
| `RADAR_PING_HZ` | 100 | fréquence du ping de la box (= paquets CSI/s en mode connecté) |
| `RADAR_CALIB_SECONDS` | 10 | durée de mesure du bruit |
| `RADAR_CALIB_DELAY_SECONDS` | 10 | délai pour quitter la pièce |
| `RADAR_DEFAULT_SENSITIVITY` | 6 | sensibilité au démarrage (1..10) |
| `RADAR_PRESENCE_HOLD_SECONDS` | 20 | maintien de la présence après le dernier mouvement |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> `RADAR_RANGE_M` | 8 | portée affichée (m) |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> `RADAR_VIEW_3D` | oui | vue 3D au démarrage (sinon 2D) |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> `RADAR_AP_SSID` | RadarWifi_Config | nom du Wi-Fi de configuration du kit (QR code) |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> `RADAR_STA_TIMEOUT_SECONDS` | 30 | délai de connexion avant d'afficher le QR code |
| `RADAR_LED_BRIGHTNESS` | 40 | luminosité max de l'anneau (0..255) |

Après modification de `main/Kconfig.projbuild` lui-même : `idf.py reconfigure` (sinon `CONFIG_RADAR_… undeclared`).

## 8. Dépannage

| Symptôme | Cause probable | Solution |
|---|---|---|
| Reste sur « Connexion Wi-Fi... » | <!-- ⚫ AJOUTÉ (24/10/2026) --> SSID / mot de passe faux, ou réseau 5 GHz seulement | <!-- ⚫ MODIFIÉ (25/10/2026) --> au bout de 30 s l'écran QR code s'affiche : ressaisir le réseau (§ 3) ; raison de déconnexion dans le journal |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> Le QR code ne s'ouvre pas sur le téléphone | appareil photo sans lecture de QR Wi-Fi | se connecter à la main au Wi-Fi `RadarWifi_Config`, puis ouvrir http://192.168.4.1 |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> La page ne s'ouvre pas toute seule | le téléphone garde les données mobiles | ouvrir http://192.168.4.1 ; accepter « rester connecté sans Internet » |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> Ma box n'est pas dans la liste | box en 5 GHz seulement, ou réseau masqué | activer le 2,4 GHz de la box ; réseau masqué : champ « nom du réseau » |
| `pps` entre 0 et 8, reste en CALIBRAGE longtemps | <!-- ⚫ AJOUTÉ (24/10/2026) --> mode passif (pas d'identifiants) | <!-- ⚫ MODIFIÉ (25/10/2026) --> maintenir l'encodeur 4 s et configurer le Wi-Fi par QR code |
| MOUVEMENT permanent | <!-- ⚫ AJOUTÉ (24/10/2026) --> seuil trop bas ou calibrage avec quelqu'un dans la pièce | tourner l'encodeur (sensibilité −), ou appui court = recalibrer pièce vide |
| Rien n'est détecté | sensibilité trop basse, kit et box côte à côte | sensibilité +, éloigner kit et box de part et d'autre de la zone |
| Écran noir + `no mem for frame buffer` + redémarrages | PSRAM désactivée (`sdkconfig` régénéré) | supprimer `sdkconfig`, puis `idf.py build` (reprend `sdkconfig.defaults`) |
| `SPI Flash Size : 2MB` dans le journal | flash mal configurée | *Serial flasher config → Flash size* = 16 MB |
| Erreur de build « app partition is too small » | table de partitions par défaut | *Partition Table* → `partitions_lvgl.csv` |
| Écran noir, aucune erreur | tas LVGL fixe saturé (`LV_MEM_CUSTOM` désactivé) | activer `LV_MEM_CUSTOM` (*LVGL → Memory settings*) |
| L'encodeur ne fait rien | bouton déclaré actif haut | `button_attch(BTN_PIN, 0, 10)` dans `main/screen.c` (déjà corrigé) |
| Écran noir dès la première animation | `full_refresh` désactivé alors que les buffers LVGL sont les frame buffers du panneau | `disp_drv.full_refresh = 1` dans `main/screen.c` (déjà corrigé) |
| `Rendering this widget needs LV_COLOR_SCREEN_TRANSP 1` | widgets semi-transparents | `LV_COLOR_SCREEN_TRANSP` (activé dans `sdkconfig.defaults`) |
| La carte redémarre quand les LED s'allument | 13 LED à 100 % ≈ 0,8 A : alimentation USB insuffisante | port/chargeur USB plus puissant, ou luminosité plus basse |
| `stack overflow in task radar_scan` + redémarrages | <!-- ⚫ AJOUTÉ (24/10/2026) --> pile de la tâche de scan trop petite | corrigé (liste des AP en `static`, pile 6 Ko) |
| `Could not open COM4, the port is busy` | <!-- ⚫ AJOUTÉ (24/10/2026) --> moniteur série ou autre flash en cours | fermer le moniteur (`Ctrl+]`) / attendre la fin de l'autre flash |
| `ninja: failed recompaction: Permission denied` | deux builds simultanés sur le même `build/` (extension + terminal) | un seul build à la fois |
| Écran qui « glisse » quand le Wi-Fi est actif | écriture flash pendant l'affichage | le Wi-Fi est déjà en stockage RAM (`WIFI_STORAGE_RAM`) ; signaler le cas |

## 9. Comment ça marche

Le Wi-Fi moderne (OFDM) découpe chaque trame en 64 sous-porteuses. L'ESP32 fournit pour chaque trame reçue le
**CSI** (*Channel State Information*) : amplitude et phase de chaque sous-porteuse. Un corps humain modifie les
trajets multiples des ondes entre la box et le kit : la *forme* du spectre se met à fluctuer.

- `radar_wifi.c` : connexion à la box + **ping de la passerelle à 100 Hz** ; chaque réponse est une trame
  émise par la box → un échantillon CSI. Seules les trames de la box (adresse MAC source = BSSID) sont gardées.

### Algorithme (`main/radar_dsp.c`)

1. amplitudes des sous-porteuses LLTF (indices 2..26 et 38..63) divisées par leur moyenne : supprime le gain
   automatique (AGC) et ne garde que la *forme* du spectre ;
2. variance temporelle de chaque sous-porteuse sur 40 paquets, moyennée → `metric` ;
3. calibrage pièce vide : `seuil = p95(bruit) × (1,2 + 0,25 × (10 − sensibilité))` ;
4. `score = metric / seuil` : MOUVEMENT si ≥ 1 (sortie sous 0,7 pendant 0,5 s), PRÉSENCE tant qu'un mouvement
   date de moins de 20 s ou que le score reste ≥ 0,45 (micro-mouvements), sinon VIDE ;
5. <!-- ⚫ AJOUTÉ (25/10/2026) --> distance estimée `= portée × (1 − log(score) / log(10))`, bornée à 0,4 m ;
   distance de la box d'après le RSSI : `10^((−40 − RSSI) / 27)` m.

Exemple de journal (1/s) : `RADAR_DSP: MOUVEMENT metric=0.912 seuil=0.240 score=3.80 pps=98 rssi=-48 angle=127 dist=4.2m ap=1.3m`.

## 10. Organisation du code

| Fichier | Rôle |
|---|---|
| `main/main.c` | initialisation des périphériques, tâche LVGL, démarrage du radar |
| `main/radar.h` | types et API partagés |
| `main/radar_wifi.c` | Wi-Fi (connecté + ping / passif), callback CSI filtré sur le BSSID |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> `main/wifi_portal.c` | portail de configuration : point d'accès, page web, DNS captif, identifiants en NVS |
| `main/radar_dsp.c` | variance glissante, calibrage, machine d'états, distances estimées |
| `main/radar_ui.c` | écran : vues 3D / 2D, silhouettes humaines animées, courbe, encodeur |
| <!-- ⚫ AJOUTÉ (25/10/2026) --> ↳ vue 3D | projection perspective (`project()`), sol, kit, box, silhouettes mises à l'échelle |
| `main/radar_led.c` | anneau de LED |
| `main/font_fr_20.c`, `font_fr_28.c` | polices Montserrat avec accents (lv_font_conv) |
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

### Ce qu'initialise `main.c`

| Périphérique | Initialisation | État au démarrage |
|---|---|---|
| Écran RGB 480×480 + rétroéclairage | `screen_init()` (`main/screen.c`) : config du contrôleur en SPI 9 bits, panneau RGB double buffer, LVGL (`full_refresh`) | écran radar, rétroéclairage allumé |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> NVS | `nvs_flash_init()` (effacée si incompatible) | requise par le Wi-Fi (calibrage RF) |
| <!-- ⚫ AJOUTÉ (24/10/2026) --> Radar | `radar_dsp_start()`, `radar_wifi_start()` (cœur 0), `radar_led_start()`, `radar_ui_init()` (tâche LVGL, cœur 1) | écran radar, calibrage automatique |
| Encodeur + bouton | `mt8901_init()` / `button_attch()` déclarés comme encodeur LVGL, groupe LVGL par défaut | lus directement par `radar_ui.c` |
| Anneau de LED | `ws2812_init()` (RMT) | éteint (puis piloté par `radar_led.c`) |
| Moteur | GPIO en sortie | à l'arrêt (niveau 0) |
| Tick LVGL | `esp_timer` périodique | — |

### Brochage

- ESP32-S3 N16R8 : flash 16 Mo, PSRAM **octale** 8 Mo, écran RGB 480×480 (frame buffer en PSRAM, `fb_in_psram`).
- Encodeur : rotation sur GPIO 5/6 (MT8901), bouton-poussoir sur GPIO 3 (actif bas), anneau de **13 LED WS2812** sur GPIO 4.
- Moteur sur GPIO 7, broches libres du connecteur : GPIO 1, 2, 20, 19 (`EXT_PIN_0..3`, non configurées).

### ⚠️ Configuration indispensable (`sdkconfig.defaults`)

L'écran a besoin de la **PSRAM** pour son frame buffer. Sans elle, le firmware plante au démarrage
(`lcd_panel.rgb: no mem for frame buffer` puis Guru Meditation) : **écran noir**.

`idf.py set-target` (ou *ESP-IDF: Set Espressif Device Target* dans VS Code) régénère `sdkconfig`.
Les valeurs indispensables (PSRAM octale 80 MHz, flash 16 Mo, `partitions_lvgl.csv`, CPU 240 MHz)
sont donc reprises dans `sdkconfig.defaults` et réappliquées automatiquement.

<!-- ⚫ AJOUTÉ (24/10/2026) -->
Radar : `CONFIG_ESP32_WIFI_CSI_ENABLED=y` (sans lui, aucun paquet CSI : l'écran reste sur « CONNEXION »).

## Licence

Code : domaine public (Unlicense), voir [LICENSE.md](LICENSE.md). LVGL : licence MIT (`components/lvgl/LICENCE.txt`).
