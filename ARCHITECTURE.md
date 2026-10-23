# sqldb pour Zephyr — Architecture

Oct 18, 2026 · @PELLETANT

## Vision et objectifs

sqldb est une base SQL embarquée pour Zephyr RTOS : l'API de SQLite, un moteur conçu pour les MCU, et des garanties ACID qui tiennent aux coupures de courant sur flash comme sur FRAM. Il est livré comme un module Zephyr (west, Kconfig, devicetree).

- **Portage sans douleur** : une application Linux qui utilise SQLite se recompile sur Zephyr avec `#include <sqlite3.h>`, sans réécrire ses requêtes, à l'exception du SQL construit dynamiquement.
- **Garanties maîtrisées** : atomicité, durabilité et isolation prouvées pour chaque type de support par fuzzing de coupure.
- **Tout MCU Zephyr** : du Cortex-M0+ avec FRAM externe au Cortex-M7 avec QSPI NOR, via des profils Kconfig.
- **Schéma évolutif par OTA** : migrations versionnées et sûres face au retour arrière de MCUboot.
- **Pas d'interpréteur sur la cible** : le SQL est compilé sur l'hôte en C au moment du build.
- **Diagnostic confortable** : un shell Zephyr intégré et `sqldb-cli`, un vrai prompt SQLite côté PC branché sur la carte.

Nom de travail : sqldb. « SQLite » est une marque déposée ; seule la couche de compatibilité expose `sqlite3.h`.

## Contraintes et non-objectifs

Les contraintes ci-dessous fixent le design ; chacune a une conséquence directe sur l'architecture.

| Contrainte | Conséquence sur le design |
| --- | --- |
| Tout MCU supporté par Zephyr, de quelques dizaines de Ko à plusieurs Mo | Profils Kconfig `tiny` / `standard` / `full` ; aucune allocation dynamique |
| Supports NOR, NAND, FRAM/MRAM, EEPROM, LittleFS | Pilote de support avec descripteur de capacités et deux stratégies de commit |
| Coupure de courant à tout instant | Commit atomique par superblock A/B ou journal à CRC ; fuzzing de coupure obligatoire |
| Schéma qui évolue par OTA, MCUboot peut revenir en arrière | Catalogue versionné, snapshot épinglé jusqu'à confirmation de l'image |
| Requêtes écrites comme avec SQLite | Compilateur SQL sur l'hôte, couche `sqlite3.h` |
| Temps réel (RTOS) | Opérations bornées, compactage incrémental, lecteurs sur snapshot sans verrou |
| Mémoire statique | Pools de pages, curseurs et espace de tri dimensionnés par Kconfig ; pire cas connu au build |

**Non-objectifs**

- Pas de compatibilité binaire (ABI) avec SQLite, ni du format de fichier SQLite.
- Pas d'interpréteur ni de VM sur la cible ; le SQL inconnu au build n'est pas exécuté sur le MCU (voir Outillage pour les alternatives).
- Pas de DDL à l'exécution : le schéma change uniquement par migration livrée avec le firmware.
- Hors périmètre en version 1 : triggers, vues complexes, fonctions fenêtres, CTE récursives, `ATTACH`, fonctions SQL utilisateur, FTS, R-Tree.
- Pas de multi-écrivain : un seul écrivain à la fois, lecteurs concurrents illimités dans la limite des pools.

## Vue d'ensemble

sqldb se compose d'un compilateur qui tourne sur l'hôte au build, d'une pile de couches dans le firmware, et d'outils de diagnostic branchés sur l'API curseurs.

&#91;embedded content: architecture sqldb · hôte, 7 couches firmware, outils\]

Le compilateur transforme le SQL en C lié au firmware ; sous ce C, chaque couche ne connaît que celle du dessous, et seul le pilote connaît le support physique.

### Intégration Zephyr

