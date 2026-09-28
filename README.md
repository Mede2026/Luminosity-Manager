# Luminosity Manager

Application Windows légère (**~290 Ko**, aucune installation) qui ajuste **automatiquement la luminosité de l'écran** selon la lumière de la pièce.

## Utilisation
1. Va dans **Releases** (à droite sur la page GitHub) et télécharge `LuminosityManager.exe`.
2. Double-clique dessus : la fenêtre s'ouvre.
3. La croix cache la fenêtre ; l'app continue près de l'horloge (icône ☀, grise quand l'app est désactivée). Clic gauche = rouvrir, clic droit = menu.

**Légère** : la fenêtre utilise WebView2 (le moteur d'Edge, déjà dans Windows 10/11). Il n'existe **que quand la fenêtre est ouverte** : fenêtre cachée = ~3 Mo de mémoire.

## D'où vient la mesure de lumière ?
| Priorité | Source | Quand |
|---|---|---|
| 1 | **Capteur de lumière** (lux) | Si le PC en a un |
| 2 | **Webcam** : une photo toutes les X secondes (30 s par défaut) | Pas de capteur |
| 3 | **Soleil** : heure + position de ta ville | Pas de webcam, ou webcam désactivée |

### Comment la webcam mesure la lumière
- L'app **évite la caméra infrarouge** (Windows Hello), dont l'image est presque noire. Tu peux aussi choisir la caméra dans la page **Webcam**.
- L'app **fixe elle-même l'exposition** (le temps pendant lequel la caméra capte la lumière) et l'ajuste d'un cran si l'image est trop noire ou trop blanche. Comme elle connaît l'exposition, elle calcule la vraie lumière (en lux, estimée).
- **Exposition verrouillée** (page Webcam) : toujours la même exposition, choisie avec un curseur.
- Mesure fiable : l'app revient à la lumière réelle (sans le « gamma » de l'image), ignore les lampes / fenêtres dans l'image (5 % les plus clairs et les plus sombres) et fait la moyenne de 3 images.
- Après la photo, la caméra est remise en automatique pour les autres apps (Teams, Caméra…).
- **Calibrer** : dans une pièce éclairée normalement, clique sur « Calibrer » : cette lumière devient la référence.
- L'accueil montre une **miniature de la dernière photo** pour vérifier ce que voit la caméra.

## Fenêtre (style Windows 11, clair ou sombre selon Windows)
| Page | Contenu |
|---|---|
| **Accueil** | Jauge de luminosité, état (actif / désactivé / profil), miniature webcam, graphique 24 h (survole pour voir l'heure) |
| **Luminosité** | Ajout −50 à +50 %, minimum / maximum, aperçu de ta courbe, démarrage Windows |
| **Webcam** | Photo, clarté, lux estimés, choix de la caméra, intervalle, calibrer |
| **Profils d'apps** | Luminosité fixe par application (ex. `LumaFusion.exe` → 100 %) |
| **Raccourcis** | Clique puis appuie sur la nouvelle combinaison |
| **Ville** | Recherche de ville ou coordonnées (mode Soleil) |
| **Mises à jour** | Vérifier, mettre à jour en un clic |

Interrupteur **Activé** toujours visible en bas à gauche.

## Comment la luminosité est choisie
1. **Courbe** : lux → luminosité, en échelle logarithmique comme l'œil (nuit ~3 lux, pièce sombre ~30, salon ~200, près d'une fenêtre ~1500, plein jour 8000+), entre ton minimum et ton maximum.
2. **Lissage** : plus clair = **vite** (pour lire tout de suite), plus sombre = **lentement** (une ombre qui passe n'assombrit pas l'écran). Petite zone morte (~12 % de lumière) pour éviter les micro-changements.
3. **Transition douce** : petits pas toutes les 0,15 s au lieu de sauts.
4. **Ajout** (curseur) : partout sur la courbe.

## Il apprend de toi
Si tu changes la luminosité toi-même (touches Fn, Windows ou raccourcis), l'app **le retient pour ce niveau de lumière seulement** : régler dans le noir ne change pas le plein jour. La page Luminosité montre ta courbe, les points appris, et un bouton « Oublier ». Si un profil d'app est actif, c'est le profil qui est mis à jour.

## Raccourcis clavier (modifiables dans la page « Raccourcis »)
| Raccourci | Action |
|---|---|
| Ctrl + Alt + ↑ | Plus clair (+5 %) |
| Ctrl + Alt + ↓ | Plus sombre (−5 %) |
| Ctrl + Alt + M | Mesurer maintenant |
| Ctrl + Alt + A | Activer / désactiver l'app |

## Mises à jour
L'app vérifie une fois par jour s'il y a une nouvelle Release. Le bouton « Mettre à jour » télécharge la nouvelle version, la met à la place de l'ancienne et la relance.

## Écrans compatibles
- Écran de portable : via WMI (Windows).
- Écran externe : via DDC/CI (doit être activé dans le menu de l'écran).

## Pour les développeurs
- Moteur en C++ : `src/` (`main.cpp` boucle et réglages, `light.cpp` capteur/webcam/soleil, `brightness.cpp` écrans, `net.cpp` internet, `webview.cpp` WebView2, `ui.cpp` fenêtre + icône + messages).
- Interface : `src/ui/` (`index.html`, `style.css`, `app.js`). Ouvre `index.html` dans un navigateur pour voir le design avec des données de démo.
- C++ ↔ page : messages JSON (`PostWebMessageAsJson` / `chrome.webview.postMessage`).
- `src/webview2/WebView2.h` : généré avec `widl` depuis l'IDL officiel du SDK WebView2 (licence dans ce dossier).
- Compiler : `./build.sh` (MinGW-w64). La version vient du fichier `VERSION`.
- Icônes : `python3 src/make_icon.py`.
- **Publier une version** : changer le numéro dans `VERSION` et pousser → GitHub Actions compile et crée la Release.
