#ifndef _env_h_
#define _env_h_

struct env;

struct env *env_create(const char *file);
void env_release(struct env *);

void env_setint(struct env * env, const char *key, int val);
int env_getint(struct env * env, const char *key);

void env_setstr(struct env * env, const char *key, const char *val);
const char *env_getstr(struct env * env, const char *key);

#endif // _env_h_
