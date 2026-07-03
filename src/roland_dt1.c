#include "scgfx_internal.h"

uint8_t roland_checksum(const uint8_t addr[3], const uint8_t *data, int len) {
    int sum = addr[0] + addr[1] + addr[2];
    for (int i = 0; i < len; i++) sum += data[i];
    return (128 - (sum & 0x7f)) & 0x7f;
}

void emit_dt1(FILE *f, uint8_t offset, const uint8_t *data, int len, uint32_t delay) {
    const uint8_t addr[3] = { 0x10, 0x01, offset };
    uint8_t checksum = roland_checksum(addr, data, len);
    write_varlen(f, delay);
    fputc(0xF0, f); write_varlen(f, (uint32_t)(9 + len));
    fputc(0x41, f); fputc(0x10, f); fputc(0x45, f); fputc(0x12, f);
    fputc(addr[0], f); fputc(addr[1], f); fputc(addr[2], f);
    fwrite(data, 1, len, f);
    fputc(checksum, f); fputc(0xF7, f);
}

void emit_sc55_frame(FILE *f, const uint8_t fb[64], uint32_t delay) { emit_dt1(f, 0, fb, 64, delay); }

uint8_t *make_dt1_event_data(uint8_t offset, const uint8_t *data, int len, uint32_t *out_len) {
    const uint8_t addr[3] = { 0x10, 0x01, offset };
    uint8_t checksum = roland_checksum(addr, data, len);
    uint8_t varbuf[5]; int sysex_len = 9 + len; int varlen = encode_varlen_to_buf((uint32_t)sysex_len, varbuf);
    uint8_t *ev = (uint8_t *)malloc(1 + varlen + sysex_len);
    if (!ev) return NULL;
    int p = 0;
    ev[p++] = 0xF0; memcpy(ev + p, varbuf, varlen); p += varlen;
    ev[p++] = 0x41; ev[p++] = 0x10; ev[p++] = 0x45; ev[p++] = 0x12;
    ev[p++] = addr[0]; ev[p++] = addr[1]; ev[p++] = addr[2];
    memcpy(ev + p, data, len); p += len;
    ev[p++] = checksum; ev[p++] = 0xF7;
    *out_len = (uint32_t)p; return ev;
}

uint8_t *make_sc55_event_data(const uint8_t fb[64], uint32_t *out_len) { return make_dt1_event_data(0, fb, 64, out_len); }

uint8_t *make_dt1_event_data_addr(uint8_t a0, uint8_t a1, uint8_t a2,
                                    const uint8_t *payload, uint32_t payload_len,
                                    uint32_t *out_len) {
    uint8_t addr[3] = { a0, a1, a2 };
    uint8_t checksum = roland_checksum(addr, payload, (int)payload_len);
    uint32_t syx_len = 9 + payload_len;
    uint8_t lenbuf[5];
    int lenbytes = encode_varlen_to_buf(syx_len, lenbuf);
    uint32_t total = 1 + (uint32_t)lenbytes + syx_len;

    uint8_t *ev = (uint8_t *)malloc(total);
    if (!ev)
        return NULL;

    uint32_t p = 0;
    ev[p++] = 0xF0;
    memcpy(ev + p, lenbuf, lenbytes);
    p += lenbytes;

    ev[p++] = 0x41;
    ev[p++] = 0x10;
    ev[p++] = 0x45;
    ev[p++] = 0x12;

    ev[p++] = a0;
    ev[p++] = a1;
    ev[p++] = a2;

    memcpy(ev + p, payload, payload_len);
    p += payload_len;

    ev[p++] = checksum;
    ev[p++] = 0xF7;

    *out_len = p;
    return ev;
}

