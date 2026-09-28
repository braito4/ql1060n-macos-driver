#include <cups/cups.h>
#include <cups/raster.h>
#include <cups/sidechannel.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HEAD_PINS 1296
#define HEAD_BYTES (HEAD_PINS / 8)
#define CONTINUOUS_MIN_ROWS 295
#define CONTINUOUS_MAX_ROWS 35433
#define DRIVER_VERSION "0.7.0"

typedef struct {
  const char *name;
  uint8_t width_mm;
  uint8_t length_mm;
  uint16_t left;
  uint16_t printable;
  int continuous;
} media_t;

typedef struct {
  int valid;
  uint8_t error1;
  uint8_t error2;
  uint8_t width_mm;
  uint8_t type;
  uint8_t length_mm;
  uint8_t status_type;
  uint8_t phase_type;
} printer_status_t;

static const media_t media_table[] = {
    {"DC01", 17, 54, 1087, 165, 0}, {"DC02", 17, 87, 1087, 165, 0},
    {"DC20", 23, 23, 976, 236, 0},  {"DC03", 29, 90, 940, 306, 0},
    {"DC04", 38, 90, 827, 413, 0},  {"DC17", 39, 48, 821, 425, 0},
    {"DC24", 52, 29, 674, 578, 0},  {"DC06", 62, 29, 544, 696, 0},
    {"DC07", 62, 100, 544, 696, 0}, {"DC15", 102, 51, 76, 1164, 0},
    /* Brother reports the nominal 102 x 152 mm DK roll as length 153. */
    {"DC16", 102, 153, 76, 1164, 0},
    {"DC103_164", 104, 164, 40, 1200, 0},
    {"DC12", 12, 12, 1046, 94, 0}, {"DC13", 24, 24, 975, 236, 0},
    {"DC05", 58, 58, 584, 618, 0},
    {"W12", 12, 0, 1116, 106, 1},  {"W29", 29, 0, 940, 306, 1},
    {"W38", 38, 0, 827, 413, 1},   {"W50", 50, 0, 686, 554, 1},
    {"W54", 54, 0, 662, 590, 1},   {"W62", 62, 0, 544, 696, 1},
    {"W102", 102, 0, 76, 1164, 1},
    {"W103", 104, 0, 40, 1200, 1},
};

/*
 * Model and media compatibility data was cross-checked against the public
 * pklaus/brother_ql project (GPL-3.0). This is an independent implementation
 * of Brother's published raster command reference; no brother_ql source code
 * is incorporated here.
 */

static volatile sig_atomic_t cancelled = 0;
static int trace_fd = -1;
static char trace_path[PATH_MAX];

static int parse_status_block(const uint8_t *data, size_t length,
                              printer_status_t *status);
static int status_error_summary(const printer_status_t *status, char *buffer,
                                size_t capacity);

static int trace_try_open(const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                0644);
  if (fd < 0)
    return -1;
  trace_fd = fd;
  snprintf(trace_path, sizeof(trace_path), "%s", path);
  (void)fchmod(trace_fd, 0644);
  return 0;
}

static void trace_open(void) {
  const char *cups_tmp = getenv("TMPDIR");
  char path[PATH_MAX];
  if (cups_tmp && strstr(cups_tmp, "/cups")) {
    snprintf(path, sizeof(path), "%s/ql1060n-driver.log", cups_tmp);
    if (!trace_try_open(path))
      return;
  }
  if (!trace_try_open("/private/var/spool/cups/tmp/ql1060n-driver.log"))
    return;
  (void)trace_try_open("/private/tmp/ql1060n-driver.log");
}

static void trace_log(const char *format, ...) {
  if (trace_fd < 0)
    return;
  char timestamp[32];
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
  dprintf(trace_fd, "[%s] ", timestamp);
  va_list args;
  va_start(args, format);
  vdprintf(trace_fd, format, args);
  va_end(args);
  dprintf(trace_fd, "\n");
}

