#pragma once

struct zip;

void zip_close(struct zip *zip);
const char *zip_next(struct zip *zip);
struct zip *zip_open(const char *path);