- **Module west** : `modules/lib/sqldb`, déclaré par `zephyr/module.yml`, ajouté au manifeste de l'application.
- **Kconfig** : profils, tailles de pools, choix des supports et des options (voir Profils).
- **Devicetree** : binding `zephyr,sqldb` qui associe un nom de base à un support (partition flash, FRAM, chemin LittleFS).
- **CMake** : `zephyr_sqldb_queries(queries.sql SCHEMA schema/)` lance le compilateur hôte et ajoute le C généré à la cible.
- **MCUboot** : `sqldb_migration_confirm()` appelé avec `boot_write_img_confirmed()`.
- **mcumgr** : un groupe SMP dédié pour `sqldb-cli` et l'export.
- **Shell, Ztest, Twister, `native_sim`** : utilisés tels quels pour le diagnostic et les tests.

```
modules/lib/sqldb/
  zephyr/module.yml
  Kconfig
  dts/bindings/zephyr,sqldb.yaml
  include/sqldb/        API curseurs, API typée
  include/sqlite3.h     façade de compatibilité
  src/media/            pilotes NOR, NAND, FRAM, LittleFS, RAM
  src/pager/            pages, commit, ramasse-miettes
  src/btree/            B+tree, encodage, tri externe
  src/compat/           façade sqlite3, table de hachage des requêtes
  src/shell/            commande db
  src/remote/           serveur de curseurs SMP
  tools/sqldbc/         compilateur hôte
  tools/sqldb-cli/      prompt côté PC
  tests/                coupures, différentiel, migrations
```

## Stockage : pilotes et commit

Le moteur ne voit que des pages numérotées ; le pilote de support décrit ce que le média sait faire, et la stratégie de commit en découle.

**Descripteur de capacités** (fourni par chaque pilote, à la manière des `IOCAP` de SQLite) :

```c
struct sqldb_media_caps {
    uint32_t write_granularity;  /* 1 octet (FRAM), 256 o (NOR), 2 Kio (NAND) */
    uint32_t erase_size;         /* 0 si pas d'effacement (FRAM, EEPROM) */
    uint32_t endurance_cycles;   /* 1e5 NOR, 1e14 FRAM */
    uint32_t atomic_write_size;  /* souvent 0 : rien n'est garanti atomique */
    bool     needs_ecc;          /* NAND */
    bool     sequential_in_block;/* NAND : programmation séquentielle */
};
```

| Pilote | API Zephyr | Stratégie de commit | Usage typique |
| --- | --- | --- | --- |
| Flash NOR interne / QSPI | `flash_area` | Shadow paging | Cas général, partition dédiée |
| Flash NAND | `flash_area` + ECC | Shadow paging, écriture séquentielle par bloc, blocs défectueux | Gros volumes |
| FRAM / MRAM / EEPROM | `eeprom` ou pilote SPI/I2C | Mise à jour en place + journal | Endurance, écritures fréquentes |
| Hybride FRAM + NOR | les deux | Journal en FRAM, données en NOR, report par checkpoint | Petites écritures fréquentes sur gros volume |
| LittleFS | `fs_*` | Disposition en log + manifest | Cohabitation avec d'autres fichiers |
| RAM | — | Au choix, avec injection de coupure | Tests sous `native_sim` |

### Shadow paging (flash)

- Une page n'est jamais réécrite : chaque modification écrit une nouvelle version ailleurs, jusqu'à la racine du B+tree.
- Deux superblocks A/B contiennent la racine, un numéro de séquence et un CRC. Le commit = écrire le superblock le plus ancien avec séquence + 1.
- Au montage, on prend le superblock valide de plus grand numéro de séquence. Une coupure laisse soit l'ancien état, soit le nouveau.
- Les pages libérées sont récupérées par un ramasse-miettes incrémental (work item Zephyr, étapes bornées) qui assure aussi le nivellement d'usure.

### Journal en place (FRAM, MRAM, EEPROM)

- Pas de copy-on-write : il gaspillerait une FRAM de 256 Ko à 4 Mo et l'endurance le rend inutile.
- Un journal d'annulation (undo) enregistre l'ancienne image d'une page avant sa modification, puis un enregistrement de commit protégé par CRC clôt la transaction.
- Au montage, une transaction sans commit valide est annulée à partir du journal. Une écriture SPI coupée au milieu est détectée par CRC.
- Pages de 128 à 256 octets : très peu de RAM pour le cache.

