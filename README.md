# Tech Tutorial Suite para OBS

Plugin **nativo (C++17)** para OBS Studio pensado para *live coding* y tutoriales técnicos. Lleva al directo dos efectos que normalmente se añaden en postproducción:

| Herramienta | Tipo en OBS | Qué hace |
|---|---|---|
| **Auto-Zoom y Smart Cursor** | Filtro de vídeo | Detecta dónde haces clic o escribes y aplica un zoom suave y automático (al estilo de Screen Studio), sigue al ratón mientras dura el zoom y se aleja cuando paras. Además añade ondas al hacer clic y un foco opcional alrededor del cursor. |
| **Visualizador de atajos de teclado** | Fuente | Muestra en pantalla los atajos que pulsas (`⌘ ⇧ P`, `Ctrl + Shift + P`...) con un overlay moderno en Vue. **Nunca muestra texto normal**, así que tus contraseñas no salen en el stream. |

Plataformas: **Windows 10/11** y **macOS 12+** son los objetivos principales. En Linux funciona en sesiones **X11** (en Wayland el sistema no permite la captura global de entrada).

---

## Instalación

Descarga el paquete de tu sistema desde *Releases* (lo generan las GitHub Actions del repositorio):

- **Windows**: descomprime el `.zip` en `C:\ProgramData\obs-studio\plugins\` o ejecuta el instalador.
- **macOS**: ejecuta el `.pkg`. También puedes copiar `obs-tech-tutorial-suite.plugin` a `~/Library/Application Support/obs-studio/plugins/`.
- **Linux**: instala el `.deb` o copia el `.so` a `~/.config/obs-studio/plugins/obs-tech-tutorial-suite/bin/64bit/` y la carpeta `data` a `.../obs-tech-tutorial-suite/data/`.

### Permisos en macOS

La primera vez que añadas una de las herramientas, macOS te pedirá permisos para OBS:

1. **Monitorización de entrada** (Ajustes del Sistema → Privacidad y seguridad): necesario para ver teclas y clics. Reinicia OBS después de concederlo.
2. **Accesibilidad**: opcional, permite que el zoom siga al **cursor de texto** mientras escribes. Sin este permiso, el zoom usa la posición del ratón.

En Windows no hace falta ningún permiso especial.

---

## Uso

### Auto-Zoom y Smart Cursor

1. Añade una **Captura de pantalla** con el monitor en el que programas.
2. Clic derecho → **Filtros** → **+** → **Auto-Zoom y Smart Cursor**.
3. En *Pantalla capturada* elige **el mismo monitor** que captura la fuente (o una región personalizada en píxeles si capturas solo una parte).
4. Pulsa **«Probar: zoom en el ratón ahora»** para comprobarlo.

| Ajuste | Por defecto | Descripción |
|---|---|---|
| Hacer zoom al hacer clic / al escribir | ✔ / ✔ | Qué activa el zoom. |
| Seguir el cursor de texto | ✔ | Al escribir, centra el zoom en el *caret* (Windows: caret del sistema; macOS: API de Accesibilidad. En VS Code y otras apps Electron se activa automáticamente). |
| Seguir al ratón durante el zoom | ✔ | Desplaza la cámara cuando el ratón se acerca al borde de la vista. |
| Nivel de zoom | 2.0x | 1.1x – 4x. |
| Alejar tras inactividad | 2 s | Tiempo sin clics ni teclas antes de volver a la vista completa. |
| Velocidad de animación | 4 | Muelles críticamente amortiguados: movimiento fluido y sin rebotes. |
| Margen antes de desplazar | 20 % | Cuánto puede acercarse el ratón al borde antes de que la cámara lo siga. |
| Ondas al hacer clic / Foco en el cursor | ✔ / ✘ | Smart Cursor, dibujado en la GPU dentro del mismo shader. |

**Atajos** (Ajustes → Atajos de teclado): *Auto-Zoom: activar / desactivar* y *Auto-Zoom: mantener para hacer zoom en el cursor*.

### Visualizador de atajos de teclado

1. En la escena, **+** → **Visualizador de atajos de teclado**.
2. Elige tema (*Cristal*, *Claro*, *Neón*, *Minimalista*), posición, color de acento, tamaño y tiempo en pantalla.
3. Pulsa **«Enviar un atajo de prueba»** para ver el resultado.

Si repites el mismo atajo (`⌘Z ⌘Z ⌘Z`), en lugar de apilarse aparece un contador `×3`.

**Atajo**: *Visualizador de atajos: pausar / reanudar captura*, útil antes de escribir algo sensible.

#### Cómo se protege tu privacidad

El filtro se aplica en el **código nativo**, antes de que nada salga del proceso de OBS, y está cubierto por tests:

- Letras, números, símbolos y espacio **solo** se muestran junto a `Ctrl` / `Cmd` / `Win` (o `Alt` si lo activas).
- `Shift + letra` es una mayúscula y **no** se muestra.
- **AltGr** (y `Option` en macOS) escribe caracteres como `@ # € [ ]`, así que siempre se trata como texto.
- Enter, Tab, Retroceso y las flechas sueltas están desactivados por defecto (no revelan nada, pero hacen ruido).
- La repetición automática del teclado se ignora.
- Los eventos solo se envían a la página del overlay que crea el propio plugin, nunca a otras fuentes de navegador.

