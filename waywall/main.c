#include "server/server.h"
#include "util.h"

int
main() {
    struct server *server = server_create();
    if (!server) {
        return 1;
    }
    return 0;
}