### Mode hybride FRAM + NOR

- Le journal (redo), les superblocks et les métadonnées chaudes vivent en FRAM.
- Les pages de données vivent en NOR et sont mises à jour par checkpoints groupés, ce qui réduit fortement l'usure de la flash.

### Pilote LittleFS (optionnel)

- LittleFS réécrit les blocs suivants quand on modifie le milieu d'un fichier : le moteur n'écrit donc qu'en fin de fichiers segments.
- Un petit fichier `manifest` contient la racine et la liste des segments ; un commit = réécriture du manifest + `fs_sync()`, atomique par fichier sous LittleFS.
- Le compactage réécrit les segments fragmentés, met à jour le manifest, puis supprime les anciens.
- FatFS est refusé par défaut : sa sémantique de `fs_sync` ne garantit rien après une coupure.

### Déclaration dans le devicetree

```dts
appdb: sqldb {
    compatible = "zephyr,sqldb";
    path = "app.db";             /* nom utilisé par sqlite3_open() */
    storage = <&fram0>;          /* ou <&storage_partition>, ou "lfs:/data" */
    journal = <&fram0>;          /* mode hybride : journal séparé */
};
```

## Moteur : pages, transactions, B+tree

Le moteur fournit des tables et des index ordonnés, des transactions à un écrivain avec lecteurs sur snapshot, et une API curseurs que le C généré, le shell et `sqldb-cli` utilisent tous.

### Gestion de pages

- Cache de pages dans un pool statique (`CONFIG_SQLDB_PAGE_CACHE_COUNT`), taille de page fixée par base (128 o à 4 Kio selon le support).
- Chaque page porte un en-tête : type, numéro de transaction, CRC32.
- Une page corrompue (CRC faux) est signalée par `SQLITE_CORRUPT`, jamais lue silencieusement.

### Transactions et concurrence

- Un seul écrivain à la fois, protégé par un `k_mutex`. `BEGIN` / `COMMIT` / `ROLLBACK` et `SAVEPOINT` imbriqués.
- Lecteurs sur snapshot : un lecteur garde la racine du moment où il a commencé et ne bloque jamais l'écrivain (gratuit en shadow paging ; en mode journal, les pages modifiées par une transaction en cours sont lues depuis leur ancienne image).
- Le ramasse-miettes ne récupère pas les pages encore visibles par un snapshot ouvert.
- Taille maximale de transaction bornée par l'espace libre (shadow paging) ou la taille du journal ; dépassement = `SQLITE_FULL`.

### B+tree et encodage

- Une table = un B+tree indexé par `rowid` (ou par clé primaire pour les tables `WITHOUT ROWID`).
- Un index = un B+tree dont la clé est (colonnes indexées, clé de la ligne).
- Clés encodées de façon comparable octet à octet (entiers big-endian signés, chaînes avec terminateur), ce qui évite de décoder pendant les recherches.
- Enregistrement auto-décrit : version de schéma + en-tête de types + valeurs. Un firmware plus ancien ignore les colonnes qu'il ne connaît pas (nécessaire au schéma expand/contract).
- Types SQLite : `NULL`, `INTEGER` (varint), `REAL`, `TEXT`, `BLOB`, avec l'affinité de colonne de SQLite.

### API curseurs (interne et publique)

```c
int  sqldb_cur_open(sqldb_txn *t, sqldb_cursor *c, uint16_t tree_id);
int  sqldb_cur_seek(sqldb_cursor *c, const sqldb_key *k, enum sqldb_seek op); /* EQ, GE, LE */
int  sqldb_cur_next(sqldb_cursor *c);
int  sqldb_cur_prev(sqldb_cursor *c);
int  sqldb_cur_column(sqldb_cursor *c, uint16_t col, sqldb_value *out);
int  sqldb_cur_insert(sqldb_cursor *c, const sqldb_record *r);
int  sqldb_cur_delete(sqldb_cursor *c);
```

### Tri et agrégats sans index

