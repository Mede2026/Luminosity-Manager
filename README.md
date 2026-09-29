# Luminosity Manager

Application Windows légère (**~470 Ko**, installation en un clic, sans droits administrateur) qui ajuste **automatiquement la luminosité de l'écran** selon la lumière de la pièce.

## Utilisation
1. Va dans **Releases** (à droite sur la page GitHub) et télécharge `LuminosityManager.exe`.
2. Double-clique dessus : la fenêtre s'ouvre.
3. Clique sur **Installer** (bandeau de l'accueil, ou page **Mises à jour**) : l'app est copiée dans `%LOCALAPPDATA%\Programs\LuminosityManager`, avec un raccourci dans le **menu Démarrer**. Le fichier téléchargé est ensuite supprimé. Pas besoin d'être administrateur.
4. La croix cache la fenêtre ; l'app continue près de l'horloge (icône ☀, grise quand l'app est désactivée). Clic gauche = rouvrir, clic droit = menu.

**Légère** : la fenêtre utilise WebView2 (le moteur d'Edge, déjà dans Windows 10/11). Il n'existe **que quand la fenêtre est ouverte** : fenêtre cachée = ~3 Mo de mémoire.

**Économe** : mode efficacité de Windows 11, une vérification toutes les 5 s fenêtre fermée (2 s avec un capteur), pause complète quand l'ordi est verrouillé / en veille / écran éteint, et Windows prévient l'app quand la luminosité change (elle ne demande pas sans arrêt).

## D'où vient la mesure de lumière ?
| Priorité | Source | Quand |
|---|---|---|
| 1 | **Capteur de lumière** (lux) | Si le PC en a un |
| 2 | **Webcam** : une photo toutes les X secondes (30 s par défaut) | Pas de capteur |
| 3 | **Soleil** : heure + position de ta ville | Pas de webcam, ou webcam désactivée |

### Comment la webcam mesure la lumière
- L'app mesure **les bords de l'image** (plafond, murs) et **ignore ton visage** au centre (il est éclairé par l'écran). La miniature montre la zone ignorée en pointillés.
- **Jamais la webcam si une autre app l'utilise** (Teams, Discord…) : l'app garde la dernière mesure.
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
| **Accueil** | Jauge, état (actif / pause / profil), miniature webcam couleur, graphique 24 h, Mesurer, **Mode lecture**, Désactiver |
| **Statistiques** | Aujourd'hui / 7 / 30 / 90 jours (gardées 120 jours, jamais effacées par erreur) : temps actif, luminosité et lumière moyennes, écran économisé (Wh estimés), ajustements, photos, journée type, types de lumière, sources, apps… |
| **Luminosité** | Ajout −50 à +50 %, minimum / maximum, ta courbe (« Tu es ici », points appris), démarrage Windows |
| **True Tone** | Couleur de la pièce → blanc de l'écran, activer, intensité |
| **Énergie et jeux** | Économie d'énergie (sur batterie / économiseur Windows / jamais), luminosité en moins, pause pendant les jeux, écrans externes |
| **Webcam** | Photo, clarté, lux estimés, caméra, intervalle, exposition verrouillée, calibrer |
| **Profils d'apps** | Luminosité fixe par app (ex. `LumaFusion.exe` → 100 %), ou **🎮 Jeu** : l'app se désactive |
| **Raccourcis** | Clique puis appuie sur la nouvelle combinaison |
| **Ville** | Automatique (d'après la connexion), recherche ou coordonnées |
| **Sauvegarde** | Exporter / importer **toutes** les données (réglages, profils, apprentissage, historique, statistiques) |
| **Mises à jour** | Vérifier, mettre à jour en un clic (empreinte SHA-256 vérifiée), installer / désinstaller, revenir à la version précédente, aide si Sécurité Windows bloque l'app |

Interrupteur **Activé** toujours visible en bas à gauche. Fenêtre agrandie : le contenu prend **toute la largeur** (graphiques plus grands).

## Comment la luminosité est choisie
1. **Courbe** : lux → luminosité, en échelle logarithmique comme l'œil (nuit ~3 lux, pièce sombre ~30, salon ~200, près d'une fenêtre ~1500, plein jour 8000+), entre ton minimum et ton maximum.
2. **Lissage** : plus clair = **vite** (pour lire tout de suite), plus sombre = **lentement** (une ombre qui passe n'assombrit pas l'écran). Petite zone morte (~12 % de lumière) pour éviter les micro-changements.
3. **Transition douce** : petits pas toutes les 0,15 s au lieu de sauts.
4. **Ajout** (curseur) : partout sur la courbe.

## True Tone (comme sur iPhone)
Le blanc de l'écran s'adapte à la **couleur** de la lumière de la pièce : plus chaud (jaune) sous des lampes, plus froid (bleu) en plein jour.
- Couleur mesurée par : le **capteur de couleur** du PC s'il en a un, sinon la **balance des blancs de la webcam** (la caméra mesure la couleur en Kelvin à chaque photo), sinon **l'heure du jour**.
- L'écran suit la pièce **sans la copier** (entre 4700 K et 7200 K), **très doucement** (~30 s).
- Réglable : activer / désactiver, **intensité** (page True Tone).
- Technique : table de couleurs de l'écran (gamma ramp), comme f.lux. Les couleurs normales reviennent quand l'app est désactivée ou fermée.

## Jeux, pauses et économie d'énergie
- **Jeux** : en plein écran (jeux, vidéos) ou pour une app marquée « 🎮 Jeu », l'app se désactive et les couleurs redeviennent normales (réglable).
- **Pause** quand l'ordi est verrouillé, en veille ou écran éteint : aucune photo webcam.
- **Au réveil** (sortie de veille, déverrouillage, écran rallumé, fin d'un jeu) : nouvelle mesure **tout de suite**, appliquée sans transition. Si la webcam n'est pas encore prête, l'app réessaie toutes les 2 s.
- **Économie d'énergie** (sur batterie, ou seulement avec l'économiseur de Windows) : écran plus sombre (−10 % réglable, −5 % de plus avec l'économiseur), 4× moins de photos webcam, transitions plus simples.
- **Mode lecture** (bouton, menu ou Ctrl+Alt+L) : écran 30 % plus sombre et chaud (3800 K).

## Il apprend de toi
Si tu changes la luminosité toi-même (touches Fn, Windows ou raccourcis), l'app **le retient pour ce niveau de lumière seulement** : régler dans le noir ne change pas le plein jour. La page Luminosité montre ta courbe, les points appris, et un bouton « Oublier ». Si un profil d'app est actif, c'est le profil qui est mis à jour.

## Raccourcis clavier (modifiables dans la page « Raccourcis »)
| Raccourci | Action |
|---|---|
| Ctrl + Alt + ↑ | Plus clair (+5 %) |
| Ctrl + Alt + ↓ | Plus sombre (−5 %) |
| Ctrl + Alt + M | Mesurer maintenant |
| Ctrl + Alt + A | Activer / désactiver l'app |
| Ctrl + Alt + L | Mode lecture |

## Mises à jour
L'app vérifie une fois par jour s'il y a une nouvelle Release. Le bouton « Mettre à jour » :
1. télécharge la nouvelle version et **vérifie son empreinte SHA-256** (publiée avec la Release) ;
2. garde une **copie de secours** de ta version (`%LOCALAPPDATA%\LuminosityManager\backup`) ;
3. attend 3 s et vérifie que **Sécurité Windows** n'a pas effacé le nouveau fichier ; sinon, ta version est gardée ;
4. met la nouvelle version en place et la lance. Si elle n'ouvre pas sa fenêtre dans les **20 s**, l'ancienne version est **remise toute seule**.

Bouton **« Revenir à la version précédente »** (page Mises à jour) : remet la copie de secours.

## Désinstaller
Page **Mises à jour → Désinstaller**, ou **Paramètres Windows → Applications → Luminosity Manager → Désinstaller**. Tu choisis de garder ou d'effacer tes réglages et statistiques.

## Si Sécurité Windows bloque l'app
L'app n'est pas signée (un certificat coûte cher). Microsoft Defender la prend parfois pour un virus (par exemple `Trojan:Win32/Bearfoos.A!ml`). Le `!ml` veut dire que c'est une **devinette automatique**, pas un vrai virus connu : c'est un **faux positif**.
- **Récupérer l'app** : Sécurité Windows → Protection contre les virus et menaces → **Historique de protection** → clique sur le blocage → **Actions → Restaurer**. Ou retélécharge-la dans les Releases.
- **Signaler l'erreur** : [microsoft.com/wdsi/filesubmission](https://www.microsoft.com/wdsi/filesubmission) → « Fichier incorrectement détecté ».

## Écrans compatibles
- Écran de portable : via WMI (Windows).
- Écran externe : via DDC/CI (doit être activé dans le menu de l'écran).

## Pour les développeurs
- Moteur en C++ : `src/` (`main.cpp` boucle et réglages, `light.cpp` capteur/webcam/soleil, `brightness.cpp` écrans, `net.cpp` internet, `webview.cpp` WebView2, `ui.cpp` fenêtre + icône + messages).
- Interface : `src/ui/` (`index.html`, `style.css`, `app.js`). Ouvre `index.html` dans un navigateur pour voir le design avec des données de démo.
- C++ ↔ page : messages JSON (`PostWebMessageAsJson` / `chrome.webview.postMessage`).
- `src/webview2/WebView2.h` : généré avec `widl` depuis l'IDL officiel du SDK WebView2 (licence dans ce dossier).
- Calculs purs (courbe, apprentissage, soleil, couleurs…) : `src/core.cpp`, testés par `tests/` (`./tests/run.sh`). GitHub lance les tests à chaque envoi, et avant chaque Release.
- Statistiques : `src/stats.cpp` ; sauvegarde : `src/backup.cpp` ; installation, copie de secours, retour arrière, désinstallation : `src/install.cpp`.
- Compiler : `./build.sh` (MinGW-w64). La version vient du fichier `VERSION`.
- Icônes : `python3 src/make_icon.py`.
- Police : [Inter](https://rsms.me/inter/) (licence SIL OFL 1.1, voir `src/ui/fonts/`), intégrée dans l'app.
- **Publier une version** : changer le numéro dans `VERSION` et pousser → GitHub Actions compile et crée la Release.