---

## Arquitectura

```
                        ┌──────────────────────── proceso de OBS ─────────────────────────┐
 Teclado / ratón        │                                                                  │
 ──────────────▶ Hook   │  InputHook (singleton, se arranca solo si hay oyentes)           │
 Win:  WH_*_LL          │      │ eventos normalizados (ts_key_event / MouseEvent)          │
 Mac:  CGEventTap       │      ├──────────────▶ AutoZoomFilter ── zoom-logic (muelles) ──▶ │ shader GPU
 X11:  XRecord          │      │                    ▲ caret (Accesibilidad / GetGUIThreadInfo)
                        │      └──────────────▶ KeystrokeOverlay                           │
                        │                          │ shortcut-filter (descarta el texto)   │
                        │                          ▼ JSON                                  │
                        │                  browser_source privada ──▶ overlay Vue         │
                        │                  (proc "javascript_event")   (data/overlay)      │
                        └──────────────────────────────────────────────────────────────────┘
```

```
src/
  plugin-main.cpp              registro del módulo
  core/                        lógica pura, sin OBS ni SO (tests unitarios)
    shortcut-filter.*          ¿es un atajo o es texto? + JSON
    zoom-logic.*               cámara: muelles críticamente amortiguados, márgenes, límites
  input/
    input-hook.*               hook compartido, oyentes, hilo de seguimiento del caret
    platform-win.cpp           Windows: hooks de bajo nivel, GetGUIThreadInfo, EnumDisplayMonitors
    platform-mac.cpp           macOS: CGEventTap, AXUIElement, CGDisplayBounds
    platform-x11.cpp           Linux: XRecord, XRandR
  zoom/auto-zoom-filter.cpp    filtro de vídeo
  keystrokes/keystroke-source.cpp  fuente del visualizador
data/
  effects/auto-zoom.effect     shader (zoom + foco + ondas en una sola pasada)
  overlay/index.html           overlay compilado (un solo archivo, funciona sin internet)
  locale/                      en-US, es-ES
overlay/                       código fuente Vue 3 + Vite del overlay
tests/                         tests unitarios del núcleo (no necesitan OBS)
```

Decisiones de diseño:

- **Un único hook global** con recuento de oyentes: si no usas ninguna herramienta, no se captura nada.
- Los *callbacks* del hook solo copian datos y vuelven (en Windows, un hook lento se elimina; en macOS, el *tap* se desactiva). Todo el trabajo se hace en `video_tick`, en el hilo gráfico.
- El seguimiento del caret es caro (IPC de accesibilidad), así que corre en un hilo aparte, solo justo después de pulsar teclas y a ~14 Hz.
- Coordenadas normalizadas respecto al monitor: funciona igual con pantallas Retina o con escalado de Windows.
- El overlay se dibuja con una `browser_source` **privada** que el plugin crea y controla. La configuración se inyecta como variables CSS (`--ts-theme`, `--ts-accent`...) y los atajos llegan como eventos `techsuite:shortcut`.

---

## Compilar

Basado en el [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) oficial (CMake ≥ 3.28).

```bash
# macOS (Xcode 16)
cmake --preset macos && cmake --build --preset macos

# Windows (Visual Studio 2022)
cmake --preset windows-x64 && cmake --build --preset windows-x64

# Linux (Ubuntu 24.04: libobs-dev libx11-dev libxtst-dev libxrandr-dev)
cmake --preset ubuntu-x86_64 && cmake --build --preset ubuntu-x86_64
```

En macOS y Windows los presets descargan automáticamente las fuentes de OBS y sus dependencias (ver `buildspec.json`). Las GitHub Actions generan los paquetes de las tres plataformas en cada push a `main` y en cada PR; al crear un tag se publica una *release*.

### Tests

```bash
# Núcleo C++ (sin OBS)
cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests

# Overlay
cd overlay && npm ci && npm test
```

### Trabajar en el overlay

```bash
cd overlay
npm ci
npm run dev      # fuera de OBS muestra una demo en bucle
npm run build    # genera data/overlay/index.html (hay que hacer commit de ese archivo)
```

---

## Limitaciones conocidas

- **macOS**: las letras se nombran según su posición en un teclado ANSI (convención habitual en los atajos de macOS); en distribuciones como AZERTY, algunas teclas de puntuación pueden aparecer con otro símbolo.
- **Caret**: depende de que la aplicación lo exponga. Funciona en la mayoría de apps nativas, en VS Code y en navegadores; si no está disponible, se usa el ratón.
- **Wayland**: no hay captura global de entrada. El zoom sigue funcionando con el atajo «mantener para hacer zoom».
- El filtro de zoom asume que la fuente muestra el monitor completo (o la región configurada). Para *Captura de ventana* usa una región personalizada con las coordenadas de la ventana.

## Licencia

GPL-2.0, igual que OBS Studio.