- `ORDER BY` / `GROUP BY` / `DISTINCT` sans index adapté passent par un tri externe : runs triés en RAM, écrits dans une zone temporaire du support, puis fusion.
- RAM et zone temporaire bornées par Kconfig ; dépassement = `SQLITE_FULL`, jamais un débordement mémoire.
- Le compilateur hôte signale au build les requêtes qui demandent un tri externe.

## SQL : compilation sur l'hôte et C généré

Toutes les requêtes du firmware sont connues au build : un compilateur sur l'hôte les vérifie contre le schéma et les traduit en fonctions C qui appellent l'API curseurs. Aucun interpréteur n'est embarqué.

### Sources des requêtes

1. **Fichiers `queries.sql`** déclarés dans le projet, avec un nom par requête (`-- name: sensor_last`).
2. **Extraction statique** des chaînes littérales passées à `sqlite3_prepare*` et `sqlite3_exec` dans les sources C.
3. **Capture à l'exécution** : les tests de l'application sous `native_sim` en mode enregistrement consignent toutes les requêtes préparées dans un manifeste versionné avec le projet.

### Chaîne de compilation

1. Parseur : grammaire dérivée de `parse.y` de SQLite (domaine public), pour une syntaxe identique.
2. Résolution contre le catalogue (tables, colonnes, types, index) ; une colonne inconnue casse le build.
3. Planification : choix d'index par règles simples et statistiques optionnelles, jointures en boucles imbriquées sur index.
4. Représentation intermédiaire (IR) documentée et versionnée, réutilisable plus tard par un éventuel interpréteur ou par LLEXT.
5. Génération de C : une fonction `step` par requête, une struct d'état (curseurs, paramètres, colonnes), un rapport de ressources (curseurs, RAM, tri externe).
6. Table `hash(SQL normalisé) → requête` pour `sqlite3_prepare_v2()`.

L'outil est en Python (comme le reste de l'outillage Zephyr) et branché dans CMake via `zephyr_sqldb_queries()`.

### Exemple

```c
/* queries.sql */
-- name: sensor_last
SELECT ts, value FROM sensor WHERE id = :id ORDER BY ts DESC LIMIT :n;

/* généré (extrait) */
int q_sensor_last_step(q_sensor_last_t *q) {
    if (!q->started) {
        sqldb_cur_open(q->txn, &q->c, TREE_SENSOR_PK);
        sqldb_cur_seek(&q->c, KEY(q->id, TS_MAX), SQLDB_SEEK_LE);
        q->started = 1;
    } else {
        sqldb_cur_prev(&q->c);
    }
    if (q->count >= q->n || !sqldb_cur_valid(&q->c) || cur_i32(&q->c, 0) != q->id) {
        return SQLITE_DONE;
    }
    q->ts    = cur_i64(&q->c, 1);
    q->value = cur_f64(&q->c, 2);
    q->count++;
    return SQLITE_ROW;
}
```

### Sous-ensemble SQL de la version 1

| Catégorie | Pris en charge |
| --- | --- |
| Lecture | `SELECT` avec `WHERE`, `ORDER BY`, `LIMIT/OFFSET`, `GROUP BY`/`HAVING`, `DISTINCT`, agrégats, sous-requêtes scalaires et `IN (SELECT ...)`, `INNER`/`LEFT JOIN` |
| Écriture | `INSERT` (dont `OR REPLACE`/`OR IGNORE`), `UPSERT`, `UPDATE`, `DELETE`, `RETURNING` |
| Transactions | `BEGIN`, `COMMIT`, `ROLLBACK`, `SAVEPOINT`, `RELEASE` |
| Expressions | Arithmétique, comparaisons, `LIKE`, `GLOB`, `BETWEEN`, `CASE`, `COALESCE`, `IFNULL`, `CAST`, fonctions scalaires courantes (`abs`, `length`, `substr`, `lower`, `upper`, `round`, `min`/`max`) |
| Schéma (migrations uniquement) | `CREATE TABLE`, `CREATE INDEX`, `ALTER TABLE ADD/RENAME/DROP COLUMN`, `DROP TABLE/INDEX` |
| Contraintes | `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `CHECK`, `DEFAULT`, `FOREIGN KEY` |

### API typée (optionnelle)

En plus de la façade `sqlite3.h`, chaque requête nommée génère une API typée (`q_sensor_last_bind(q, id, n)`, champs `q->ts`, `q->value`). Elle est recommandée pour le code neuf : un changement de schéma casse le build plutôt que le terrain.

## Schéma et migrations OTA

Le schéma ne change que par une migration livrée avec le firmware, et une migration ne devient définitive qu'après la confirmation de l'image par MCUboot.

### Catalogue versionné

```
schema/
  v1.sql          -- schéma initial
  v2.sql          -- ALTER TABLE / CREATE INDEX
  v2_up.sql       -- transformation de données (optionnelle)
  v3.sql
