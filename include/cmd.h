#ifndef WAYWALL_CMD_H
#define WAYWALL_CMD_H

int cmd_cpu();
int cmd_exec(char **argv);
int cmd_run(const char *profile);
int cmd_wrap(const char *profile, char **argv);

#endif
