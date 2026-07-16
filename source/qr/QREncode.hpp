#pragma once
#include <cstddef>

int  QRGetWidth(int version);
int  QRGetDataWords(int version);
int  QRGetRemainder(int version);
int  QRGetECCBytes(int version, int level);
void QRGetEccSpec(int version, int level, int spec[5]);
void QRRSEncode(size_t dataLen, size_t eccLen,
                const unsigned char *data, unsigned char *ecc);

extern const unsigned int QRFormatInfo[4][8];
const int* QRGetAlignPattern(int version);
