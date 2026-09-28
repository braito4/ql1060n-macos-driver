#define QL1060N_NO_MAIN
#include "../src/rastertoql1060n.c"
#include <assert.h>

static size_t unpack(const uint8_t *src, size_t n, uint8_t *out, size_t cap) {
  size_t i = 0, o = 0;
  while (i < n) {
    int8_t control = (int8_t)src[i++];
    if (control >= 0) {
      size_t count = (size_t)control + 1;
      assert(i + count <= n && o + count <= cap);
      memcpy(out + o, src + i, count); i += count; o += count;
    } else if (control != -128) {
      size_t count = (size_t)(1 - control);
      assert(i < n && o + count <= cap);
      memset(out + o, src[i++], count); o += count;
    }
  }
  return o;
}

int main(void) {
  uint8_t source[HEAD_BYTES], packed[HEAD_BYTES + 4], restored[HEAD_BYTES];
  memset(source, 0, sizeof(source));
  source[80] = 0xaa; source[81] = 0x55; source[161] = 0xff;
  size_t n = packbits(source, sizeof(source), packed, sizeof(packed));
  assert(n > 0 && n < sizeof(source));
  assert(unpack(packed, n, restored, sizeof(restored)) == sizeof(source));
  assert(!memcmp(source, restored, sizeof(source)));
  assert(find_media("DC15")->printable == 1164);
  assert(find_media("W102")->continuous == 1);
  assert(find_media("102mm")->continuous == 1);
  assert(find_media("C_DC03_01")->width_mm == 29);
  assert(find_media("DC16")->length_mm == 153);
  assert(find_media("no-existe") == NULL);
  assert(device_uri_is_usb("usb://Brother/QL-1050"));
  assert(device_uri_is_usb("USB://Brother/QL-1060N"));
  assert(!device_uri_is_usb("socket://192.0.2.10:9100"));
  assert(!device_uri_is_usb(NULL));
  uint8_t command[3];
  make_raster_command(8, command);
  assert(command[0] == 0x67 && command[1] == 0x00 && command[2] == 0x08);
  make_raster_command(162, command);
  assert(command[0] == 0x67 && command[1] == 0x00 && command[2] == 0xa2);
  make_raster_command(0x1234, command);
  assert(command[0] == 0x67 && command[1] == 0x12 && command[2] == 0x34);

  const uint8_t status_bytes[32] = {
      0x80, 0x20, 0x42, 0x34, 0x34, 0x30, 0x00, 0x00,
      0x00, 0x01, 0x00, 0x0a, 0x00, 0x00, 0x27, 0x00,
      0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  printer_status_t status;
  assert(parse_status_block(status_bytes, sizeof(status_bytes), &status) == 0);
  assert(status.valid && status.type == 0x0a && status.width_mm == 0);
  assert(status.error2 == 0x01 && status.status_type == 0x02);
  media_t resolved;
  const media_t *auto_media =
      resolve_media(find_media("DC16"), &status, 1, &resolved);
  assert(auto_media->continuous == 1 && auto_media->width_mm == 102);
  assert(auto_media->length_mm == 0 && !strcmp(auto_media->name, "AUTO-W102"));

  cups_page_header2_t header;
  memset(&header, 0, sizeof(header));
  header.cupsWidth = 8;
  header.cupsBitsPerColor = 1;
  header.cupsBitsPerPixel = 1;
  header.cupsNumColors = 1;
  header.cupsColorSpace = CUPS_CSPACE_K;
  const media_t tiny = {"tiny", 1, 0, 0, 8, 1};
  const uint8_t left_pixel[] = {0x80};
  uint8_t head[HEAD_BYTES];
  make_head_row(left_pixel, &header, &tiny, 0, 0, 0, head);
  assert(head[0] == 0x01);
  make_head_row(left_pixel, &header, &tiny, 1, 0, 0, head);
  assert(head[0] == 0x80);

  FILE *control = tmpfile();
  assert(control);
  assert(emit_control(control, auto_media, 1660, 1, 1, 1, 1, 0, 35, 0) == 0);
  rewind(control);
  uint8_t control_bytes[32];
  size_t control_length = fread(control_bytes, 1, sizeof(control_bytes), control);
  fclose(control);
  assert(control_length == sizeof(control_bytes));
  assert(control_bytes[4] == 0x1b && control_bytes[5] == 0x69 &&
         control_bytes[6] == 0x7a);
  assert(control_bytes[7] == 0x82 && control_bytes[8] == 0x0a &&
         control_bytes[9] == 102 && control_bytes[10] == 0);

  const media_t *auto_from_w102 =
      resolve_media(find_media("W102"), &status, 1, &resolved);
  assert(auto_from_w102->continuous == 1 && auto_from_w102->length_mm == 0);
  assert(!should_validate_width(find_media("DC16"), auto_media, &status, 1));
  assert(!should_validate_width(find_media("W102"), auto_from_w102, NULL, 1));
  assert(should_validate_width(find_media("DC16"), find_media("DC16"), NULL,
                               1));
  assert(should_validate_width(find_media("W102"), auto_from_w102, &status,
                               0));

  printer_status_t diecut_status = status;
  diecut_status.error2 = 0;
  diecut_status.width_mm = 102;
  diecut_status.type = 0x0b;
  diecut_status.length_mm = 153;
  const media_t *auto_diecut =
      resolve_media(find_media("W102"), &diecut_status, 1, &resolved);
  assert(!auto_diecut->continuous && auto_diecut->length_mm == 153);
  assert(!strcmp(auto_diecut->name, "AUTO-DC16"));
  assert(should_validate_width(find_media("W102"), auto_diecut,
                               &diecut_status, 1));
  puts("OK");
  return 0;
}