static void trace_status(const char *label, double timeout) {
  uint8_t status[32];
  errno = 0;
  ssize_t n = cupsBackChannelRead((char *)status, sizeof(status), timeout);
  if (n <= 0) {
    trace_log("STATUS %s: sin respuesta n=%zd errno=%d (%s)", label, n,
              errno, strerror(errno));
    return;
  }
  char hex[32 * 3 + 1];
  size_t offset = 0;
  for (ssize_t i = 0; i < n && offset + 3 < sizeof(hex); i++)
    offset += (size_t)snprintf(hex + offset, sizeof(hex) - offset, "%02x%s",
                              status[i], i + 1 == n ? "" : " ");
  trace_log("STATUS %s: n=%zd bytes=%s", label, n, hex);
  printer_status_t parsed;
  if (!parse_status_block(status, (size_t)n, &parsed)) {
    char errors[256];
    int has_errors = status_error_summary(&parsed, errors, sizeof(errors));
    trace_log("STATUS decoded: error1=0x%02x error2=0x%02x media=%u mm "
              "type=0x%02x length=%u status=0x%02x phase=0x%02x errors=%s",
              parsed.error1, parsed.error2, parsed.width_mm, parsed.type,
              parsed.length_mm, parsed.status_type, parsed.phase_type, errors);
    if (has_errors)
      fprintf(stderr, "WARNING: Estado Brother: %s.\n", errors);
  }
}

static int parse_device_host(const char *uri, char *host, size_t capacity) {
  if (!uri || !capacity)
    return -1;
  const char *start = strstr(uri, "://");
  start = start ? start + 3 : uri;
  const char *end;
  if (*start == '[') {
    start++;
    end = strchr(start, ']');
  } else {
    end = start + strcspn(start, ":/");
  }
  size_t length = end ? (size_t)(end - start) : strlen(start);
  if (!length || length >= capacity)
    return -1;
  memcpy(host, start, length);
  host[length] = '\0';
  return 0;
}

static int device_uri_is_usb(const char *uri) {
  return uri && !strncasecmp(uri, "usb:", 4);
}

static int parse_status_block(const uint8_t *data, size_t length,
                              printer_status_t *status) {
  if (!data || !status)
    return -1;
  for (size_t i = 0; i + 32 <= length; i++) {
    if (data[i] != 0x80 || data[i + 1] != 0x20 || data[i + 2] != 0x42)
      continue;
    memset(status, 0, sizeof(*status));
    status->valid = 1;
    status->error1 = data[i + 8];
    status->error2 = data[i + 9];
    status->width_mm = data[i + 10];
    status->type = data[i + 11];
    status->length_mm = data[i + 17];
    status->status_type = data[i + 18];
    status->phase_type = data[i + 19];
    return 0;
  }
  return -1;
}

typedef struct {
  uint8_t mask;
  const char *description;
} status_error_t;

static void append_status_error(char *buffer, size_t capacity,
                                const char *description) {
  size_t used = strlen(buffer);
  if (used >= capacity - 1)
    return;
  (void)snprintf(buffer + used, capacity - used, "%s%s", used ? "; " : "",
                 description);
}

static int status_error_summary(const printer_status_t *status, char *buffer,
                                size_t capacity) {
  static const status_error_t error1[] = {
      {0x01, "sin material"},
      {0x02, "fin de etiqueta precortada"},
      {0x04, "atasco del cortador"},
      {0x10, "unidad principal en uso"},
      {0x80, "fallo del ventilador"},
  };
  static const status_error_t error2[] = {
      {0x01, "material incorrecto"},
      {0x02, "búfer de expansión lleno"},
      {0x04, "error de transmisión"},
      {0x08, "búfer de comunicación lleno"},
      {0x10, "tapa abierta"},
      {0x20, "trabajo cancelado"},
      {0x40, "no se puede alimentar el material"},
      {0x80, "error del sistema"},
  };
  if (!buffer || !capacity)
    return 0;
  buffer[0] = '\0';
  if (!status || !status->valid) {
    (void)snprintf(buffer, capacity, "estado no válido");
    return 0;
  }
  for (size_t i = 0; i < sizeof(error1) / sizeof(error1[0]); i++)
    if (status->error1 & error1[i].mask)
      append_status_error(buffer, capacity, error1[i].description);
  for (size_t i = 0; i < sizeof(error2) / sizeof(error2[0]); i++)
    if (status->error2 & error2[i].mask)
      append_status_error(buffer, capacity, error2[i].description);
  if (!buffer[0])
    (void)snprintf(buffer, capacity, "ninguno");
  return status->error1 || status->error2;
}

