#include <stdbool.h>

struct compositor *compositor_create();
void compositor_destroy(struct compositor *compositor);
bool compositor_run(struct compositor *compositor);
