# Luminosity Manager

Petite application Windows (**~75 Ko**, aucune installation) qui ajuste **automatiquement la luminosité de l'écran** selon la lumière de la pièce.

## Utilisation
1. Va dans **Releases** (à droite sur la page GitHub) et télécharge `LuminosityManager.exe`.
2. Double-clique dessus : la fenêtre de réglages s'ouvre.
3. La croix cache la fenêtre ; l'app continue près de l'horloge (icône ☀). Clic gauche = rouvrir, clic droit = menu.

## D'où vient la mesure de lumière ?
L'app choisit automatiquement la meilleure source :

| Priorité | Source | Quand |
|---|---|---|
| 1 | **Capteur de lumière** (lux) | Si le PC en a un |
| 2 | **Webcam** : une image toutes les X secondes (réglable, 30 s par défaut), caméra éteinte le reste du temps | Pas de capteur |
| 3 | **Soleil** : heure + position (Boucherville par défaut) | Pas de webcam, ou webcam désactivée dans le menu |

## Fenêtre
- **État** : source, mesure, luminosité détectée → appliquée
- **Mesurer maintenant et ajuster** : nouvelle mesure tout de suite, ajustement immédiat
- **Adaptation automatique** : activer / désactiver
- **Ajout à la luminosité détectée** : de −50 % à +50 % (ex. +10 % : détecté 60 % → écran 70 %)
- **Photo webcam toutes les X secondes** : de 5 s à 1 h
- **Lancer au démarrage de Windows** (démarre caché près de l'horloge)

## Publier une version
Changer le numéro dans le fichier `VERSION` (ex. `0.2`) et pousser : GitHub Actions compile l'app et crée la Release `v0.2`.

## Écrans compatibles
- Écran de portable : via WMI (Windows).
- Écran externe : via DDC/CI (doit être activé dans le menu de l'écran).

## Changer la ville (mode Soleil)
Registre `HKCU\Software\LuminosityManager` : valeurs DWORD `Latitude` et `Longitude` en degrés × 100 (ex. Boucherville : 4559 et -7344).

## Compiler
`./build.sh` (MinGW-w64).
