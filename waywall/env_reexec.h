#ifndef WAYWALL_ENV_REEXEC_H
#define WAYWALL_ENV_REEXEC_H

void env_passthrough_add_display(char **env_passthrough);
void env_passthrough_destroy(char **env_passthrough);
char **env_passthrough_get();
int env_reexec(char **argv);

#endif
