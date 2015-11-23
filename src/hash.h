#ifndef _hash_h_
#define _hash_h_

struct hash;
struct hash *hash_new(int cap);
void hash_free(struct hash *h);
int hash_insert(struct hash *h, const char *key, int size);
int hash_exist(struct hash *h, const char *key, int size);
void hash_remove(struct hash *h, int pos);
int hash_size(struct hash *h);
int hash_cap(struct hash *h);

#endif // _hash_h_
