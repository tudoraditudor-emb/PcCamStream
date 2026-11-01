# PcCamStream

Aplicație C (Linux, GStreamer) care preia stream-ul video de la camera laptopului,
îl procesează în timp real și îl afișează pe ecran:

- **Input**: camera via `v4l2src`, în formatul video default al dispozitivului (nu se forțează niciun caps pe intrare; `decodebin` preia orice format — raw sau MJPEG).
- **Procesare**:
  1. **Scale** — `videoscale` + capsfilter `video/x-raw,width=W,height=H` (implicit 640x480, configurabil).
  2. **Flip** — `videoflip` (implicit `horizontal-flip`, vezi `--flip`).
  3. **Invert colors** — fiecare cadru RGB este inversat octet cu octet (`255 - v`) în C, într-un callback `appsink` → `appsrc`.
- **Output**: formatul este **forțat la H264** prin capsfilter `video/x-h264` după encoder (`x264enc`), apoi stream-ul H264 este decodat (`avdec_h264`) și afișat pe ecran (`autovideosink`) — sink de ecran, deci fără overhead de fișier, păstrând frame-rate-ul intrării.

Pipeline (două conducte — una cu două lanțuri deconectate ar rămâne blocată în preroll,
fiindcă ramura `appsrc` nu produce buffer până nu este alimentată):

```
pipe_in : v4l2src ! decodebin ! videoconvert ! videoscale ! video/x-raw,format=RGB,WIDTHxHEIGHT
                  ! videoflip ! appsink (proc)
                                                          │ callback C: invert (255 - octet)
                                                          ▼
pipe_out: appsrc (out) ! videoconvert ! x264enc ! video/x-h264 (forțat) ! avdec_h264
                  ! videoconvert ! autovideosink
```

## Cerințe

Ubuntu / Debian (testat pe Ubuntu 24.04):

```sh
sudo apt update
sudo apt install -y build-essential pkg-config \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-ugly gstreamer1.0-libav
```

Acces la cameră (o singură dată, apoi relogin):

```sh
sudo usermod -aG video "$USER"
# verifică: ls -l /dev/video0 && gst-inspect-1.0 v4l2src
```

## Compilare

```sh
make            # produce binarul ./pc-cam-stream
```

## Rulare

```sh
make run        # sau: ./pc-cam-stream  — cameră → procesare → H264 → ecran
```

Opțiuni:

| Opțiune | Implicit | Descriere |
|---|---|---|
| `--device=DEV` | `/dev/video0` | dispozitivul camerei |
| `--width=PX` / `--height=PX` | `640` / `480` | rezoluția de ieșire (scale) |
| `--flip=MODE` | `horizontal` | `none` \| `horizontal` \| `vertical` \| `180` |
| `--bitrate=KBPS` | `800` | bitrate-ul H264 |
| `--sink=ELEM` | `autovideosink` | sink-ul final (ex. `fakesink` headless) |
| `--frames=N` | `0` (infinit) | oprește automat după N cadre procesate |
| `--test` | off | folosește `videotestsrc` în locul camerei |

Exemple:

```sh
./pc-cam-stream --width=1280 --height=720 --flip=180 --bitrate=2000
./pc-cam-stream --test --sink=fakesink --frames=120   # smoke test fără ecran/cameră (make test)
```

Ctrl+C oprește aplicația și afișează statistica (cadre procesate, timp, fps).

## Structura proiectului

```
├── Makefile               # build (make / make run / make test / make clean)
├── README.md
├── docs/
│   ├── architecture.svg  # diagrama de arhitectură (imagine — se deschide oriunde)
│   ├── sequence.svg      # diagrama de secvență (imagine)
│   ├── architecture.txt  # aceeași arhitectură în ASCII (text brut)
│   ├── architecture.md   # diagrame Mermaid (randate automat pe GitHub)
│   └── architecture.puml # sursă PlantUML
└── src/
    └── main.c             # pipeline-urile GStreamer + inversia culorilor în C
```

## Arhitectură (UML)

Diagramele UML sunt în `docs/` — **arhitectură/componente** și **secvență**, în 4 formate:

- `architecture.svg` / `sequence.svg` — imagini; le deschizi în orice browser (`xdg-open docs/architecture.svg`) și GitHub le afișează direct în repo;
- `architecture.txt` — ASCII, text brut, îl vezi în orice editor;
- `architecture.md` — Mermaid, randat automat de interfața GitHub;
- `architecture.puml` — sursă PlantUML.

Randare locală a variantei PlantUML:

```sh
sudo apt install plantuml        # sau pluginul PlantUML în VS Code, sau https://www.plantuml.com/plantuml
plantuml docs/architecture.puml # generează .png/.svg lângă fișier
```

## Note de performanță

- `x264enc tune=zerolatency speed-preset=ultrafast` — encoder fără buffering, pentru latență minimă.
- Toate sink-urile rulează cu `sync=false` — cadrele sunt procesate cât de repede apar, păstrând frame-rate-ul camerei.
- Inversia se face pe CPU în callback-ul `appsink` (fără dependență de OpenGL), pe bufferul RGB forțat prin caps.

## Depanare

- `make: pkg-config: No such file or directory` și `gst/gst.h: No such file or directory` → **nu sunt instalate dependențele de build**; rulează comanda `sudo apt install ...` din secțiunea „Cerințe” (Makefile-ul afișează acum automat acest mesaj dacă lipsesc). După instalare: `make && ./pc-cam-stream`.
- `ERROR ... v4l2src` / permission denied pe `/dev/video0` → adaugă userul în grupul `video` (vezi Cerințe).
- `x264enc not found` → `sudo apt install gstreamer1.0-plugins-ugly`.
- Fără ecran (SSH/CI) → rulează cu `--test --sink=fakesink --frames=N`.
- Verificare rapidă a pipeline-ului direct din consolă:

  ```sh
  gst-launch-1.0 v4l2src ! decodebin ! videoconvert ! videoscale ! \
      video/x-raw,format=RGB,width=640,height=480 ! videoflip method=horizontal-flip ! \
      x264enc tune=zerolatency ! video/x-h264 ! avdec_h264 ! videoconvert ! autovideosink
  ```