/* Read Brother's 32-byte printer status block through its private SNMP OID. */
static int query_network_status(const char *device_uri,
                                printer_status_t *status) {
  static const uint8_t request[] = {
      0x30, 0x2c, 0x02, 0x01, 0x00, 0x04, 0x06,
      'p',  'u',  'b',  'l',  'i',  'c',  0xa0, 0x1f, 0x02, 0x01, 0x01,
      0x02, 0x01, 0x00, 0x02, 0x01, 0x00, 0x30, 0x14, 0x30, 0x12,
      0x06, 0x0e, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x93, 0x03, 0x03,
      0x03, 0x09, 0x01, 0x06, 0x01, 0x00, 0x05, 0x00};
  char host[256];
  if (parse_device_host(device_uri, host, sizeof(host)))
    return -1;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *addresses = NULL;
  if (getaddrinfo(host, "161", &hints, &addresses))
    return -1;

  int result = -1;
  for (struct addrinfo *address = addresses; address; address = address->ai_next) {
    int fd = socket(address->ai_family, address->ai_socktype,
                    address->ai_protocol);
    if (fd < 0)
      continue;
    struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (!connect(fd, address->ai_addr, address->ai_addrlen) &&
        send(fd, request, sizeof(request), 0) == (ssize_t)sizeof(request)) {
      uint8_t response[1024];
      ssize_t received = recv(fd, response, sizeof(response), 0);
      if (received > 0 &&
          !parse_status_block(response, (size_t)received, status))
        result = 0;
    }
    close(fd);
    if (!result)
      break;
  }
  freeaddrinfo(addresses);
  return result;
}

static const media_t *resolve_media(const media_t *selected,
                                    const printer_status_t *status,
                                    int automatic, media_t *resolved) {
  if (!selected || !resolved)
    return selected;
  *resolved = *selected;
  int family_102 = selected->width_mm == 102 &&
                   (!strcmp(selected->name, "DC16") ||
                    !strcmp(selected->name, "W102"));
  int family_103 = selected->width_mm == 104 &&
                   (!strcmp(selected->name, "DC103_164") ||
                    !strcmp(selected->name, "W103"));
  if (!automatic || !status || !status->valid ||
      (!family_102 && !family_103))
    return resolved;
  if ((family_102 && status->width_mm && status->width_mm != 102) ||
      (family_103 && status->width_mm != 104))
    return resolved;

  if (status->type == 0x0a) {
    resolved->name = family_102 ? "AUTO-W102" : "AUTO-W103";
    resolved->continuous = 1;
    resolved->length_mm = 0;
  } else if (status->type == 0x0b &&
             (!family_103 || !status->length_mm || status->length_mm == 164)) {
    resolved->name = family_102 ? "AUTO-DC16" : "AUTO-DC103_164";
    resolved->continuous = 0;
    resolved->length_mm = family_102 ? 153 : 164;
  }
  return resolved;
}

static int should_validate_width(const media_t *selected,
                                 const media_t *resolved,
                                 const printer_status_t *status,
                                 int automatic) {
  if (!selected || !resolved || !automatic || resolved->width_mm != 102)
    return 1;
  if (!strcmp(selected->name, "W102") && resolved->continuous)
    return 0;
  return !(status && status->valid && status->width_mm == 0);
}

