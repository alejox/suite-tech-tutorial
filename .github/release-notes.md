Native OBS Studio plugin for live coding and tech tutorials · Plugin nativo de OBS Studio para live coding y tutoriales técnicos.

**Windows y macOS: compatible con OBS Studio 31, 32 y 33** · Windows 10/11 · macOS 12+ (Apple Silicon e Intel)

**Ubuntu:** el `.deb` se compila contra la versión de OBS del PPA oficial (`ppa:obsproject/obs-studio`); requiere esa versión o una más nueva. Solo sesiones X11.

### Incluye

- **Auto-Zoom y Smart Cursor** (filtro): zoom suave y automático donde haces clic o escribes (sigue el cursor de texto), seguimiento del ratón, ondas al hacer clic y foco en el cursor.
- **Visualizador de atajos de teclado** (fuente): muestra `⌘ ⇧ P` / `Ctrl + Shift + P` con un overlay moderno (4 temas). El texto normal nunca se muestra: tus contraseñas no salen en pantalla.

### Instalación

| Sistema | Archivo |
|---|---|
| Windows | `*-windows-x64.zip` → descomprimir en `C:\ProgramData\obs-studio\plugins\` (o el instalador `.exe` si está disponible) |
| macOS | `*-macos-universal.pkg` (o el `.tar.xz` → `~/Library/Application Support/obs-studio/plugins/`) |
| Ubuntu 24.04 | `*-x86_64.deb` |

**macOS:** concede a OBS los permisos de *Monitorización de entrada* (teclas y clics) y, opcionalmente, *Accesibilidad* (seguir el cursor de texto). Los paquetes de macOS no están firmados con un certificado de Apple ni notarizados: si macOS los bloquea, ábrelos con clic derecho → *Abrir*.

Guía completa en el [README](https://github.com/alejox/suite-tech-tutorial#readme).
