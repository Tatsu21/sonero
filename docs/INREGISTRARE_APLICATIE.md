# Înregistrarea unei Aplicații Noi în Sonero — De la Detectare la Alocarea unui Canal

Acest document descrie **procesul complet** prin care o aplicație nouă este detectată de Sonero, înregistrată în sistem și alocată unui canal audio, până la momentul în care primește un canal funcțional.

---

## 📋 Cuprins

1. [Precondiții](#-precondiții)
2. [Arhitectura de bază](#-arhitectura-de-bază)
3. [Fluxul de detectare automată a aplicațiilor](#-fluxul-de-detectare-automată-a-aplicațiilor)
4. [Asignarea manuală a aplicației la un canal](#-asignarea-manuală-a-aplicației-la-un-canal)
5. [Verificarea alocării canalului](#-verificarea-alocării-canalului)
6. [Depanarea problemelor comune](#-depanarea-problemelor-comune)
7. [Diagrama fluxului](#-diagrama-fluxului)

---

## 🔧 Precondiții

Pentru ca procesul de detectare și alocare să funcționeze, trebuie îndeplinite următoarele condiții:

| Condiție | Descriere | Verificare |
|----------|-----------|------------|
| **PipeWire rulează** | Daemon-ul PipeWire trebuie să fie activ | `systemctl --user status pipewire pipewire-pulse` |
| **Sonero pornit** | Aplicația Sonero trebuie să fie lansată | `pgrep -x Sonero` |
| **Backend inițializat** | `PipeWireManager` trebuie să fie în stare `Available` | Verificare în logs: `PipeWire: connected to` |
| **Virtual sinks create** | Modulele de filter-chain pentru canale trebuie create | Verificare în logs: `PipeWire: created X virtual channel sinks` |

> ⚠️ **Notă**: Dacă PipeWire nu rulează, `PipeWireManager::initialize()` va eșua și va returna `BackendState::Unavailable`.

---

## 🏗️ Arhitectura de bază

### Componentele cheie

```
Sonero
├── app/              # Application (entry point)
│   └── Application.cpp/h  # Main application class
├── audio/            # Audio backend
│   ├── PipeWireManager.cpp/h  # PipeWire backend implementation
│   ├── Channel.h     # Channel definitions (System, Game, Chat, etc.)
│   ├── IAppRouter.h  # Interface for app routing
│   └── ...
└── ui/               # User interface
```

### Canalele disponibile

Sonero definește **7 canale predefinite** în `audio/Channel.h`:

| Canal | ID | Nume intern PipeWire | Descriere |
|-------|----|---------------------|------------|
| System | 0 | `sonar_system` | Aplicații de sistem |
| Game | 1 | `sonar_game` | Jocuri |
| Chat | 2 | `sonar_chat` | Aplicații de chat (Discord, Teams, etc.) |
| Media | 3 | `sonar_media` | Media players (Spotify, VLC, etc.) |
| Browser | 4 | `sonar_browser` | Browser-e web |
| Microphone | 5 | `sonar_microphone` | Intrări microfon |
| Aux | 6 | `sonar_aux` | Canal auxilar |

Fiecare canal corespunde unui **virtual sink** în PipeWire, creat prin module de tip `filter-chain`.

---

## 🔄 Fluxul de detectare automată a aplicațiilor

### Pasul 1: Inițializarea PipeWireManager

Când Sonero pornește, `PipeWireManager` execută următoarea secvență:

```cpp
// În Application.cpp
PipeWireManager manager;
manager.initialize();
```

**Ce se întâmplă în `initialize()`:**

1. **Crearea thread loop-ului PipeWire**
   ```cpp
   loop_ = pw_thread_loop_new("sonero", nullptr);
   pw_thread_loop_start(loop_);
   ```

2. **Conectarea la server-ul PipeWire**
   ```cpp
   context_ = pw_context_new(...);
   core_ = pw_context_connect(context_, nullptr, 0);
   ```

3. **Handshake și sincronizare**
   ```cpp
   doRoundtrip(); // Așteaptă confirmare de la server
   ```

4. **Configurarea grafului** (`setupGraph()`)
   - Obținerea registry-ului PipeWire
   - Enumerarea obiectelor existente
   - **Crearea virtual sinks** (`createVirtualSinks()`)

### Pasul 2: Crearea Virtual Sinks

Pentru fiecare canal din `kAllChannels`, se creează un **modul filter-chain**:

```cpp
// audio/PipeWireManager.cpp:263-292
void PipeWireManager::createVirtualSinks() {
    for (const ChannelId id : kAllChannels) {
        const std::string node = nodeNameFor(id); // e.g., "sonar_game"
        const std::string desc = "Sonero " + std::string(channelName(id));
        
        const std::string args = filterChainArgs(node, desc, dspFreqs(), gains, kEqQ);
        
        pw_impl_module* module = pw_context_load_module(
            context_, "libpipewire-module-filter-chain", args.c_str(), nullptr);
        
        modules_.push_back(module);
    }
}
```

**Ce face un modul filter-chain:**
- Creează un **Audio/Sink** virtual (ex: `sonar_game`)
- Include un graf de filtre (31 de benzi EQ) pentru procesare audio
- Forwardă audio către ieșirea implicită a sistemului
- Expune un **nod de captare** (`.out`) pentru monitorizare

> ✅ **Rezultat**: 7 virtual sinks sunt create în PipeWire, gata să primească audio de la aplicații.

### Pasul 3: Înregistrarea listener-ului de registry

PipeWireManager înregistrează un listener pentru evenimentele de **registry global**:

```cpp
// audio/PipeWireManager.cpp:242-253
pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);
```

Acest listener va fi notificat de **orice nou obiect** care apare în graful PipeWire.

### Pasul 4: Detectarea unei aplicații noi

Când o aplicație nouă pornește și produce audio, PipeWire creează un **stream de ieșire audio** de tip `Stream/Output/Audio`.

**Exemplu de proprietăți ale unui stream:**
```
media.class = "Stream/Output/Audio"
node.name = "firefox.output"
application.name = "Firefox"
media.name = "Firefox"
object.serial = 12345
```

**PipeWireManager detectează acest stream** în callback-ul `onGlobal()`:

```cpp
// audio/PipeWireManager.cpp:476-498
} else if (std::strcmp(mediaClass, "Stream/Output/Audio") == 0) {
    if (startsWith(nodeName, "sonar_")) {
        // Ignoră propriile stream-uri Sonero
        return;
    }
    
    AppInfo info{displayNameOf(props, id), serialOf(props)};
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        apps_[id] = std::move(info);  // 📌 ÎNREGISTRARE!
    }
    bumpRevision();  // Incrementă contorul de revizie
}
```

**Ce se întâmplă aici:**
1. Se extrag proprietățile stream-ului (nume, serial)
2. Se creează un obiect `AppInfo` cu:
   - `name`: Numele afișat (din `application.name` sau `media.name`)
   - `serial`: Serial-ul unic al obiectului PipeWire
3. Aplicația este **adăugată în mapa `apps_`** cu cheia = `nodeId` (ID-ul PipeWire)
4. Se incrementă `revision_` pentru a notifica UI-ul că lista de aplicații s-a schimbat

> ✅ **Rezultat**: Aplicația este **înregistrată** în sistemul Sonero și apare în lista de aplicații disponibile.

### Pasul 5: Notificarea UI-ului

Interfața grafică (UI) poate obține lista curentă de aplicații prin:

```cpp
std::vector<AppStream> apps = audioBackend->applications();
```

Fiecare `AppStream` conține:
- `id`: ID-ul PipeWire al aplicației
- `name`: Numele afișat
- `channel`: Canalul alocat (inițial `std::nullopt` = nealocat)

UI-ul va afișa aplicația în interfață, de obicei cu opțiunea de a alege un canal.

---

## 🎯 Asignarea manuală a aplicației la un canal

Până în acest punct, aplicația este **detectată și înregistrată**, dar **nu este alocată niciunui canal**. Audio-ul ei merge către ieșirea implicită a sistemului.

### Pasul 6: Selectarea canalului de către utilizator

Utilizatorul selectează din UI:
- Aplicația: `Firefox` (ID: 12345)
- Canalul dorit: `Media`

UI-ul apelează:

```cpp
audioBackend->assign(appId, ChannelId::Media);
```

### Pasul 7: Executarea asignării

Metoda `assign()` din `PipeWireManager` execută următorii pași:

```cpp
// audio/PipeWireManager.cpp:403-429
bool PipeWireManager::assign(std::uint32_t appId, ChannelId channel) {
    std::string serial;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        
        // 1. Obține serial-ul sink-ului virtual pentru canal
        const std::string node = nodeNameFor(channel); // "sonar_media"
        const auto it = sinkSerials_.find(node);
        if (it == sinkSerials_.end()) {
            // Sink-ul nu este gata încă
            return false;
        }
        serial = std::to_string(it->second);
        
        // 2.Înregistrează asignarea în mapa internă
        assignments_[appId] = channel;
    }
    
    // 3. Setează metadata target.object
    pw_thread_loop_lock(loop_);
    pw_metadata_set_property(
        metadata_, 
        appId, 
        "target.object", 
        nullptr, 
        serial.c_str()  // Serial-ul sink-ului virtual
    );
    pw_thread_loop_unlock(loop_);
    
    bumpRevision();
    log::info("Router: routed app {} to channel {}", appId, channelName(channel));
    return true;
}
```

**Ce se întâmplă aici:**

1. **Verificare sink virtual**
   - Se obține numele nodului pentru canal (ex: `sonar_media`)
   - Se caută serial-ul său în `sinkSerials_` (populat când sink-ul a fost creat)
   - Dacă sink-ul nu există sau nu este gata → **eșec**

2. **Înregistrare asignare internă**
   - Se adaugă în `assignments_`: `{appId: channel}`

3. **Setare metadata PipeWire**
   - Se folosește **PipeWire Metadata API** pentru a seta:
     ```
     target.object = "<serial_sink_virtual>"
     ```
   - Aceasta instruiește PipeWire să **redirecționeze** audio-ul aplicației către sink-ul virtual

4. **Notificare UI**
   - `bumpRevision()` incrementă contorul de revizie
   - UI-ul va detecta schimbarea și va afișa aplicația ca fiind alocată canalului

> ✅ **Rezultat**: Aplicația este **asignată canalului** și audio-ul ei merge către virtual sink-ul corespunzător.

### Pasul 8: Aplicarea rutei

Când se setează `target.object`, PipeWire **redirecționează automat** stream-ul aplicației către sink-ul virtual.

În plus, `PipeWireManager` poate re-aplica rutele manual:

```cpp
// audio/PipeWireManager.cpp:155-156
void applyChannelRouteLocked(ChannelId id);
void applyAllRoutesLocked();
```

Aceste funcții asigură că:
- Fiecare canal are stream-ul de ieșire (`sonar_<channel>.out`) conectat la dispozitivul audio corect
- Dacă dispozitivul implicit se schimbă, toate canalele sunt re-rutate

---

## ✅ Verificarea alocării canalului

### Comenzi pentru verificare

#### 1. Listare aplicații detectate

```bash
# Folosind pw-cli (PipeWire CLI)
pw-cli list-objects | grep -E "Stream/Output/Audio|node.name"
```

#### 2. Verificare target.object

```bash
# Verifică metadata target.object pentru un stream
pw-cli info <stream_id> | grep target.object
```

Exemplu de ieșire:
```
target.object = "123"  # Serial-ul sink-ului virtual
```

#### 3. Listare virtual sinks

```bash
pw-cli list-objects | grep sonar_
```

Exemplu:
```
id 42, type Node/Audio/Sink, node.name = "sonar_media"
id 43, type Node/Audio/Sink, node.name = "sonar_game"
...
```

#### 4. Verificare conexiuni

```bash
# Afișează graful de conexiuni
pw-cli dump | grep -A5 -B5 "sonar_media"
```

#### 5. Log-uri Sonero

```bash
# Rulează Sonero cu log-uri detaliate
./build/Sonero --verbose
```

Caută mesajele:
```
PipeWire: created 7 virtual channel sinks
Router: routed app 12345 to channel Media
```

### Verificare programatică

Din codul C++:

```cpp
// Obține lista de aplicații
std::vector<AppStream> apps = pipeWireManager.applications();

for (const auto& app : apps) {
    std::cout << "App: " << app.name << " (ID: " << app.id << ")";
    if (app.channel) {
        std::cout << " -> Channel: " << channelName(*app.channel);
    } else {
        std::cout << " -> Not assigned to any channel";
    }
    std::cout << std::endl;
}
```

---

## 🚨 Depanarea problemelor comune

### Problema 1: Aplicația nu apare în listă

| Cauză | Soluție |
|-------|---------|
| PipeWire nu rulează | `systemctl --user start pipewire pipewire-pulse` |
| Aplicația nu folosește PipeWire | Verifică dacă aplicația rulează cu PulseAudio compatibil |
| Aplicația nu produce audio | Lansază aplicația și rulează audio |
| Backend-ul nu este disponibil | Verifică `pipeWireManager.state()` |

**Debug:**
```bash
# Verifică dacă aplicația apare în PipeWire
pw-cli list-objects | grep -i firefox

# Verifică stare backend
std::cout << "Backend state: " << pipeWireManager.state() << std::endl;
```

### Problema 2: Aplicația apare dar nu poate fi asignată

| Cauză | Soluție |
|-------|---------|
| Virtual sink-ul nu este creat | Așteaptă ca `createVirtualSinks()` să se finalizeze |
| Sink-ul nu are serial valid | Verifică `sinkSerials_` în `PipeWireManager` |
| Metadata API nu este disponibil | Verifică `metadata_ != nullptr` |

**Debug:**
```cpp
// Verifică dacă sink-ul virtual există
std::string nodeName = PipeWireManager::nodeNameFor(ChannelId::Media);
auto it = sinkSerials_.find(nodeName);
if (it == sinkSerials_.end()) {
    std::cerr << "Virtual sink not ready: " << nodeName << std::endl;
}
```

### Problema 3: Audio-ul nu merge către canalul selectat

| Cauză | Soluție |
|-------|---------|
| `target.object` nu este setat corect | Verifică cu `pw-cli info <app_id>` |
| Sink-ul virtual nu este conectat la ieșire | Verifică conexiunile cu `pw-cli dump` |
| Metadata a fost suprascrisă | Reapelează `assign()` |

**Debug:**
```bash
# Verifică target.object
pw-cli info <app_id> | grep target.object

# Forțează re-aplicarea rutei
pipeWireManager.applyAllRoutesLocked();
```

### Problema 4: Virtual sinks nu sunt create

| Cauză | Soluție |
|-------|---------|
| `libpipewire-module-filter-chain` lipsește | Instalează pachetul `pipewire` complet |
| Permisiuni insuficiente | Rulează cu drepuri de user corespunzătoare |
| Eroare la încărcarea modului | Verifică logs pentru `pw_context_load_module` |

**Debug:**
```bash
# Verifică modulele disponibile
ls /usr/lib/pipewire-*/libpipewire-module-filter-chain*

# Verifică logs
journalctl --user -u pipewire -f
```

---

## 📊 Diagrama fluxului

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         PROCESUL DE ÎNREGISTRARE A UNEI APLICAȚII                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                              │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐  │
│  │   PAS 1:    │    │   PAS 2:    │    │   PAS 3:    │    │   PAS 4:    │  │
│  │ Initializare│────▶│Creare Sinks │────▶│ Detectare  │────▶│ Asignare   │  │
│  │  Backend    │    │  Virtuale   │    │ Aplicație   │    │   la Canal │  │
│  └─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘  │
│         │                 │                 │                 │             │
│         ▼                 ▼                 ▼                 ▼             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  PipeWireManager::initialize()                                        │   │
│  │    ├── pw_thread_loop_new()                                          │   │
│  │    ├── pw_context_connect()                                          │   │
│  │    └── setupGraph()                                                  │   │
│  │        ├── pw_registry_add_listener()                               │   │
│  │        └── createVirtualSinks()                                      │   │
│  │            └── pw_context_load_module("libpipewire-module-filter-chain")│   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  PipeWireManager::onGlobal()                                          │   │
│  │    ├── Detectează Stream/Output/Audio                                │   │
│  │    ├── Extrage node.name, application.name                          │   │
│  │    └── Adaugă în apps_[id] = {name, serial}                            │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  PipeWireManager::assign(appId, channel)                              │   │
│  │    ├── Obține serial sink virtual: sinkSerials_[nodeName]           │   │
│  │    ├── Adaugă în assignments_[appId] = channel                        │   │
│  │    └── pw_metadata_set_property(metadata_, appId, "target.object", serial)│   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │  REZULTAT FINAL:                                                       │   │
│  │    ✓ Aplicația este înregistrată în Sonero                        │   │
│  │    ✓ Audio-ul aplicației merge către virtual sink-ul canalului        │   │
│  │    ✓ UI-ul afișează aplicația cu canalul alocat                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Flux secvențial detaliat

```
1. Utilizatorul lansează Sonero
   ↓
2. Application.cpp: PipeWireManager::initialize()
   ↓
3. PipeWireManager creează thread loop și se conectează la server
   ↓
4. setupGraph() → createVirtualSinks()
   ↓
5. Pentru fiecare canal (System, Game, Chat, ...):
   │  → pw_context_load_module("libpipewire-module-filter-chain", ...)
   │  → Creează nodul virtual (ex: sonar_game)
   ↓
6. pw_registry_add_listener() pentru detectare obiecte noi
   ↓
7. Utilizatorul lansează o aplicație (ex: Firefox)
   ↓
8. PipeWire creează un Stream/Output/Audio pentru Firefox
   ↓
9. PipeWireManager::onGlobal() detectează noul stream
   │  → Verifică media.class == "Stream/Output/Audio"
   │  → Extrage application.name = "Firefox"
   │  → Adaugă în apps_[id] = {name: "Firefox", serial: ...}
   │  → bumpRevision()
   ↓
10. UI-ul detectează schimbarea (revision a crescut)
    ↓
11. UI-ul afișează Firefox în lista de aplicații
    ↓
12. Utilizatorul selectează: Firefox → Canal: Media
    ↓
13. UI-ul apelează: pipeWireManager.assign(appId, ChannelId::Media)
    ↓
14. PipeWireManager::assign():
    │  → Obține serial-ul pentru sonar_media
    │  → Adaugă în assignments_[appId] = ChannelId::Media
    │  → pw_metadata_set_property(metadata_, appId, "target.object", serial)
    │  → bumpRevision()
    ↓
15. PipeWire redirecționează automat audio-ul Firefox către sonar_media
    ↓
16. Audio-ul trece prin filter-chain-ul sonar_media (EQ, volum, etc.)
    ↓
17. Audio-ul iese către dispozitivul fizic
    ↓
18. ✅ Aplicația primește canalul!
```

---

## 📝 Rezumat

| Etapă | Acțiune | Componentă | Rezultat |
|-------|---------|------------|----------|
| 1 | Inițializare backend | `PipeWireManager::initialize()` | Conexiune la PipeWire |
| 2 | Creare infrastructură | `createVirtualSinks()` | 7 virtual sinks create |
| 3 | Detectare aplicație | `onGlobal()` + `Stream/Output/Audio` | Aplicație înregistrată în `apps_` |
| 4 | Selectare canal | UI → `assign(appId, channel)` | Asignare înregistrată |
| 5 | Rutare audio | `pw_metadata_set_property()` | Audio redirecționat |
| 6 | Confirmare | UI + logs | Aplicația are canal |

---

## 🔗 Referințe

- **Fișiere cheie:**
  - [`audio/PipeWireManager.cpp`](../audio/PipeWireManager.cpp) — Implementarea principală
  - [`audio/PipeWireManager.h`](../audio/PipeWireManager.h) — Declarații
  - [`audio/Channel.h`](../audio/Channel.h) — Definiția canalelor
  - [`audio/IAppRouter.h`](../audio/IAppRouter.h) — Interfață routing

- **PipeWire Documentație:**
  - [PipeWire API Reference](https://pipewire.pages.freedesktop.org/wire/plumbing/)
  - [Module filter-chain](https://pipewire.pages.freedesktop.org/wire/modules/module-filter-chain/)

- **Concepte cheie:**
  - [PipeWire Nodes and Streams](https://pipewire.pages.freedesktop.org/wire/concepts/nodes/)
  - [PipeWire Metadata](https://pipewire.pages.freedesktop.org/wire/concepts/metadata/)

---

*Document generat pentru Sonero — o alternativă open-source la SteelSeries Sonar pentru Linux.*
