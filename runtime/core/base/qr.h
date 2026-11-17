// A QR code of a short text (qr.c): the menu shows the address a phone opens to become a controller
// (input/phonepad.h) as one, so nobody types an IP address on a phone.
#pragma once
#include <stdint.h>

enum { QR_MAX_SIZE = 37, QR_MAX_TEXT = 106 };   // version 5: 37 modules a side, 106 bytes at error correction L

// The QR code of text (its bytes as they are: UTF-8 or ASCII) into modules, row by row, 1 a dark module. mask: -1
// picks the mask pattern with the lowest penalty, as a printed code has; 0-7 forces one (the test compares each with
// another encoder's). Returns the side in modules (21 to QR_MAX_SIZE), 0 when the text is longer than QR_MAX_TEXT.
// Callers draw a light margin of four modules around it.
int qr_encode(const char *text, int mask, uint8_t modules[QR_MAX_SIZE * QR_MAX_SIZE]);
