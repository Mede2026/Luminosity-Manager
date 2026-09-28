# Luminosity Manager

Petite application Windows (**68 Ko**, aucune installation) qui ajuste **automatiquement la luminosité de l'écran** selon la lumière de la pièce.

## Utilisation
1. Télécharge `LuminosityManager.exe` et double-clique dessus.
2. Une icône ☀ apparaît près de l'horloge (zone de notification). Clic droit pour le menu.

## D'où vient la mesure de lumière ?
L'app choisit automatiquement la meilleure source :

| Priorité | Source | Quand |
|---|---|---|
| 1 | **Capteur de lumière** (lux) | Si le PC en a un |
| 2 | **Webcam** : une image toutes les 30 s, caméra éteinte le reste du temps | Pas de capteur |
| 3 | **Soleil** : heure + position (Boucherville par défaut) | Pas de webcam, ou webcam désactivée dans le menu |

## Menu
- **Adaptation automatique** : activer / désactiver
- **Plus clair / Plus sombre** : décale la courbe de ±10 % (mémorisé)
- **Utiliser la webcam si pas de capteur**
- **Lancer au démarrage de Windows**
- **Quitter**

## Écrans compatibles
- Écran de portable : via WMI (Windows).
- Écran externe : via DDC/CI (doit être activé dans le menu de l'écran).

## Changer la ville (mode Soleil)
Registre `HKCU\Software\LuminosityManager` : valeurs DWORD `Latitude` et `Longitude` en degrés × 100 (ex. Boucherville : 4559 et -7344).

## Compiler
`./build.sh` (MinGW-w64).
