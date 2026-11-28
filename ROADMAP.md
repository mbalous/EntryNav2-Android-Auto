# Feuille de route

Un point n'est coché qu'avec une preuve en voiture : un test sur PC ne suffit pas.

## 0.10 : première version publique

- [x] Contrôle de compatibilité sans rien modifier (`aa-supervisor --compat`)
- [x] Procédure d'installation manuelle, répétée avec la BusyBox de la voiture
- [ ] Procédure suivie de bout en bout sur la voiture de référence
- [x] Essai de la 0.10.0 en profil maintenance sur la voiture de référence (10 novembre 2026)
  - [ ] Le contrôle répond « Compatible », démarrage à blanc compris
  - [x] Android Auto venu tout seul (démarrage automatique, téléphone reconnecté) reste à l'écran, molette sans effet
    sur l'iDrive
  - [ ] Media, Android Auto, Ouvrir : l'iDrive reste sur sa page vide, y compris après un mode de conduite puis RETOUR
  - [ ] Volume : pastille avec le niveau, Android Auto opaque
  - [ ] Wi-Fi coupé : Android Auto se ferme en 5 secondes environ
  - [ ] Arrêter et Démarrer
  - [ ] Molette (un cran par pas), touches, tactile, modes de conduite, caméra, Assistant
  - [ ] GPS de la voiture dans Waze, flèches sur la tête haute
- [ ] Sans journaux (fichier `maintenance` absent) : tout marche pareil

## 0.12 : câble USB

- [x] Android Auto démarre par câble USB sur la voiture de référence, le sans-fil marche toujours (11 novembre 2026)
- [ ] Trajet de 20 à 30 minutes par câble, musique en Bluetooth
- [ ] Câble débranché puis rebranché en cours de route
- [ ] *Arrêter* puis *Démarrer* sans débrancher, puis retour au sans-fil
- [x] Câble essayé sur une autre voiture : X3 G01 ([#17](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/17)),
  i3 ([#19](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/19)), G21
  ([#26](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/26))

## 1.0 : d'autres voitures, mise à jour par clé USB

- [x] Rapport de compatibilité d'une deuxième voiture, sans rien installer : X3 G01 de 2020
  ([#17](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/17))
- [x] Installation sur d'autres voitures : une vingtaine de modèles, Série F, Série G, i3 et MINI, signalés par leurs
  propriétaires ([compatibilité](docs/COMPATIBILITE.md#voitures-essayées))
- [ ] Désinstallation vérifiée sur une autre voiture
- [x] Tableau des logiciels essayés rempli ([compatibilité](docs/COMPATIBILITE.md))
- [ ] Dernière pré-version essayée sur la voiture de référence
- [ ] Mise à jour par clé USB essayée sur la voiture de référence : version plus récente, version antérieure, clé
  sans archive, archive non signée ([installation](docs/INSTALLATION.md#par-clé-usb-sans-pc))
- [x] Ancienne molette (contrôleurs C10/C20) essayée sur une vraie voiture : i3
  ([#19](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/19), 11 novembre 2026)
- [x] Flèches au compteur des Série F par MOST (depuis la 0.13.0-rc.10) essayées sur une voiture : F52 et X1 F49 au
  compteur 6WA ([#79](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/79)), guidage au compteur d'une F20
  ([#101](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/101)), novembre 2026

## Limites connues

| Limite | Cause | Piste |
|---|---|---|
| Le son passe par le Bluetooth de la voiture : qualité limitée, débuts de messages vocaux coupés | pas de son par le Wi-Fi | son par le Wi-Fi |
| Caméra ouverte par son bouton sans l'aide au stationnement : pas encore vérifié qu'Android Auto lui laisse l'écran | l'aide au stationnement est suivie (`sensors.PDCStatus`, depuis la 0.13.0-rc.3), pas la caméra seule | numéro de l'écran de la caméra, relevé dans `app.log` |
| Pendant un appel, la molette agit aussi sur l'écran d'appel de l'iDrive | idem | lire l'écran affiché par l'iDrive |
| Tête haute des Série G : la flèche ne revient pas après l'affichage d'une limitation de vitesse, et reste masquée au-delà de 1,6 km | comportement du compteur | à étudier |
| Le texte de sortie de rond-point du compteur des Série G est en français | texte fixe | suivre la langue de l'iDrive |
| Le bouton vocal du volant reste à l'iDrive | quand il allait à Android Auto, l'iDrive répondait aussi « fonction non disponible » (essai de novembre 2026) | à revoir ([discussion #193](https://github.com/Jiraiya43/EntryNav2-Android-Auto/discussions/193)) |
| Sans-fil lent sur certaines voitures ; par câble, pas de lenteur | les images arrivent déjà en retard du téléphone, la voiture ne les retient pas ([#203](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/203)) : téléphone ou radio, cause exacte pas trouvée | débit vidéo reçu par tranche de 10 s ([#206](https://github.com/Jiraiya43/EntryNav2-Android-Auto/pull/206)) |
| Sur certaines voitures, la radio FM reprend l'écran en roulant (X1 F48) | l'iDrive remonte lui-même sa page radio quand le tuner suit la station | [#138](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/138) |
| Une seule identité de voiture pour tous : un téléphone utilisé dans deux voitures équipées les confond | `vehicleId` fixe | identifiant propre à chaque installation |

## Après 1.0

| Sujet | État |
|---|---|
| Son par le Wi-Fi au lieu du Bluetooth | en pause |
| Écran affiché par l'iDrive lu directement (alertes, caméra, appels) | numéros des écrans relevés dans `app.log` depuis la 0.13.0-rc.3 |
| Boussole envoyée au téléphone (carte orientée à l'arrêt) | idée |
| Touche favori du tableau de bord qui ouvre Android Auto | idée |
| Son Bluetooth du téléphone choisi tout seul à la connexion d'Android Auto | idée (voir [#87](https://github.com/Jiraiya43/EntryNav2-Android-Auto/issues/87)) |
| Pochette de l'album sur l'iDrive | idée |
| Autre structure de la bibliothèque Bluetooth | refusée pour l'instant, il manque le rapport et la bibliothèque |
| Message à l'écran si Android Auto ne démarre pas en production | idée |

## Hors périmètre

- NBT Evo, ID7 et MGU : autres systèmes, ce programme ne peut pas y tourner.
- Marques autres que BMW.