static void cancel_job(int sig) {
  (void)sig;
  cancelled = 1;
  trace_log("SIGNAL: trabajo cancelado");
}

static const char *option_value(const char *options, const char *name,
                                const char *fallback) {
  cups_option_t *parsed = NULL;
  int count = cupsParseOptions(options ? options : "", 0, &parsed);
  const char *value = cupsGetOption(name, count, parsed);
  static char result[128];
  snprintf(result, sizeof(result), "%s", value ? value : fallback);
  cupsFreeOptions(count, parsed);
  return result;
}

static int option_is_on(const char *value) {
  return value && (!strcasecmp(value, "ON") || !strcasecmp(value, "TRUE") ||
                   !strcmp(value, "1") || !strcasecmp(value, "YES"));
}

static const media_t *find_media(const char *page_size) {
  if (!page_size)
    return NULL;
  for (size_t i = 0; i < sizeof(media_table) / sizeof(media_table[0]); i++)
    if (!strcmp(page_size, media_table[i].name))
      return &media_table[i];

  /* Aliases used by Brother's legacy PPD and by its preset page names. */
  char normalized[32];
  if (!strncmp(page_size, "C_", 2)) {
    const char *start = page_size + 2;
    size_t n = strcspn(start, "_");
    if (n < sizeof(normalized)) {
      memcpy(normalized, start, n);
      normalized[n] = '\0';
      return find_media(normalized);
    }
  }
  static const struct { const char *old_name; const char *new_name; } aliases[] = {
      {"12mm", "W12"}, {"29mm", "W29"}, {"38mm", "W38"},
      {"50mm", "W50"}, {"54mm", "W54"}, {"62mm", "W62"},
      {"102mm", "W102"}, {"103mm", "W103"},
  };
  for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++)
    if (!strcmp(page_size, aliases[i].old_name))
      return find_media(aliases[i].new_name);
  return NULL;
}

static int write_bytes(FILE *out, const void *data, size_t size) {
  return fwrite(data, 1, size, out) == size ? 0 : -1;
}

/* TIFF PackBits, bounded to a single 162-byte print-head row. */
static size_t packbits(const uint8_t *src, size_t length, uint8_t *dst,
                       size_t capacity) {
  size_t i = 0, o = 0;
  while (i < length) {
    size_t run = 1;
    while (i + run < length && run < 128 && src[i] == src[i + run])
      run++;
    if (run >= 2) {
      if (o + 2 > capacity)
        return 0;
      dst[o++] = (uint8_t)(1 - (int)run);
      dst[o++] = src[i];
      i += run;
      continue;
    }

    size_t literal = i++;
    while (i < length && i - literal < 128) {
      run = 1;
      while (i + run < length && run < 128 && src[i] == src[i + run])
        run++;
      if (run >= 2)
        break;
      i++;
    }
    size_t n = i - literal;
    if (o + 1 + n > capacity)
      return 0;
    dst[o++] = (uint8_t)(n - 1);
    memcpy(dst + o, src + literal, n);
    o += n;
  }
  return o;
}

static int make_raster_command(size_t packed_length, uint8_t command[3]) {
  /* Brother defines this as: 'g', a zero byte, then one length byte. */
  if (packed_length > UINT8_MAX)
    return -1;
  command[0] = 0x67;
  command[1] = 0x00;
  command[2] = (uint8_t)(packed_length & 0xff);
  return 0;
}

