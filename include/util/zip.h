#ifndef WAYWALL_UTIL_ZIP_H
#define WAYWALL_UTIL_ZIP_H

struct zip;

void zip_close(struct zip *zip);
const char *zip_next(struct zip *zip);
struct zip *zip_open(const char *path);

#endif
