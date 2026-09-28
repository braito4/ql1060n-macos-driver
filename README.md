# Brother QL-1050 / QL-1060N para macOS 27 (Apple Silicon)

Filtro CUPS nativo `arm64` que convierte el raster monocromo de macOS al modo
ráster documentado por Brother para la QL-1050 y la QL-1060N. No instala
extensiones de kernel. Funciona con la conexión USB gestionada por CUPS o por
red mediante JetDirect/RAW (puerto 9100); la QL-1050 solo tiene USB.

## Compilar y probar

```sh
make test
```

## Instalar

La forma recomendada es abrir el instalador generado:

```sh
make installer
open build/QL-1050-QL-1060N-macOS27-arm64-0.7.0.pkg
```

Después, añade la impresora desde Ajustes del Sistema o crea la cola desde
Terminal con una de las opciones siguientes.

Si ya existe una cola llamada `Brother_QL_1060N` o `Brother_QL_1050`, el
instalador la actualiza automáticamente para que deje de usar el filtro Intel
antiguo de Brother.

Impresora de red (solo QL-1060N):

```sh
./install.sh --ip 192.168.1.50
```

Impresora USB conectada y encendida (QL-1050 o QL-1060N):

```sh
./install.sh --usb
```

Sin argumentos se instalan el filtro y el PPD, pero no se crea una cola.

## Alcance

- Resolución: 300 × 300 dpi, negro térmico.
- Rollos continuos: 12, 29, 38, 50, 54, 62, 102 y 103 mm.
- Etiquetas precortadas Brother incluidas en la referencia oficial, incluida
  la DK-11247 de 103 × 164 mm.
- Corte automático, espejo, prioridad de calidad y tramado ordenado opcional
  para fotografías y gráficos. El umbral nítido para texto y códigos sigue
  siendo el valor predeterminado.
- Detección automática por SNMP (QL-1060N en red) entre DC16 102 × 152 mm
  precortado y rollo continuo de 102 mm, y entre 103 × 164 mm y continuo de
  103 mm.
- El filtro usa compresión TIFF PackBits, requerida por Brother para LAN.
- En material continuo valida el intervalo documentado por Brother: de 295 a
  35 433 filas (aproximadamente de 25 mm a 3 m a 300 dpi).

Este proyecto es una implementación independiente basada en la especificación
pública de comandos Brother; no contiene binarios ni código de Brother.

macOS usa su icono genérico de impresora; el paquete no redistribuye los
iconos propietarios del controlador antiguo de Brother.

## Registro de diagnóstico

La versión de diagnóstico guarda metadatos del protocolo y las respuestas de
estado de la impresora en `/Library/Logs/QL1060N/driver.log`. No guarda el
contenido rasterizado de las etiquetas. Los bits de error se traducen a texto
legible, por ejemplo «material incorrecto», «tapa abierta» o «atasco del
cortador».

## Agradecimientos y atribución

La verificación del catálogo de rollos, los márgenes del cabezal y los límites
de longitud se benefició de la referencia pública
[`pklaus/brother_ql`](https://github.com/pklaus/brother_ql), creada por Philipp
Klaus y sus colaboradores y publicada bajo GPL-3.0. Gracias por documentar y
mantener la compatibilidad de la familia Brother QL.

Este controlador implementa de forma independiente la especificación pública
de comandos ráster de Brother. No incorpora ni redistribuye código fuente de
`brother_ql`.