static int pixel_blackness(const uint8_t *row, unsigned x,
                           const cups_page_header2_t *h) {
  if (h->cupsBitsPerColor == 8 && h->cupsNumColors == 1) {
    int v = row[x];
    return h->cupsColorSpace == CUPS_CSPACE_K ? v : 255 - v;
  }
  if (h->cupsBitsPerColor == 1 && h->cupsNumColors == 1) {
    int bit = (row[x / 8] >> (7 - (x % 8))) & 1;
    if (h->cupsColorSpace == CUPS_CSPACE_K)
      return bit ? 255 : 0;
    return bit ? 0 : 255;
  }
  if (h->cupsBitsPerColor == 8 && h->cupsNumColors >= 3) {
    const uint8_t *p = row + x * h->cupsNumColors;
    int luminance = (54 * p[0] + 183 * p[1] + 19 * p[2]) / 256;
    return 255 - luminance;
  }
  return 0;
}

static int halftone_is_black(int black, unsigned x, unsigned y,
                             int ordered_dither) {
  static const uint8_t bayer[4][4] = {
      {0, 8, 2, 10},
      {12, 4, 14, 6},
      {3, 11, 1, 9},
      {15, 7, 13, 5},
  };
  if (black < 0)
    black = 0;
  if (black > 255)
    black = 255;
  int threshold = ordered_dither ? bayer[y & 3][x & 3] * 16 + 8 : 128;
  return black >= threshold;
}

static void make_head_row(const uint8_t *src, const cups_page_header2_t *h,
                          const media_t *media, unsigned y, int mirror,
                          int brightness, int contrast, int ordered_dither,
                          uint8_t out[HEAD_BYTES]) {
  memset(out, 0, HEAD_BYTES);
  unsigned width = h->cupsWidth < media->printable ? h->cupsWidth : media->printable;
  unsigned pad = (media->printable - width) / 2;
  for (unsigned x = 0; x < width; x++) {
    int black = pixel_blackness(src, x, h);
    black += brightness * 5;
    black = 128 + ((black - 128) * (100 + contrast * 4)) / 100;
    if (!halftone_is_black(black, x, y, ordered_dither))
      continue;
    /* QL raster rows are transmitted from the right side of the head. */
    unsigned active_x = mirror ? x + pad : media->printable - 1 - (x + pad);
    unsigned pin = media->left + active_x;
    if (pin < HEAD_PINS)
      out[pin / 8] |= (uint8_t)(0x80u >> (pin % 8));
  }
}

static int valid_page_length(const media_t *media, uint32_t rows) {
  return !media || !media->continuous ||
         (rows >= CONTINUOUS_MIN_ROWS && rows <= CONTINUOUS_MAX_ROWS);
}

