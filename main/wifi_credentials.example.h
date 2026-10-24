// ⚫ AJOUTÉ (24/10/2026) : modèle d'identifiants Wi-Fi du radar.
// Copier ce fichier en main/wifi_credentials.h (ignoré par git) et renseigner le réseau de la pièce surveillée.
// Sans main/wifi_credentials.h, le radar passe en mode PASSIF : il écoute les balises (beacons) du point
// d'accès voisin le plus fort, sans s'y connecter (~10 paquets/s au lieu de ~100 : moins réactif).
// ⚫ 24/10/2026 (mesuré) : en passif, 0 à 8 paquets CSI/s seulement : les balises sont émises en 1 Mb/s (DSSS)
// et l'ESP32 ne fournit le CSI que des trames OFDM. Le mode passif est un secours ; utiliser le mode connecté.
#pragma once

#define RADAR_WIFI_SSID     "MonReseau"
#define RADAR_WIFI_PASSWORD "MonMotDePasse"
