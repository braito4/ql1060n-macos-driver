# Brother QL-1060N para macOS 27 (Apple Silicon)

Filtro CUPS nativo `arm64` que convierte el raster monocromo de macOS al modo
ráster documentado por Brother para la QL-1060N. No instala extensiones de
kernel. Funciona con la conexión USB gestionada por CUPS o por red mediante
JetDirect/RAW (puerto 9100).

## Compilar y probar

```sh
make test
```

## Instalar

La forma recomendada es abrir el instalador generado:

```sh
make installer
open build/QL-1060N-macOS27-arm64-0.5.2.pkg
```

Después, añade la impresora desde Ajustes del Sistema o crea la cola desde
Terminal con una de las opciones siguientes.

Si ya existe una cola llamada `Brother_QL_1060N`, el instalador la actualiza
automáticamente para que deje de usar el filtro Intel antiguo de Brother.

Impresora de red:

```sh
./install.sh --ip 192.168.1.50
```

Impresora USB conectada y encendida:

```sh
./install.sh --usb
```

Sin argumentos se instalan el filtro y el PPD, pero no se crea una cola.

## Alcance

- Resolución: 300 × 300 dpi, negro térmico.
- Rollos continuos: 12, 29, 38, 50, 54, 62 y 102 mm.
- Etiquetas precortadas Brother incluidas en la referencia oficial.
- Corte automático, espejo y prioridad de calidad.
- Detección automática por SNMP entre DC16 102 × 152 mm precortado y rollo
  continuo de 102 mm, manteniendo una página de 102 × 152 mm.
- El filtro usa compresión TIFF PackBits, requerida por Brother para LAN.

Este proyecto es una implementación independiente basada en la especificación
pública de comandos Brother; no contiene binarios ni código de Brother.

## Registro de diagnóstico

La versión de diagnóstico guarda metadatos del protocolo y las respuestas de
estado de la impresora en `/Library/Logs/QL1060N/driver.log`. No guarda el
contenido rasterizado de las etiquetas.