static int emit_control(FILE *out, const media_t *media, uint32_t rows,
                        int first_page, int auto_cut, int cut_every,
                        int cut_at_end, int quality, int margin_dots,
                        int validate_width) {
  uint8_t info[13] = {0x1b, 0x69, 0x7a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  info[3] = 0x82; /* recovery + media kind */
  if (validate_width)
    info[3] |= 0x04;
  if (!media->continuous)
    info[3] |= 0x08;
  if (quality)
    info[3] |= 0x40;
  info[4] = media->continuous ? 0x0a : 0x0b;
  info[5] = media->width_mm;
  info[6] = media->length_mm;
  info[7] = (uint8_t)(rows & 0xff);
  info[8] = (uint8_t)((rows >> 8) & 0xff);
  info[9] = (uint8_t)((rows >> 16) & 0xff);
  info[10] = (uint8_t)((rows >> 24) & 0xff);
  info[11] = first_page ? 0 : 1;

  const uint8_t raster_mode[] = {0x1b, 0x69, 0x61, 0x01};
  const uint8_t mode[] = {0x1b, 0x69, 0x4d, auto_cut ? 0x40 : 0x00};
  const uint8_t cut[] = {0x1b, 0x69, 0x41, (uint8_t)cut_every};
  const uint8_t expanded[] = {0x1b, 0x69, 0x4b, cut_at_end ? 0x08 : 0x00};
  const uint8_t margin[] = {0x1b, 0x69, 0x64, (uint8_t)(margin_dots & 0xff),
                            (uint8_t)((margin_dots >> 8) & 0xff)};
  const uint8_t compression[] = {0x4d, 0x02};
  return write_bytes(out, raster_mode, sizeof(raster_mode)) ||
                 write_bytes(out, info, sizeof(info)) ||
                 write_bytes(out, mode, sizeof(mode)) ||
                 write_bytes(out, cut, sizeof(cut)) ||
                 write_bytes(out, expanded, sizeof(expanded)) ||
                 write_bytes(out, margin, sizeof(margin)) ||
                 write_bytes(out, compression, sizeof(compression))
             ? -1
             : 0;
}

static int copy_stream(FILE *src, FILE *dst) {
  uint8_t buffer[16384];
  rewind(src);
  while (!cancelled) {
    size_t n = fread(buffer, 1, sizeof(buffer), src);
    if (n && write_bytes(dst, buffer, n))
      return -1;
    if (n < sizeof(buffer))
      return ferror(src) ? -1 : 0;
  }
  return -1;
}

static int encode_page(cups_raster_t *ras, const cups_page_header2_t *h,
                       const media_t *media, const char *options, int page_no,
                       int validate_width, FILE *encoded) {
  uint8_t *input = malloc(h->cupsBytesPerLine);
  if (!input)
    return -1;

  int auto_cut = option_is_on(option_value(options, "BrAutoTapeCut", "ON"));
  int cut_every = atoi(option_value(options, "BrCutLabel", "1"));
  int cut_at_end = option_is_on(option_value(options, "BrCutAtEnd", "ON"));
  int mirror = option_is_on(option_value(options, "BrMirror", "OFF"));
  int ordered_dither =
      !strcasecmp(option_value(options, "BrHalftone", "Threshold"), "Ordered");
  int quality = strcmp(option_value(options, "BrPriority", "BrSpeed"), "BrQuality") == 0;
  int brightness = atoi(option_value(options, "BrBrightness", "0"));
  int contrast = atoi(option_value(options, "BrContrast", "0"));
  double margin_mm = atof(option_value(options, "BrMargin", "3.0"));
  int margin_dots = media->continuous ? (int)(margin_mm * 300.0 / 25.4 + 0.5) : 0;
  if (cut_every < 1) cut_every = 1;
  if (cut_every > 255) cut_every = 255;
  if (margin_dots < 0) margin_dots = 0;
  if (margin_dots > 1500) margin_dots = 1500;

  if (emit_control(encoded, media, h->cupsHeight, page_no == 1, auto_cut,
                   cut_every, cut_at_end, quality, margin_dots,
                   validate_width)) {
    free(input);
    return -1;
  }

  unsigned blank_rows = 0, packed_rows = 0;
  size_t packed_bytes = 0, packed_min = SIZE_MAX, packed_max = 0;
  trace_log("PAGE %d: media=%s type=%s protocol=%ux%u raster=%ux%u "
            "bpc=%u bpp=%u bytes_line=%u cut=%d mirror=%d halftone=%s "
            "margin_dots=%d",
            page_no, media->name, media->continuous ? "continuous" : "die-cut",
            media->width_mm, media->length_mm, h->cupsWidth, h->cupsHeight,
            h->cupsBitsPerColor, h->cupsBitsPerPixel, h->cupsBytesPerLine,
            auto_cut, mirror, ordered_dither ? "ordered" : "threshold",
            margin_dots);

  for (unsigned y = 0; y < h->cupsHeight && !cancelled; y++) {
    if (cupsRasterReadPixels(ras, input, h->cupsBytesPerLine) != h->cupsBytesPerLine) {
      fprintf(stderr, "ERROR: Datos raster incompletos en la fila %u.\n", y);
      free(input);
      return -1;
    }
    uint8_t head[HEAD_BYTES], packed[HEAD_BYTES + 4];
    make_head_row(input, h, media, y, mirror, brightness, contrast,
                  ordered_dither, head);
    int blank = 1;
    for (size_t i = 0; i < sizeof(head); i++)
      if (head[i]) { blank = 0; break; }
    if (blank) {
      blank_rows++;
      const uint8_t zero = 0x5a;
      if (write_bytes(encoded, &zero, 1)) { free(input); return -1; }
    } else {
      size_t n = packbits(head, sizeof(head), packed, sizeof(packed));
      uint8_t command[3];
      if (!n || make_raster_command(n, command)) {
        free(input);
        return -1;
      }
      packed_rows++;
      packed_bytes += n;
      if (n < packed_min) packed_min = n;
      if (n > packed_max) packed_max = n;
      if (write_bytes(encoded, command, sizeof(command)) ||
          write_bytes(encoded, packed, n)) { free(input); return -1; }
    }
  }
  trace_log("PAGE %d encoded: blank_rows=%u packed_rows=%u packed_bytes=%zu "
            "packed_min=%zu packed_max=%zu",
            page_no, blank_rows, packed_rows, packed_bytes,
            packed_rows ? packed_min : 0, packed_max);
  free(input);
  return cancelled ? -1 : 0;
}

#ifndef QL1060N_NO_MAIN
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--version")) {
    puts("rastertoql1060n " DRIVER_VERSION " (arm64 macOS 27, trace)");
    return 0;
  }
  if (argc == 3 && !strcmp(argv[1], "--probe")) {
    printer_status_t status;
    if (query_network_status(argv[2], &status)) {
      fprintf(stderr, "ERROR: No se pudo leer el estado SNMP de %s.\n", argv[2]);
      return 1;
    }
    char errors[256];
    (void)status_error_summary(&status, errors, sizeof(errors));
    printf("media_type=0x%02x width=%u length=%u error1=0x%02x "
           "error2=0x%02x status=0x%02x phase=0x%02x errors=%s\n",
           status.type, status.width_mm, status.length_mm, status.error1,
           status.error2, status.status_type, status.phase_type, errors);
    return 0;
  }
  if (argc < 6 || argc > 7) {
    fprintf(stderr, "ERROR: Uso CUPS: %s job user title copies options [file]\n", argv[0]);
    return 1;
  }
  signal(SIGTERM, cancel_job);
  signal(SIGINT, cancel_job);
  trace_open();
  if (trace_fd >= 0)
    trace_log("TRACE path=%s", trace_path);
  trace_log("START version=%s job=%s user=%s title=%s copies=%s uri=%s",
            DRIVER_VERSION, argv[1], argv[2], argv[3], argv[4],
            getenv("DEVICE_URI") ? getenv("DEVICE_URI") : "(none)");

  printer_status_t detected_status;
  memset(&detected_status, 0, sizeof(detected_status));
  int automatic_media =
      strcasecmp(option_value(argv[5], "BrMediaDetect", "Auto"), "Selected") != 0;
  const char *device_uri = getenv("DEVICE_URI");
  if (automatic_media && device_uri && !device_uri_is_usb(device_uri) &&
      !query_network_status(device_uri, &detected_status)) {
    trace_log("MEDIA-DETECT type=0x%02x width=%u length=%u error1=0x%02x "
              "error2=0x%02x status=0x%02x phase=0x%02x",
              detected_status.type, detected_status.width_mm,
              detected_status.length_mm, detected_status.error1,
              detected_status.error2, detected_status.status_type,
              detected_status.phase_type);
    char errors[256];
    if (status_error_summary(&detected_status, errors, sizeof(errors))) {
      trace_log("MEDIA-DETECT errors=%s", errors);
      fprintf(stderr, "WARNING: Estado Brother: %s.\n", errors);
    }
  } else if (automatic_media && device_uri && device_uri_is_usb(device_uri)) {
    trace_log("MEDIA-DETECT skipped for USB device");
  } else if (automatic_media) {
    trace_log("MEDIA-DETECT unavailable; using selected PageSize");
  }

  int fd = argc == 7 ? open(argv[6], O_RDONLY) : STDIN_FILENO;
  if (fd < 0) {
    fprintf(stderr, "ERROR: No se puede abrir la entrada: %s\n", strerror(errno));
    return 1;
  }
  cups_raster_t *ras = cupsRasterOpen(fd, CUPS_RASTER_READ);
  if (!ras) {
    fprintf(stderr, "ERROR: No se puede abrir el flujo CUPS raster.\n");
    if (fd != STDIN_FILENO) close(fd);
    return 1;
  }

  uint8_t reset[202] = {0};
  reset[200] = 0x1b; reset[201] = 0x40;
  if (write_bytes(stdout, reset, sizeof(reset))) return 1;
  const uint8_t status_request[] = {0x1b, 0x69, 0x53};
  if (!write_bytes(stdout, status_request, sizeof(status_request))) {
    fflush(stdout);
    trace_status("before-job", 3.0);
  }

  cups_page_header2_t h;
  FILE *pending = NULL;
  int page_no = 0, result = 0;
  while (!cancelled && cupsRasterReadHeader2(ras, &h)) {
    page_no++;
    if (pending) {
      if (copy_stream(pending, stdout) || fputc(0x0c, stdout) == EOF) result = 1;
      fclose(pending); pending = NULL;
      if (result) break;
    }
    const char *page_size = option_value(argv[5], "PageSize", h.cupsPageSizeName);
    const media_t *selected_media = find_media(page_size);
    if (!selected_media) {
      fprintf(stderr, "ERROR: Tamaño de etiqueta no compatible: %s\n", page_size);
      result = 1; break;
    }
    media_t resolved_media;
    const media_t *media = resolve_media(selected_media, &detected_status,
                                         automatic_media, &resolved_media);
    int validate_width = should_validate_width(
        selected_media, media, &detected_status, automatic_media);
    trace_log("MEDIA-RESOLVE selected=%s resolved=%s protocol=%s %ux%u "
              "validate-width=%s",
              selected_media->name, media->name,
              media->continuous ? "continuous" : "die-cut", media->width_mm,
              media->length_mm, validate_width ? "yes" : "no");
    if (h.cupsWidth > media->printable + 8) {
      fprintf(stderr, "ERROR: Raster demasiado ancho (%u > %u puntos).\n",
              h.cupsWidth, media->printable);
      result = 1; break;
    }
    if (!valid_page_length(media, h.cupsHeight)) {
      fprintf(stderr,
              "ERROR: Longitud continua no compatible: %u filas; el modelo "
              "admite de %u a %u filas (aprox. 25 a 3000 mm).\n",
              h.cupsHeight, CONTINUOUS_MIN_ROWS, CONTINUOUS_MAX_ROWS);
      result = 1;
      break;
    }
    fprintf(stderr, "INFO: Página %d, %s, %ux%u píxeles.\n",
            page_no, page_size, h.cupsWidth, h.cupsHeight);
    pending = tmpfile();
    if (!pending || encode_page(ras, &h, media, argv[5], page_no,
                                validate_width, pending)) {
      fprintf(stderr, "ERROR: No se pudo convertir la página %d.\n", page_no);
      result = 1; break;
    }
  }
  if (!result && pending) {
    if (copy_stream(pending, stdout) || fputc(0x1a, stdout) == EOF) result = 1;
  }
  fflush(stdout);
  if (!result && page_no > 0)
    trace_status("after-job", 5.0);
  if (pending) fclose(pending);
  cupsRasterClose(ras);
  if (fd != STDIN_FILENO) close(fd);
  if (!page_no && !result) {
    fprintf(stderr, "ERROR: El trabajo no contiene páginas.\n");
    result = 1;
  }
  trace_log("END job=%s pages=%d result=%d cancelled=%d", argv[1], page_no,
            result, cancelled ? 1 : 0);
  if (trace_fd >= 0) close(trace_fd);
  return cancelled ? 1 : result;
}
#endif
