#pragma once

#include "server/ui.h"
#include "server/wl_seat.h"

void server_view_send_click(struct server_view *view);
void server_view_send_keys(struct server_view *view, size_t num_keys,
                           const struct syn_key keys[static num_keys]);
