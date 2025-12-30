#pragma once

void env_passthrough_set(const char *name, const char *value);
void env_passthrough_unset(const char *name);

void env_passthrough_destroy();
char **env_passthrough_get();
int env_reexec(char **argv);