```

- Le catalogue (tables, colonnes, index, version) est stocké dans la base et compilé dans le firmware.
- Le firmware embarque la version N et les migrations depuis les versions N-k (`CONFIG_SQLDB_MIGRATE_FROM_MIN`).
- Au montage : version base = firmware → rien ; base plus ancienne → migration ; base plus récente → ouverture en mode compatible si le format le permet, sinon `SQLITE_CANTOPEN` avec un code explicite.

### Protocole avec MCUboot

1. Le nouveau firmware démarre en mode test (image non confirmée).
2. Il exécute la migration v(N-1) → vN dans une transaction. L'ancienne racine est **épinglée** : le ramasse-miettes ne la récupère pas.
3. L'application tourne normalement sur la base migrée.
4. Si l'application appelle `boot_write_img_confirmed()`, elle appelle aussi `sqldb_migration_confirm()` : l'épinglage est levé et l'ancienne version devient récupérable.
5. Si MCUboot revient à l'ancienne image, l'ancien firmware trouve la racine épinglée et rouvre la base telle qu'elle était avant la migration.

Les écritures faites entre la migration et le retour arrière sont perdues. C'est un choix documenté ; une option permet de rejouer les insertions d'un journal d'export si l'application en a besoin.

En mode journal (FRAM), l'épinglage passe par une copie de sauvegarde des pages modifiées par la migration, conservée jusqu'à la confirmation.

### Trois stratégies, combinables

| Stratégie | Principe | Quand |
| --- | --- | --- |
| Snapshot épinglé | L'ancienne racine survit jusqu'à la confirmation | Par défaut, toutes les migrations |
| Expand / contract | N+1 ne fait qu'ajouter (colonnes nullables, tables) ; N+2 supprime ce qui ne sert plus | Pour qu'un firmware N lise une base N+1 sans retour arrière de la base |
| Migration paresseuse | Chaque enregistrement porte sa version de schéma, converti à la lecture ou à la réécriture | Grosses tables qui ne tiennent pas dans l'espace libre |

### Contrôles au build

- Le compilateur vérifie que chaque migration mène bien du schéma N-1 au schéma N déclaré.
- Il signale les migrations destructives (`DROP COLUMN`, changement de type) et celles qui demandent une transformation complète des données, avec une estimation de l'espace libre nécessaire.
- La CI exécute le scénario « migration puis retour arrière MCUboot » sous `native_sim` avec coupures injectées.

## Compatibilité sqlite3.h et portage depuis Linux

Une application Linux se porte en recompilant, en lisant un rapport de compatibilité et en paramétrant ses requêtes dynamiques ; le reste du code SQLite ne change pas.

### Façade `sqlite3.h` (`CONFIG_SQLDB_SQLITE_COMPAT`)

- Fonctions : `sqlite3_open_v2`, `close_v2`, `prepare_v2/v3`, `bind_*`, `step`, `column_*`, `column_count/name/type`, `reset`, `clear_bindings`, `finalize`, `exec`, `errmsg`, `errcode`, `extended_errcode`, `changes`, `last_insert_rowid`, `busy_timeout`, `get_autocommit`.
- Codes de retour et constantes identiques (`SQLITE_OK`, `SQLITE_ROW`, `SQLITE_DONE`, `SQLITE_FULL`…).
- Sémantiques identiques : conversions entre types de `column_*`, durée de vie des pointeurs `column_text/blob`, comportement de `reset` et `finalize`.
- Compatibilité au niveau du code source uniquement, pas de l'ABI.
- `sqlite3_prepare_v2()` normalise et hache le SQL, puis retrouve la requête précompilée. Chaîne inconnue : `SQLITE_ERROR`, `errmsg = "query not precompiled: <hash>"`.

### Habitudes Linux prises en charge

| Habitude | Comportement |
| --- | --- |
| `CREATE TABLE IF NOT EXISTS` au démarrage | Comparé au catalogue : sans effet si identique, erreur explicite sinon |
| Migrations par `PRAGMA user_version` + `ALTER TABLE` | Extraites en migrations versionnées, protégées par le protocole OTA |
| `PRAGMA journal_mode`, `synchronous`, `cache_size` | Acceptés ; sans effet ou associés à la configuration |
| `PRAGMA foreign_keys`, `user_version` | Pris en charge |
| `sqlite3_open("/data/app.db")` | Le chemin est associé à une base déclarée dans le devicetree |
| Mode sérialisé multi-thread | `k_mutex` Zephyr |
| Affinité de types | Émulée avec les règles de SQLite |

### Parcours de portage

1. **Analyse** : `west sqldb check <app>` produit un rapport (requêtes acceptées, refusées avec la raison, SQL dynamique à corriger avec fichier et ligne, fonctions non prises en charge).
2. **Portage sous `native_sim`** : même code, mêmes tests, moteur remplacé. Le mode enregistrement capture les requêtes ; le test différentiel compare chaque résultat avec le vrai SQLite.
3. **Passage sur la cible** : choix du support dans le devicetree et du profil Kconfig.
4. **Optionnel** : adoption de l'API typée dans le code neuf.

### Ce qui change pour le développeur

- Le SQL construit à l'exécution (`sprintf` dans une requête) doit devenir une requête paramétrée.
- Les fonctionnalités hors périmètre (triggers, fonctions fenêtres, `ATTACH`, FTS…) sont détectées dès l'étape 1.
- Les limites de ressources renvoient les codes SQLite habituels (`SQLITE_FULL`, `SQLITE_NOMEM`) au lieu de grossir sans fin.

## Outillage de diagnostic

On interroge la base en SQL de deux façons : un SQL restreint directement sur la carte via le shell Zephyr, ou le SQL complet de SQLite depuis le PC avec `sqldb-cli`.

| Outil | Où s'exécute la requête | SQL disponible | Coût sur le MCU |
| --- | --- | --- | --- |
| Shell Zephyr `db` | MCU | Une table, filtres `AND`, `ORDER BY` sur index, agrégats simples ; plus les requêtes précompilées | \~6 à 12 Ko flash (estimation) |
| `sqldb-cli` (curseurs distants) | PC, vrai SQLite | Complet | Serveur de curseurs SMP, quelques Ko |
| `sqldb-cli --on-device` (LLEXT) | MCU, code natif | Le sous-ensemble du moteur | Chargeur LLEXT + RAM ou flash pour le code |
| Export | PC, sur une copie | Complet | Envoi des pages via mcumgr |

### Shell Zephyr intégré (`CONFIG_SQLDB_SHELL`)

```
uart:~$ db .tables
sensor  event  config
uart:~$ db SELECT ts, value FROM sensor WHERE id = 3 AND value > 20.5 ORDER BY ts DESC LIMIT 5;
ts          value
----------  -----
1760783012  21.7
1760782952  20.9
(2 rows, idx sensor_pk, 4 ms)
uart:~$ db .run sensor_last id=3 n=2
uart:~$ db EXPLAIN SELECT * FROM sensor WHERE value > 25;
FULL SCAN sensor (~12 000 rows est.)  — no index on value
```

- Commandes à point comme le CLI `sqlite3` : `.tables`, `.schema`, `.indexes`, `.stats`, `.queries`, `.run`, `.mode table|csv|json|line`, `.limit`, `.readonly`.
- Un mini-parseur traduit le SQL restreint en une structure de filtre déclarative (table, index, plage de clés, filtres, colonnes, limite, agrégat) : pas de boucle ni de saut, coût vérifiable avant exécution.
- Autocomplétion des tables, colonnes et requêtes par les sous-commandes dynamiques du shell.
- Lecture sur snapshot, parcours par tranches qui cèdent la main, `Ctrl+C`, délai maximal et plafond de lignes par défaut.
- Écritures désactivées par défaut (`CONFIG_SQLDB_SHELL_WRITE`), soumises si besoin à l'authentification du shell.
- Requête hors périmètre : erreur avec suggestion (`use sqldb-cli from host, or add it to queries.sql`).

### `sqldb-cli` (hôte)

- `sqldb-cli --port /dev/ttyACM0` ouvre un prompt SQLite sur le PC. Transports : UART, USB CDC, BLE, UDP, via SMP (mcumgr).
- Chaque table de la carte apparaît comme une table virtuelle SQLite. Les contraintes transmises par `xBestIndex` deviennent des positionnements sur plage, et les filtres simples sont poussés vers le MCU (même structure de filtre que le shell).
- Le MCU n'exécute que des commandes de curseur : `list`, `open`, `seek`, `next`, `read`, et en option `insert`/`delete`, sur snapshot.
- Mêmes commandes à point que le shell, plus `.export` (copie complète vers un fichier SQLite standard).

### Mode `--on-device` (LLEXT, optionnel)

- Pour une liaison lente : `sqldb-cli` compile la requête avec le même générateur de C, puis pour l'architecture de la cible, et l'envoie comme module LLEXT signé.
- Le firmware vérifie la signature et la version d'ABI, charge le module, l'exécute et renvoie seulement le résultat.
- Disponible uniquement avec `CONFIG_SQLDB_LLEXT=y`, destiné aux MCU confortables et aux usages maîtrisés.

## Stratégie de test

Les garanties de sqldb valent ce que valent ses tests : le fuzzing de coupure et le test différentiel contre SQLite existent dès la première ligne de code du moteur.

| Niveau | Outil | Ce qui est vérifié |
| --- | --- | --- |
| Coupure de courant | Pilote de support simulé sous `native_sim` | Coupure à chaque écriture, programmation ou effacement possible, y compris au milieu d'une écriture multi-octets ; après remontage : intégrité du B+tree, base égale à l'état avant ou après le dernier commit |
| Modèles de média | Simulateurs NOR, NAND, FRAM | Bits restant à 1 après effacement interrompu, pages partiellement programmées, blocs défectueux, erreurs ECC |
| Moteur | Tests unitaires Ztest + fuzzing (libFuzzer sur `native_sim`) | B+tree, encodage, transactions, snapshots, ramasse-miettes |
| SQL | Test différentiel contre SQLite sur l'hôte | Mêmes requêtes, mêmes données, mêmes résultats ; requêtes générées aléatoirement à la manière de SQLancer |
| Migrations | Scénarios Twister | Migration N-k → N, retour arrière MCUboot, coupure pendant migration, migration paresseuse |
| Matériel | Twister sur cartes réelles | Coupures d'alimentation pilotées par relais sur NOR QSPI et FRAM SPI ; usure accélérée |
| Ressources | Rapports du compilateur + tests de charge | Pire cas de RAM, de pile et de temps par requête, `SQLITE_FULL` au lieu de débordements |

- La CI exécute le fuzzing de coupure sur chaque commit (budget limité) et en continu la nuit.
- Le corpus de bugs trouvés devient un jeu de non-régression.
- Objectif de couverture : 100 % des branches du gestionnaire de pages et des stratégies de commit.

## Profils Kconfig et empreinte

Trois profils couvrent la gamme des MCU Zephyr ; les chiffres sont des estimations à valider sur le premier prototype.

| Profil | Cibles typiques | Contenu | Flash (estim.) | RAM (estim.) |
| --- | --- | --- | --- | --- |
| `tiny` | Cortex-M0+/M23, FRAM ou NOR | Requêtes précompilées une table, pas de jointure ni de tri externe, un seul support | 15 à 25 Ko | 1 à 2 Ko |
| `standard` | Cortex-M4/M33 | Jointures, tri externe, migrations, façade `sqlite3.h`, shell | 40 à 70 Ko | 4 à 16 Ko |
| `full` | Cortex-M7, ESP32, nRF54 | Tout `standard`, mode hybride, serveur `sqldb-cli`, LLEXT | 80 à 120 Ko | Configurable |

Principales options :

- `CONFIG_SQLDB_PROFILE_TINY` / `_STANDARD` / `_FULL`
- `CONFIG_SQLDB_PAGE_CACHE_COUNT`, `CONFIG_SQLDB_MAX_CURSORS`, `CONFIG_SQLDB_SORT_RAM_SIZE`, `CONFIG_SQLDB_SORT_TMP_PAGES`
- `CONFIG_SQLDB_SQLITE_COMPAT`, `CONFIG_SQLDB_SHELL`, `CONFIG_SQLDB_SHELL_WRITE`, `CONFIG_SQLDB_REMOTE` (serveur SMP), `CONFIG_SQLDB_LLEXT`
- `CONFIG_SQLDB_MIGRATE_FROM_MIN`, `CONFIG_SQLDB_LAZY_MIGRATION`
- `CONFIG_SQLDB_MEDIA_NOR`, `_NAND`, `_FRAM`, `_LITTLEFS`, `_HYBRID`

Le C généré ajoute quelques centaines d'octets de flash par requête ; l'éditeur de liens retire les parties du moteur que les requêtes n'utilisent pas.

## Roadmap, risques et questions ouvertes

Le socle (support, coupures, pages) passe en premier parce que toutes les garanties en dépendent ; le SQL vient ensuite, validé contre SQLite.

### Phases

1. **Socle** : pilotes RAM, NOR et FRAM, simulateur de coupure sous `native_sim`, gestionnaire de pages avec shadow paging et journal en place. Critère de sortie : zéro échec sur le fuzzing de coupure.
2. **Stockage structuré** : B+tree, encodage, transactions, snapshots, API curseurs, ramasse-miettes.
3. **Compilateur SQL** : parseur, catalogue, planificateur, génération de C pour `SELECT`/`INSERT`/`UPDATE`/`DELETE` sur une table ; test différentiel en CI.
4. **Façade `sqlite3.h` et portage** : extraction et capture des requêtes, `west sqldb check`, portage d'une application Linux réelle comme preuve.
5. **Migrations OTA** : catalogue versionné, snapshot épinglé avec MCUboot, expand/contract.
6. **Outillage** : shell Zephyr, serveur de curseurs SMP, `sqldb-cli`.
7. **Extensions** : jointures et tri externe complets, NAND, mode hybride, LittleFS, migration paresseuse, LLEXT.

### Risques

| Risque | Impact | Parade |
| --- | --- | --- |
| Effort de test sous-estimé | Garanties non tenues sur le terrain | Simulateur de coupure en phase 1, critère de sortie bloquant |
| Écarts de sémantique avec SQLite (NULL, affinité, collations) | Bugs subtils au portage | Test différentiel systématique, liste des écarts publiée |
| SQL dynamique très répandu dans les applications à porter | Portage moins fluide que promis | Rapport `west sqldb check` précis ; option future d'interpréteur ou LLEXT |
| Format sur disque figé trop tôt | Migrations de format impossibles | Numéro de version de format dans le superblock dès la phase 1 |
| Empreinte réelle supérieure aux estimations | Profil `tiny` irréaliste | Mesure à chaque phase, budget flash suivi en CI |
| Comportement réel des FRAM et NOR face aux coupures | Modèles de simulation incomplets | Banc de coupures matérielles en phase 1 |

### Questions ouvertes

- [ ] Les écritures faites entre une migration et un retour arrière MCUboot peuvent-elles être perdues, ou faut-il les préserver ?
- [ ] Faut-il prendre en charge plusieurs bases par appareil dès la version 1 ?
- [ ] Le compilateur hôte doit-il rester en Python, ou passer en Rust pour la vitesse et le partage de code avec `sqldb-cli` ?
- [ ] Licence du module (Apache 2.0 comme Zephyr ?) et nom définitif. Nom: SQNano. Apache-2
- [ ] Quelle application Linux réelle servira de preuve de portage en phase 4 ?
