# PcCamStream — arhitectură (UML)

Diagrame Mermaid (randate automat de GitHub). Sursa PlantUML echivalentă: `docs/architecture.puml`.

## 1. Diagrama de componente / flux de date

```mermaid
flowchart LR
  subgraph FIZIC["Sistem fizic"]
    CAM[("Camera laptop<br>/dev/video0 (V4L2)<br>format default: MJPEG 1280x720@30")]
    SCREEN["Ecran (X11/Wayland)"]
  end

  subgraph APP["Aplicație C — pc-cam-stream"]
    subgraph PIN["pipe_in : GstPipeline"]
      v4l2["v4l2src"] --> dec["decodebin<br/>(ex. jpegdec)"] --> vc1["videoconvert"] --> sc["videoscale"] --> caps["capsfilter<br/>video/x-raw,format=RGB,WxH"] --> flip["videoflip<br/>horizontal-flip"] --> sink["appsink<br/>name=proc"]
    end
    CB["on_new_sample()<br/>invert: octet = 255 - octet"]
    subgraph POUT["pipe_out : GstPipeline"]
      src["appsrc<br/>name=out, is-live"] --> vc2["videoconvert<br/>RGB → I420"] --> enc["x264enc<br/>tune=zerolatency"] --> h264["capsfilter<br/>video/x-h264 (FORȚAT)"] --> avdec["avdec_h264"] --> vc3["videoconvert"] --> vsink["autovideosink<br/>sync=false"]
    end
    APPSTATS["App: GMainLoop + statistici (cadre, fps)"]
  end

  CAM --> v4l2
  sink -->|"semnal new-sample"| CB
  CB -->|"gst_app_src_push_buffer()"| src
  vsink --> SCREEN
  APPSTATS -.->|"g_signal_connect"| sink
  APPSTATS -.->|"pornire/oprire"| PIN
  APPSTATS -.->|"pornire/oprire"| POUT
```

> De ce două pipeline-uri: într-o singură conductă, ramura `appsrc` nu produce niciun buffer
> până nu e alimentată din callback, iar pipeline-ul așteaptă în preroll buffer de la toate
> sink-urile → deadlock. Două pipeline-uri rezolvă asta.

## 2. Diagrama de secvență — pornire, ciclul unui cadru, oprire

```mermaid
sequenceDiagram
  actor U as User
  participant M as main()
  participant PIN as pipe_in (v4l2src→…→appsink)
  participant CB as on_new_sample()
  participant POUT as pipe_out (appsrc→x264enc→video/x-h264→avdec→autovideosink)

  U->>M: ./pc-cam-stream --width=640 --height=480 --flip=horizontal
  M->>M: gst_init, parsează opțiuni, validează
  M->>PIN: gst_parse_launch(desc_in)
  M->>POUT: gst_parse_launch(desc_out)
  M->>PIN: g_signal_connect("new-sample", on_new_sample)
  M->>PIN: set_state(PLAYING)
  M->>POUT: set_state(PLAYING)
  M->>M: g_main_loop_run()

  loop fiecare cadru de la cameră
    PIN->>CB: buffer RGB 640x480 (scalat + flip)
    CB->>CB: pull_sample, make_writable, map READWRITE
    CB->>CB: invert octet cu octet (255 - v)
    CB->>POUT: gst_app_src_push_buffer(buf)
    POUT->>POUT: encode H264 (zerolatency) → decode → afișare pe ecran
  end

  U->>M: Ctrl+C (sau --frames=N atins)
  M->>PIN: send_event(EOS)
  M->>POUT: gst_app_src_end_of_stream() + așteaptă EOS max 3 s (drain)
  M->>PIN: set_state(NULL)
  M->>POUT: set_state(NULL)
  M->>U: "Processed N frames in T s (F fps)"
```

## 3. Diagrama de clase — structurile și funcțiile C (`src/main.c`)

```mermaid
classDiagram
  class App {
    +loop: GMainLoop*
    +pipe_in: GstElement*
    +pipe_out: GstElement*
    +appsrc: GstElement*
    +proc: GstElement* (appsink)
    +frames: gint64
    +max_frames: gint64
    +t0: gint64
  }
  class main() {
    +parsează opțiuni (GOptionContext)
    +construiește desc_in / desc_out
    +gst_parse_launch() x2
    +set_state(PLAYING) x2
    +g_main_loop_run()
    +oprire gracioasă (EOS + drain 3 s)
  }
  class on_new_sample() {
    +pull_sample()
    +make_writable + map READWRITE
    +invert: 255 - octet
    +gst_app_src_push_buffer()
  }
  class on_bus() {
    +GST_MESSAGE_ERROR → quit
    +GST_MESSAGE_EOS → quit
  }
  class on_sigint() {
    +g_main_loop_quit()
  }
  class flip_method() {
    +horizontal→horizontal-flip, vertical, 180, none
  }

  main --> App : construiește
  main ..> on_new_sample : g_signal_connect("new-sample")
  main ..> on_bus : gst_bus_add_watch
  main ..> on_sigint : g_unix_signal_add
  main ..> flip_method : validează --flip
```
